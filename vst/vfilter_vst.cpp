/* =============================================================================
 * vfilter_vst.cpp - V-Filter: a "towards the Virus" filter as a VST2 insert effect for the MPC OS
 * plugin host (Force, MPC Live/One/X/Key), armhf. The DSP is vfilter_core.h: pre-emphasis,
 * filter 1, drive + saturation, filter 2, de-emphasis, output warmth, stereo chorus, and an
 * audio-triggered attack/decay envelope on the cutoff (an insert gets no notes). This file is
 * the plug-in around it, hand-written like mpc-vst-rat's rat_vst.cpp (no wrapper, no SDK).
 * MIT license (see ../LICENSE). No affiliation with Access Music or Waldorf.
 * ========================================================================== */
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "vfilter_core.h"

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10, kVstTimeSigValid = 1 << 13 };
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5 };



struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::atomic<float> cache[NPARAMS];
    std::atomic<int> notify[NPARAMS];
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    std::atomic<bool> dirty{true};
    VirusStyleFilter dsp;
    VirusStyleFilter::Params p;
    float sr = 44100;
    std::vector<uint8_t> chunk;
};

static int IDX_CUTOFF, IDX_RESO, IDX_F2, IDX_DRIVE, IDX_SAT, IDX_EMPH, IDX_ENVAMT, IDX_ATTACK,
           IDX_DECAY, IDX_THRESH, IDX_WARMTH, IDX_WIDTH, IDX_OUTPUT;

static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
static int ui(Plugin *w, int i) { return i < 0 ? 0 : norm_to_ui(&PARAMS[i], w->cache[i].load()); }
static void set_ui(Plugin *w, int i, int v) {
    if (i < 0) return;
    w->cache[i].store(ui_to_norm(&PARAMS[i], v));
    w->notify[i].store(1);
}

struct NoDenormals {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t old = 0;
    NoDenormals() { asm volatile("vmrs %0, fpscr" : "=r"(old)); asm volatile("vmsr fpscr, %0" : : "r"(old | (1u << 24))); }
    ~NoDenormals() { asm volatile("vmsr fpscr, %0" : : "r"(old)); }
#elif defined(__x86_64__) || defined(__i386__)
    unsigned old = __builtin_ia32_stmxcsr();
    NoDenormals() { __builtin_ia32_ldmxcsr(old | 0x8040); }
    ~NoDenormals() { __builtin_ia32_ldmxcsr(old); }
#endif
};

/* knob positions -> real values. CUTOFF, ATTACK and DECAY are exponential and read the knob's
 * full resolution (the 0..100 the host shows is only the readout); the rest are the shown values. */
static float nrm(Plugin *w, int i) { return i < 0 ? 0.0f : w->cache[i].load(); }
static float cutoff_hz(float n) { return 20.0f * std::pow(900.0f, n); }      /* 20 Hz .. 18 kHz */
static float attack_ms(float n) { return 0.5f * std::pow(400.0f, n); }       /* 0.5 .. 200 ms */
static float decay_ms(float n) { return 10.0f * std::pow(400.0f, n); }       /* 10 ms .. 4 s */

static void configure(Plugin *w) {
    VirusStyleFilter::Params &p = w->p;
    p.cutoffHz = cutoff_hz(nrm(w, IDX_CUTOFF));
    p.resonance = ui(w, IDX_RESO) / 100.0f;
    p.filter2Semi = (float)ui(w, IDX_F2);
    p.driveDb = (float)ui(w, IDX_DRIVE);
    p.satType = clampi(ui(w, IDX_SAT), 0, 3);
    p.emphasis = ui(w, IDX_EMPH) / 100.0f;
    p.envAmount = ui(w, IDX_ENVAMT) / 12.0f;          /* semitones -> octaves */
    p.envAttackMs = attack_ms(nrm(w, IDX_ATTACK));
    p.envDecayMs = decay_ms(nrm(w, IDX_DECAY));
    p.envThreshDb = (float)ui(w, IDX_THRESH);
    p.warmth = ui(w, IDX_WARMTH) / 100.0f;
    p.width = ui(w, IDX_WIDTH) / 100.0f;
    p.outputDb = (float)ui(w, IDX_OUTPUT);
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    NoDenormals nd;
    if (w->dirty.exchange(false)) configure(w);
    if (n > 0) w->dsp.process(w->p, in[0], in[1], out[0], out[1], (uint32_t)n);
    for (int c = 0; c < 2; c++) {
        float *y = out[c];
        for (int i = 0; i < n; i++) {
            const float a = std::fabs(y[i]);                 /* output safety above -3 dBFS */
            if (a > 0.7f) {
                float t = std::min((a - 0.7f) / 0.3f, 3.0f), t2 = t * t;
                y[i] = std::copysign(0.7f + 0.29f * t * (27 + t2) / (27 + 9 * t2), y[i]);   /* never above 0.99 */
            }
        }
    }
    bool any = false;
    for (int i = 0; i < NPARAMS; i++) {
        if (w->release[i]) { w->release[i] = 0; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
        if (!w->notify[i].exchange(0)) continue;
        any = true;
        w->master(&w->fx, audioMasterAutomate, i, 0, 0, w->cache[i].load());
    }
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    bool nudge = false;
    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            n = (float)clampi((int)std::lround(cur) + (pos > cur ? 1 : -1), 0, p->nopts - 1) / (p->nopts - 1);
            nudge = true;
        }
    }
    w->cache[i].store(clamp01(n));
    w->dirty.store(true);
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    if (popup_is(i)) return w->open[i];
    return w->cache[i].load();
}

/* The chunk keeps each knob's full position (x 100000, as an integer: no decimal point, so no
 * locale trouble), not the rounded readout, so CUTOFF comes back exactly where it was. */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string t = "VFL1;";
    char buf[64];
    for (int i = 0; i < NPARAMS; i++) {
        std::snprintf(buf, sizeof buf, "%s=%ld;", PARAMS[i].key, std::lround(w->cache[i].load() * 100000.0f));
        t += buf;
    }
    w->chunk.assign(t.begin(), t.end());
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    std::string t((const char *)data, (size_t)len);
    if (t.compare(0, 5, "VFL1;")) return 0;
    for (size_t pos = 5; pos < t.size();) {
        size_t semi = t.find(';', pos);
        if (semi == std::string::npos) break;
        std::string kv = t.substr(pos, semi - pos);
        pos = semi + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        int i = param_index(kv.substr(0, eq).c_str());
        if (i < 0) continue;
        w->cache[i].store(clamp01(std::atol(kv.c_str() + eq + 1) / 100000.0f));
        w->notify[i].store(1);
    }
    w->dirty.store(true);
    return 1;
}

static void format_value(Plugin *w, int idx, int u, float n, char *buf, size_t size) {
    const param_t *pp = &PARAMS[idx];
    if (pp->nopts) std::snprintf(buf, size, "%s", pp->opts[u]);
    else if (idx == IDX_CUTOFF) {
        const float hz = cutoff_hz(n);
        if (hz < 1000) std::snprintf(buf, size, "%d Hz", (int)std::lround(hz));
        else std::snprintf(buf, size, "%.1f kHz", hz / 1000.0f);
    }
    else if (idx == IDX_ATTACK) std::snprintf(buf, size, "%.1f ms", attack_ms(n));
    else if (idx == IDX_DECAY) std::snprintf(buf, size, "%d ms", (int)std::lround(decay_ms(n)));
    else if (idx == IDX_F2 || idx == IDX_ENVAMT) std::snprintf(buf, size, "%+d st", u);
    else if (idx == IDX_OUTPUT) std::snprintf(buf, size, "%+d dB", u);
    else if (idx == IDX_DRIVE || idx == IDX_THRESH) std::snprintf(buf, size, "%d dB", u);
    else std::snprintf(buf, size, "%d", u);
    (void)w;
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: delete w; return 1;
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS;
    case effGetParamName: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        const bool pop = popup_is(idx);
        const float n = pop ? w->open[idx] : w->cache[idx].load();
        char buf[32];
        format_value(w, idx, norm_to_ui(&PARAMS[idx], n), n, buf, sizeof buf);
        copy_str(p, buf, 24);
        return 1;
    }
    case effSetSampleRate:
        if (o > 0) { w->sr = o; w->dsp.setSampleRate(o); w->dirty.store(true); }
        return 1;
    case effSetBlockSize: case effMainsChanged: return 1;
    case effCanDo: return -1;
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

static void set_norm(Plugin *w, int i, float n) { if (i >= 0) { w->cache[i].store(clamp01(n)); w->notify[i].store(1); } }
static void start_values(Plugin *w) {
    set_norm(w, IDX_CUTOFF, 0.677f);   /* 2 kHz */
    set_ui(w, IDX_RESO, 30);
    set_ui(w, IDX_F2, 0);
    set_ui(w, IDX_DRIVE, 6);
    set_ui(w, IDX_SAT, 1);             /* SOFT */
    set_ui(w, IDX_EMPH, 30);
    set_ui(w, IDX_ENVAMT, 0);          /* envelope off */
    set_norm(w, IDX_ATTACK, 0.231f);   /* 2 ms */
    set_norm(w, IDX_DECAY, 0.537f);    /* 250 ms */
    set_ui(w, IDX_THRESH, -36);
    set_ui(w, IDX_WARMTH, 20);
    set_ui(w, IDX_WIDTH, 30);
    set_ui(w, IDX_OUTPUT, 0);
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] {
        IDX_CUTOFF = param_index("cutoff"); IDX_RESO = param_index("reso"); IDX_F2 = param_index("f2");
        IDX_DRIVE = param_index("drive"); IDX_SAT = param_index("sat"); IDX_EMPH = param_index("emph");
        IDX_ENVAMT = param_index("envamt"); IDX_ATTACK = param_index("attack"); IDX_DECAY = param_index("decay");
        IDX_THRESH = param_index("thresh"); IDX_WARMTH = param_index("warmth"); IDX_WIDTH = param_index("width");
        IDX_OUTPUT = param_index("output");
    });
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(0); }
    start_values(w);
    w->dsp.setSampleRate(w->sr);
    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    return e;
}
