/* =============================================================================
 * vfilter_vst.cpp - V-Filter: a "towards the Virus" filter as a VST2 insert effect for the MPC OS
 * plugin host (Force, MPC Live/One/X/Key), armhf. The DSP is vfilter_core.h: pre-emphasis,
 * filter 1, drive + saturation, filter 2, de-emphasis, output warmth, stereo chorus, and an
 * audio-triggered attack/decay envelope on the cutoff (an insert gets no notes). In front of it
 * sits an optional vowel/formant filter (A-E-I-O-U) moved by that envelope and its own LFO - and, since
 * 0.3.0, by a 16-step sequencer on the Force's beat grid (vowel and level per step, up to 1/64). Since 0.4.0
 * the sequencer also has a note per step and plays a synth track: MPC OS ignores a plugin's VST MIDI output,
 * so the notes go out through the plugin's own ALSA sequencer port "V-Filter" (the way of mpc-vst-acid's Acid
 * FX, which MPC detects without a restart); a track picks it as its MIDI input. Since 0.5.0 FLOW makes the
 * line endless: a slow wave in time with the song is the middle of a corridor, each step picks a note near it
 * (chord tones and small steps preferred), the levels follow - never the same twice, never chaotic. This file is
 * the plug-in around it, hand-written like mpc-vst-rat's rat_vst.cpp (no wrapper, no SDK).
 * MIT license (see ../LICENSE). No affiliation with Access Music or Waldorf.
 * ========================================================================== */
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "vfilter_core.h"
#ifndef NO_ALSA
#include <alsa/asoundlib.h>
#endif

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
    bool down[NPARAMS] = {false};   /* the trigger buttons (RANDOM, RESET): act on the press only */
    double free_beat = 0;           /* the sequencer's own clock while the Force stands */
    std::atomic<bool> has_pend0{false};   /* parameter 0 (CUTOFF) from the host, held until the next block or read */
    float pend0 = 0, pend0_from = 0;
    /* the note lane: what is sounding and when it ends (audio thread only) */
    long long n_last_k = LLONG_MIN;   /* the last step boundary handled, LLONG_MIN = not running */
    int n_cur = -1, n_ch = 0;          /* the sounding note and its channel, -1 = none */
    double n_off = 0;                  /* the quarter its note-off is due, < 0 = held (slide) until the next step */
    long long s_last_k = LLONG_MIN;    /* FLOW: the last step boundary handled (also while MPC stands) */
    int f_prev = 0;                    /* FLOW: the last note it wrote (scale step) */
    float f_phase = 0, f_amp = 1;      /* FLOW: the wave's drift (phase offset) and its swing (0.55..1) */
    uint32_t frng = 0x85ebca6bu;
    bool need_update = false;          /* an UpdateDisplay to send, at most one per 50 ms */
    int update_wait = 0;
#ifndef NO_ALSA
    snd_seq_t *seq = nullptr;
#endif
    int seq_port = -1, port_slot = -1;
    uint32_t rng = 0x9e3779b9u;
};

static int IDX_CUTOFF, IDX_RESO, IDX_F2, IDX_DRIVE, IDX_SAT, IDX_EMPH, IDX_ENVAMT, IDX_ATTACK,
           IDX_DECAY, IDX_THRESH, IDX_WARMTH, IDX_WIDTH, IDX_OUTPUT,
           IDX_VOWEL, IDX_FORMANT, IDX_FOCUS, IDX_VMIX, IDX_VENV, IDX_LFORATE, IDX_LFODEPTH, IDX_LFOSHAPE,
           IDX_SEQRATE, IDX_SEQLEN, IDX_SEQDEPTH, IDX_SEQGATE, IDX_SEQCUT, IDX_SEQGLIDE, IDX_SEQRND, IDX_SEQINIT;
static int IDX_SV[16], IDX_SL[16];
static int IDX_NOTES, IDX_ROOT, IDX_SCALE, IDX_NRANGE, IDX_NLEN, IDX_NCH, IDX_NRND;
static int IDX_SN[16], IDX_ST[16];
static int IDX_FLOW, IDX_FSPEED, IDX_FHEIGHT, IDX_FWIDTH, IDX_FDRIFT, IDX_FDENS;
enum { T_REST, T_NOTE, T_ACC, T_SLIDE, T_ACC_SLD };
static bool is_trigger(int i) { return i >= 0 && (i == IDX_SEQRND || i == IDX_SEQINIT || i == IDX_NRND); }
/* SEQ RATE: step length in quarter notes (1/32 and 1/64 = two and four times the Force's 1/16 grid) */
static const double SEQ_BEATS[8] = {1.0, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125, 1.0 / 12, 0.0625};

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
static float lfo_hz(float n) { return 0.1f * std::pow(200.0f, n); }          /* 0.1 .. 20 Hz */

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
    p.vowel = nrm(w, IDX_VOWEL);                      /* the knob's full resolution: smooth morphing */
    p.formantSemi = (float)ui(w, IDX_FORMANT);
    p.focus = ui(w, IDX_FOCUS) / 100.0f;
    p.vowelMix = ui(w, IDX_VMIX) / 100.0f;
    p.vowelEnv = ui(w, IDX_VENV) / 100.0f;
    p.lfoRateHz = lfo_hz(nrm(w, IDX_LFORATE));
    p.lfoDepth = ui(w, IDX_LFODEPTH) / 100.0f;
    p.lfoShape = clampi(ui(w, IDX_LFOSHAPE), 0, 2);
    p.seqStepBeats = SEQ_BEATS[clampi(ui(w, IDX_SEQRATE), 0, 7)];
    p.seqLen = clampi(ui(w, IDX_SEQLEN), 1, 16);
    p.seqDepth = ui(w, IDX_SEQDEPTH) / 100.0f;
    p.seqGate = ui(w, IDX_SEQGATE) / 100.0f;
    p.seqCutSemi = (float)ui(w, IDX_SEQCUT);
    p.seqGlide = ui(w, IDX_SEQGLIDE) / 100.0f;
    for (int k = 0; k < 16; k++) {
        p.seqVowel[k] = nrm(w, IDX_SV[k]);            /* full resolution, like VOWEL */
        p.seqLevel[k] = nrm(w, IDX_SL[k]);
    }
}

/* ---- the sequencer's patterns ---------------------------------------------------------------- */
/* Formant Gate (Serum preset by ultramarinedist): its step LFO, 16 sixteenths, scaled to 0..1. A low step
 * is closed and dark (U, O), a high one open and bright (E, I); the level follows the step. */
static const float FG_STEPS[16] = {0.0f, 0.314f, 0.157f, 0.6f, 0.453f, 0.761f, 1.0f, 0.6f,
                                   0.375f, 0.686f, 0.307f, 0.471f, 0.211f, 0.782f, 0.386f, 0.678f};
static float vowel_of_step(float x) {   /* VOWEL positions: A 0, E 0.25, I 0.5, O 0.75, U 1 */
    return x < 0.2f ? 1.0f : x < 0.45f ? 0.75f : x < 0.7f ? 0.0f : x < 0.9f ? 0.25f : 0.5f;
}
static float level_of_step(float x) { return x < 0.08f ? 0.08f : 0.45f + 0.55f * x; }
static void set_norm(Plugin *w, int i, float n);
static void pattern_init(Plugin *w) {
    for (int k = 0; k < 16; k++) { set_norm(w, IDX_SV[k], vowel_of_step(FG_STEPS[k])); set_norm(w, IDX_SL[k], level_of_step(FG_STEPS[k])); }
}
static float rnd01(Plugin *w) { uint32_t x = w->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; w->rng = x; return (x >> 8) * (1.0f / 16777216.0f); }
static void pattern_random(Plugin *w) {
    /* a groove: vowels mostly on A E I O U (now and then in between), an accent on the one, a few rests */
    w->rng ^= (uint32_t)std::time(nullptr) * 2654435761u;
    for (int k = 0; k < 16; k++) {
        const float v = rnd01(w) < 0.8f ? (float)(int)(rnd01(w) * 5.0f) * 0.25f : rnd01(w);
        float l = 0.4f + 0.6f * rnd01(w);
        if (rnd01(w) < 0.18f) l = 0.05f;   /* a rest */
        set_norm(w, IDX_SV[k], v > 1.0f ? 1.0f : v);
        set_norm(w, IDX_SL[k], l);
    }
    set_norm(w, IDX_SL[0], 1.0f);
}

/* ---- the note lane ------------------------------------------------------------------------------- */
/* SCALE: a step's note is a scale step from ROOT (-14..+14), so every note is in key */
static const int SCALE_LEN[8] = {12, 7, 7, 7, 7, 7, 7, 5};
static const int SCALE_ST[8][12] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},   /* CHROM */
    {0, 2, 4, 5, 7, 9, 11},                   /* MAJOR */
    {0, 2, 3, 5, 7, 8, 10},                   /* MINOR (aeolian) */
    {0, 2, 3, 5, 7, 9, 10},                   /* DORIAN */
    {0, 1, 3, 5, 7, 8, 10},                   /* PHRYG */
    {0, 1, 4, 5, 7, 8, 10},                   /* GOA: phrygian dominant, the Goa/psytrance scale */
    {0, 2, 3, 5, 7, 8, 11},                   /* HARM MIN */
    {0, 3, 5, 7, 10},                         /* PENT MIN */
};
static int step_pitch(Plugin *w, int degree) {
    const int sc = clampi(ui(w, IDX_SCALE), 0, 7), L = SCALE_LEN[sc];
    int o = degree >= 0 ? degree / L : -((-degree + L - 1) / L);
    const int k = degree - o * L;
    return clampi(ui(w, IDX_ROOT) + 12 * o + SCALE_ST[sc][k], 0, 127);
}
static void set_step_type(Plugin *w, int k, int t) { set_ui(w, IDX_ST[k], t); }
/* RESET: the Formant Gate as in Serum - one held note: every sounding step slides (no new attack on the
 * same pitch), a rest where the level lane is closed */
static void notes_init(Plugin *w) {
    for (int k = 0; k < 16; k++) {
        set_ui(w, IDX_SN[k], 0);
        set_step_type(w, k, nrm(w, IDX_SL[k]) < 0.15f ? T_REST : T_SLIDE);
    }
}
/* RND NOTES: a Goa line in the scale - the root often, octave jumps, the rest within NOTE RANGE; rests where
 * the level lane is closed, accents and slides now and then */
static void notes_random(Plugin *w) {
    w->rng ^= (uint32_t)std::time(nullptr) * 40503u + 0x6a09e667u;
    const int L = SCALE_LEN[clampi(ui(w, IDX_SCALE), 0, 7)];
    const int span = std::min(14, ui(w, IDX_NRANGE) * L);
    for (int k = 0; k < 16; k++) {
        int d;
        const float r = rnd01(w);
        if (r < 0.42f) d = 0;
        else if (r < 0.57f) d = (rnd01(w) < 0.6f ? L : -L);
        else d = (int)std::lround(-span * 0.35f + rnd01(w) * span * 1.35f);
        d = clampi(d, -14, 14);
        while (d > span) d -= L;
        set_ui(w, IDX_SN[k], d);
        int t = T_NOTE;
        const float q = rnd01(w);
        if (q < 0.22f) t = T_ACC; else if (q < 0.38f) t = T_SLIDE; else if (q < 0.43f) t = T_ACC_SLD;
        if (nrm(w, IDX_SL[k]) < 0.15f || (k > 0 && rnd01(w) < 0.08f)) t = T_REST;
        set_step_type(w, k, t);
    }
    set_ui(w, IDX_SN[0], 0);
    if (ui(w, IDX_ST[0]) == T_REST) set_step_type(w, 0, T_ACC);
}

/* ---- the MIDI port --------------------------------------------------------------------------------- */
/* Port names by lowest free slot ("V-Filter", "V-Filter 2", ...), given back on close, so a re-created instance
 * comes back under the same name and the synth track listening on it keeps playing (as in Acid FX). */
static std::mutex g_port_lock;
static bool g_port_used[32];
static int port_take() { std::lock_guard<std::mutex> lk(g_port_lock); int n = 0; while (n < 31 && g_port_used[n]) n++; g_port_used[n] = true; return n; }
static void port_give(int n) { std::lock_guard<std::mutex> lk(g_port_lock); if (n >= 0 && n < 32) g_port_used[n] = false; }

#ifdef NO_ALSA
/* offline test build: the notes are recorded with the quarter they belong to */
struct DbgMidi { double beat; uint8_t st, d1, d2; };
static std::vector<DbgMidi> g_dbg;
extern "C" int vfilter_dbg_midi_count(void) { return (int)g_dbg.size(); }
extern "C" int vfilter_dbg_midi(int i, double *beat, int *st, int *d1, int *d2) {
    if (i < 0 || i >= (int)g_dbg.size()) return 0;
    *beat = g_dbg[i].beat; *st = g_dbg[i].st; *d1 = g_dbg[i].d1; *d2 = g_dbg[i].d2;
    return 1;
}
extern "C" void vfilter_dbg_midi_clear(void) { g_dbg.clear(); }
static void port_open(Plugin *w) { w->port_slot = port_take(); }
static void port_close(Plugin *w) { port_give(w->port_slot); w->port_slot = -1; }
static void midi_out(Plugin *w, double beat, uint8_t st, uint8_t d1, uint8_t d2) {
    (void)w;
    if (g_dbg.size() < 100000) g_dbg.push_back({beat, st, d1, d2});
}
#else
static void port_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK) < 0) { w->seq = nullptr; return; }
    snd_seq_set_client_pool_output(w->seq, 1024);   /* headroom so a slow subscriber never stalls the audio thread */
    w->port_slot = port_take();
    char name[32];
    if (w->port_slot == 0) std::snprintf(name, sizeof name, "%s", PLUG_NAME);
    else std::snprintf(name, sizeof name, "%s %d", PLUG_NAME, w->port_slot + 1);
    snd_seq_set_client_name(w->seq, name);
    w->seq_port = snd_seq_create_simple_port(w->seq, "MIDI Out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                             SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
}
static void midi_out(Plugin *w, double beat, uint8_t st, uint8_t d1, uint8_t d2) {
    (void)beat;
    if (!w->seq || w->seq_port < 0) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, w->seq_port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    const uint8_t type = st & 0xF0, ch = st & 0x0F;
    if (type == 0x90 && d2 > 0) snd_seq_ev_set_noteon(&ev, ch, d1, d2);
    else snd_seq_ev_set_noteoff(&ev, ch, d1, 0);
    snd_seq_event_output(w->seq, &ev);   /* buffered, non-blocking: drops instead of hanging */
    snd_seq_drain_output(w->seq);
}
static void port_close(Plugin *w) {
    if (w->seq) {
        for (int ch = 0; ch < 16; ch++) {   /* nothing may hang on the synth */
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_source(&ev, w->seq_port);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            snd_seq_ev_set_controller(&ev, ch, 123, 0);   /* all notes off */
            snd_seq_event_output_direct(w->seq, &ev);
        }
        snd_seq_close(w->seq);
        w->seq = nullptr;
    }
    port_give(w->port_slot);
    w->port_slot = -1;
}
#endif

static void note_off_now(Plugin *w, double beat) {
    if (w->n_cur >= 0) midi_out(w, beat, (uint8_t)(0x80 | w->n_ch), (uint8_t)w->n_cur, 0);
    w->n_cur = -1;
}
/* ---- FLOW: an endless line in a swinging corridor --------------------------------------------------- */
static float frnd(Plugin *w) { uint32_t x = w->frng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; w->frng = x; return (x >> 8) * (1.0f / 16777216.0f); }
/* the middle of the corridor at quarter b, in scale steps: a sine of SPEED bars, its phase and swing drifting */
static float flow_center(Plugin *w, double b) {
    const double bars = clampi(ui(w, IDX_FSPEED), 1, 16);
    const double ph = 2.0 * 3.14159265358979 * b / (4.0 * bars) + w->f_phase;
    return (float)(ui(w, IDX_FHEIGHT) * w->f_amp * std::sin(ph));
}
/* Writes step s (its note, trigger and level; the vowel stays) for the boundary at quarter b. Chosen like a
 * rope swung up and down: near the curve (WIDTH = how far off it), chord tones (root, third, fifth of the scale)
 * and small steps from the last note preferred, an octave jump now and then; rests from DENSITY, slides on small
 * steps, accents near the crests; the level opens with the curve. */
static void flow_step(Plugin *w, int s, double b) {
    const float drift = ui(w, IDX_FDRIFT) / 100.0f;
    w->f_phase += drift * 0.35f * (frnd(w) - 0.5f);                                   /* the wave never repeats exactly */
    w->f_amp = std::min(1.0f, std::max(0.55f, w->f_amp + drift * 0.12f * (frnd(w) - 0.5f)));
    const int sc = clampi(ui(w, IDX_SCALE), 0, 7), L = SCALE_LEN[sc];
    const float c = flow_center(w, b), width = (float)ui(w, IDX_FWIDTH);
    const int lo = std::max(-14, (int)std::floor(c - width - 1)), hi = std::min(14, (int)std::ceil(c + width + 1));
    float wsum = 0, wt[29];
    for (int d = lo; d <= hi; d++) {
        const float x = (d - c) / (width + 0.6f);
        float g = std::exp(-x * x);                                  /* near the curve */
        const int deg = ((d % L) + L) % L;
        const bool chord = L == 12 ? (deg == 0 || deg == 3 || deg == 4 || deg == 7) : L == 5 ? (deg == 0 || deg == 2 || deg == 3)
                                                                                         : (deg == 0 || deg == 2 || deg == 4);
        if (chord) g *= 2.2f;                                        /* harmonic */
        const int jump = std::abs(d - w->f_prev);
        g *= jump == 0 ? 0.8f : 1.0f / (1.0f + 0.45f * (jump - 1));  /* small steps, a held note now and then */
        wt[d - lo] = g; wsum += g;
    }
    int note = w->f_prev;
    if (wsum > 0) {
        float r = frnd(w) * wsum;
        for (int d = lo; d <= hi; d++) { r -= wt[d - lo]; if (r <= 0) { note = d; break; } }
    }
    if (frnd(w) < 0.06f && std::abs(note + L) <= 14 && std::fabs(note + L - c) <= width + L) note += L;   /* an octave up, rarely */
    const float wave = ui(w, IDX_FHEIGHT) > 0 ? c / (float)ui(w, IDX_FHEIGHT) : 0.0f;                         /* -1 .. 1 */
    int t = T_NOTE;
    const bool rest = frnd(w) > ui(w, IDX_FDENS) / 100.0f && s != 0;
    if (rest) t = T_REST;
    else {
        const bool acc = frnd(w) < 0.12f + 0.2f * std::max(0.0f, wave);
        const bool sld = frnd(w) < (std::abs(note - w->f_prev) <= 2 ? 0.3f : 0.08f);
        t = acc ? (sld ? T_ACC_SLD : T_ACC) : (sld ? T_SLIDE : T_NOTE);
    }
    const float level = rest ? 0.05f : std::min(1.0f, std::max(0.3f, 0.6f + 0.3f * wave + 0.25f * (frnd(w) - 0.5f)));
    set_ui(w, IDX_SN[s], note);
    set_step_type(w, s, t);
    set_norm(w, IDX_SL[s], level);
    if (!rest) w->f_prev = note;
    w->dirty.store(true);
}

/* One audio block of the step lanes, on the same grid as the filter steps: the step boundaries
 * k * step length (in quarters) that fall into [beat0, beat0 + frames * inc), by absolute index, so a block
 * the host splits or repeats plays nothing twice; a jump (loop, locate) ends the sounding note and goes on
 * from the new place. FLOW writes each step one step ahead (the filter reads it in time, the note lane at its
 * boundary); it runs on the sequencer's own clock while MPC stands. Notes only while MPC plays. */
static void notes_block(Plugin *w, bool playing, double beat0, double inc, int frames) {
    const double sb = SEQ_BEATS[clampi(ui(w, IDX_SEQRATE), 0, 7)];
    const int len = clampi(ui(w, IDX_SEQLEN), 1, 16);
    const double end = beat0 + inc * frames;
    const long long first = (long long)std::ceil(beat0 / sb - 1e-9), last = (long long)std::ceil(end / sb - 1e-9) - 1;
    auto step_of = [len](long long k) { long long m = k % len; return (int)(m < 0 ? m + len : m); };

    /* FLOW */
    if (ui(w, IDX_FLOW) == 1) {
        if (w->s_last_k == LLONG_MIN) {   /* FLOW switched on: a new line every time (tests: VFILTER_FLOW_SEED) */
            const char *fix = std::getenv("VFILTER_FLOW_SEED");
            w->frng = fix ? (uint32_t)std::atoi(fix) * 2654435761u + 1u : ((uint32_t)std::time(nullptr) * 2654435761u) ^ (uint32_t)(uintptr_t)w;
            if (!w->frng) w->frng = 1;
            w->f_phase = 0; w->f_amp = 1; w->f_prev = 0;
        }
        if (w->s_last_k == LLONG_MIN || first > w->s_last_k + 3 || last < w->s_last_k - 2) {   /* start or jump */
            w->s_last_k = first - 1;
            flow_step(w, step_of(first), first * sb);   /* the step about to come, now */
        }
        for (long long k = std::max(first, w->s_last_k + 1); k <= last; k++) flow_step(w, step_of(k + 1), (k + 1) * sb);
        if (last > w->s_last_k) w->s_last_k = last;
    } else w->s_last_k = LLONG_MIN;

    /* the notes */
    const bool on = ui(w, IDX_NOTES) == 1;
    if (!playing || !on) {
        if (w->n_cur >= 0) note_off_now(w, beat0);
        w->n_last_k = LLONG_MIN;
        return;
    }
    if (w->n_last_k == LLONG_MIN) w->n_last_k = first - 1;
    else if (first > w->n_last_k + 3 || last < w->n_last_k - 2) {   /* a jump */
        note_off_now(w, beat0);
        w->n_last_k = first - 1;
    }
    const double gate = ui(w, IDX_NLEN) / 100.0;
    for (long long k = std::max(first, w->n_last_k + 1); k <= last; k++) {
        const double b = k * sb;
        if (w->n_cur >= 0 && w->n_off >= 0 && w->n_off <= b + 1e-9) note_off_now(w, w->n_off);
        const int s = step_of(k), t = clampi(ui(w, IDX_ST[s]), 0, 4);
        if (t == T_REST) {
            if (w->n_cur >= 0 && w->n_off < 0) note_off_now(w, b);   /* a slide into a rest ends here */
            continue;
        }
        const int note = step_pitch(w, ui(w, IDX_SN[s]));
        const bool slide = t == T_SLIDE || t == T_ACC_SLD;
        const uint8_t vel = (t == T_ACC || t == T_ACC_SLD) ? 127 : 96;
        if (!(w->n_cur == note && w->n_off < 0)) {   /* the same pitch slid into: held, no new attack */
            const int ch = clampi(ui(w, IDX_NCH), 1, 16) - 1;
            midi_out(w, b, (uint8_t)(0x90 | ch), (uint8_t)note, vel);
            if (w->n_cur >= 0) midi_out(w, b, (uint8_t)(0x80 | w->n_ch), (uint8_t)w->n_cur, 0);   /* slide: the old one after, overlapping */
            w->n_cur = note; w->n_ch = ch;
        }
        w->n_off = slide ? -1.0 : b + gate * sb;
    }
    if (w->n_cur >= 0 && w->n_off >= 0 && w->n_off < end) note_off_now(w, w->n_off);
    if (last > w->n_last_k) w->n_last_k = last;
}

static void set_param_now(Plugin *w, int32_t i, float n);
/* MPC sets parameter 0 (here CUTOFF) to the far end and straight back on insert and on every STOP
 * (mpc-vst-plugins NOTES 2026-10-10): a host set of parameter 0 waits for the next block, a read or another
 * set; only the last one counts, and a pair that ends where it started is dropped - no cutoff jump. */
static void flush_pend0(Plugin *w) {
    if (!w->has_pend0.exchange(false)) return;
    if (std::fabs(w->pend0 - w->pend0_from) > 1e-6f) set_param_now(w, 0, w->pend0);
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    NoDenormals nd;
    flush_pend0(w);
    if (w->dirty.exchange(false)) configure(w);
    /* the beat grid: MPC's song position while it plays, else an own clock at its tempo (or 120) */
    const VstTimeInfo *ti = (const VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0, kVstPpqPosValid | kVstTempoValid, 0, 0.0f);
    const double tempo = ti && (ti->flags & kVstTempoValid) && ti->tempo > 20 && ti->tempo < 1000 ? ti->tempo : 120.0;
    const double inc = tempo / 60.0 / (double)w->sr;
    double beat0 = w->free_beat;
    const bool playing = ti && (ti->flags & kVstTransportPlaying) && (ti->flags & kVstPpqPosValid);
    if (playing) beat0 = ti->ppqPos;
    w->free_beat = beat0 + inc * n;
    if (w->free_beat > 1e7) w->free_beat = 0;
    notes_block(w, playing, beat0, inc, n);
    if (n > 0) w->dsp.process(w->p, in[0], in[1], out[0], out[1], (uint32_t)n, beat0, inc);
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
        /* a trigger (RANDOM, RESET) reports itself released and is ready for the next tap: the Force sends no
         * setParameter(0) after a tap, only the 1 of the next one */
        if (w->release[i]) { w->release[i] = 0; w->down[i] = false; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
        if (!w->notify[i].exchange(0)) continue;
        any = true;
        w->master(&w->fx, audioMasterAutomate, i, 0, 0, w->cache[i].load());
    }
    if (any) w->need_update = true;
    /* at most one UpdateDisplay per 50 ms (FLOW writes steps all the time); a request inside that window waits */
    if (w->update_wait > 0) w->update_wait -= n;
    if (w->update_wait <= 0 && w->need_update) {
        w->need_update = false;
        w->update_wait = (int)(w->sr * 0.05f);
        w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
    }
}

static void set_param_now(Plugin *w, int32_t i, float n) {
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    if (is_trigger(i)) {          /* a tap: act on the press, report the release */
        const bool d = n > 0.5f, rising = d && !w->down[i];
        w->down[i] = d;
        if (rising) {
            if (i == IDX_SEQRND) { pattern_random(w); notes_random(w); }   /* RANDOM: the whole pattern, notes too */
            else if (i == IDX_NRND) notes_random(w);                        /* RND NOTES: only the notes */
            else { pattern_init(w); notes_init(w); }
            w->release[i] = 1; w->dirty.store(true);
        }
        return;
    }
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
static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i == 0) {
        if (!w->has_pend0.load()) w->pend0_from = w->cache[0].load();
        w->pend0 = n;
        w->has_pend0.store(true);
        return;
    }
    flush_pend0(w);
    set_param_now(w, i, n);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    flush_pend0(w);
    if (popup_is(i)) return w->open[i];
    if (is_trigger(i)) return 0.0f;
    return w->cache[i].load();
}

/* The chunk keeps each knob's full position (x 100000, as an integer: no decimal point, so no
 * locale trouble), not the rounded readout, so CUTOFF comes back exactly where it was. */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string t = "VFL1;";
    char buf[64];
    for (int i = 0; i < NPARAMS; i++) {
        if (is_trigger(i)) continue;
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
        if (i < 0 || is_trigger(i)) continue;
        w->cache[i].store(clamp01(std::atol(kv.c_str() + eq + 1) / 100000.0f));
        w->notify[i].store(1);
    }
    w->dirty.store(true);
    return 1;
}

static bool is_step_vowel(int idx) { for (int k = 0; k < 16; k++) if (IDX_SV[k] == idx) return true; return false; }
static bool is_level(int idx) { for (int k = 0; k < 16; k++) if (IDX_SL[k] == idx) return true; return false; }
static bool is_step_note(int idx) { for (int k = 0; k < 16; k++) if (IDX_SN[k] == idx) return true; return false; }
static void note_name(int n, char *buf, size_t size) {   /* Akai's naming: note 60 = C3 */
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    n = clampi(n, 0, 127);
    std::snprintf(buf, size, "%s%d", N[n % 12], n / 12 - 2);
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
    else if (is_trigger(idx)) buf[0] = 0;
    else if (idx == IDX_ROOT) note_name(u, buf, size);
    else if (is_step_note(idx)) note_name(step_pitch(w, u), buf, size);
    else if (idx == IDX_NRANGE) std::snprintf(buf, size, "%d oct", u);
    else if (idx == IDX_NLEN) std::snprintf(buf, size, "%d %%", u);
    else if (idx == IDX_NCH) std::snprintf(buf, size, "CH %d", u);
    else if (idx == IDX_FSPEED) std::snprintf(buf, size, u == 1 ? "%d bar" : "%d bars", u);
    else if (idx == IDX_FHEIGHT) std::snprintf(buf, size, "+-%d", u);
    else if (idx == IDX_FDRIFT || idx == IDX_FDENS) std::snprintf(buf, size, "%d %%", u);
    else if (idx == IDX_SEQCUT) std::snprintf(buf, size, "%+d st", u);
    else if (idx == IDX_SEQLEN) std::snprintf(buf, size, "%d", u);
    else if (idx == IDX_SEQDEPTH || idx == IDX_SEQGATE || idx == IDX_SEQGLIDE || is_level(idx)) std::snprintf(buf, size, "%d %%", u);
    else if (idx == IDX_VOWEL || is_step_vowel(idx)) {   /* "A", "A>E 40" ... "U" */
        static const char V[] = "AEIOU";
        const float pos = clamp01(n) * 4.0f;
        const int i0 = clampi((int)pos, 0, 3), pc = (int)std::lround((pos - i0) * 100.0f);
        if (pc < 3) std::snprintf(buf, size, "%c", V[i0]);
        else if (pc > 97) std::snprintf(buf, size, "%c", V[i0 + 1]);
        else std::snprintf(buf, size, "%c>%c %d", V[i0], V[i0 + 1], pc);
    }
    else if (idx == IDX_LFORATE) std::snprintf(buf, size, "%.1f Hz", lfo_hz(n));
    else if (idx == IDX_VENV) std::snprintf(buf, size, "%+d", u);
    else if (idx == IDX_F2 || idx == IDX_ENVAMT || idx == IDX_FORMANT) std::snprintf(buf, size, "%+d st", u);
    else if (idx == IDX_OUTPUT) std::snprintf(buf, size, "%+d dB", u);
    else if (idx == IDX_DRIVE || idx == IDX_THRESH) std::snprintf(buf, size, "%d dB", u);
    else std::snprintf(buf, size, "%d", u);
    (void)w;
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    if (op != effClose) flush_pend0(w);
    switch (op) {
    case effOpen: return 1;
    case effClose: port_close(w); delete w; return 1;
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && !is_trigger(idx);
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
    case effSetBlockSize: return 1;
    case effMainsChanged: if (!v) { note_off_now(w, 0); w->n_last_k = LLONG_MIN; } return 1;
    case effCanDo:
        if (p && !std::strcmp((const char *)p, "receiveVstTimeInfo")) return 1;
        return -1;
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
    set_norm(w, IDX_VOWEL, 0.0f);      /* A */
    set_ui(w, IDX_FORMANT, 0);
    set_ui(w, IDX_FOCUS, 60);
    set_ui(w, IDX_VMIX, 0);            /* vowel filter off: a fresh instance and old projects sound as before */
    set_ui(w, IDX_VENV, 0);
    set_norm(w, IDX_LFORATE, 0.565f);  /* 2 Hz */
    set_ui(w, IDX_LFODEPTH, 0);
    set_ui(w, IDX_LFOSHAPE, 0);        /* TRI */
    /* the step sequencer: off (VOWEL / GATE / FILTER at 0), so old projects sound as before; the steps hold the
     * Formant Gate pattern, ready when SEQ VOWEL or SEQ GATE go up */
    set_ui(w, IDX_SEQRATE, 3);         /* 1/16 */
    set_ui(w, IDX_SEQLEN, 16);
    set_ui(w, IDX_SEQDEPTH, 0);
    set_ui(w, IDX_SEQGATE, 0);
    set_ui(w, IDX_SEQCUT, 0);
    set_ui(w, IDX_SEQGLIDE, 0);
    pattern_init(w);
    /* the note lane: off; ROOT C2 (note 48), GOA scale, held note as in the Formant Gate */
    set_ui(w, IDX_NOTES, 0);
    set_ui(w, IDX_ROOT, 48);
    set_ui(w, IDX_SCALE, 5);
    set_ui(w, IDX_NRANGE, 1);
    set_ui(w, IDX_NLEN, 60);
    set_ui(w, IDX_NCH, 1);
    notes_init(w);
    set_ui(w, IDX_FLOW, 0);            /* FLOW off: the steps stay as they are */
    set_ui(w, IDX_FSPEED, 4);
    set_ui(w, IDX_FHEIGHT, 5);
    set_ui(w, IDX_FWIDTH, 1);
    set_ui(w, IDX_FDRIFT, 30);
    set_ui(w, IDX_FDENS, 80);
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] {
        IDX_CUTOFF = param_index("cutoff"); IDX_RESO = param_index("reso"); IDX_F2 = param_index("f2");
        IDX_DRIVE = param_index("drive"); IDX_SAT = param_index("sat"); IDX_EMPH = param_index("emph");
        IDX_ENVAMT = param_index("envamt"); IDX_ATTACK = param_index("attack"); IDX_DECAY = param_index("decay");
        IDX_THRESH = param_index("thresh"); IDX_WARMTH = param_index("warmth"); IDX_WIDTH = param_index("width");
        IDX_OUTPUT = param_index("output");
        IDX_VOWEL = param_index("vowel"); IDX_FORMANT = param_index("formant"); IDX_FOCUS = param_index("focus");
        IDX_VMIX = param_index("vmix"); IDX_VENV = param_index("venv"); IDX_LFORATE = param_index("lforate");
        IDX_LFODEPTH = param_index("lfodepth"); IDX_LFOSHAPE = param_index("lfoshape");
        IDX_SEQRATE = param_index("seqrate"); IDX_SEQLEN = param_index("seqlen"); IDX_SEQDEPTH = param_index("seqdepth");
        IDX_SEQGATE = param_index("seqgate"); IDX_SEQCUT = param_index("seqcut"); IDX_SEQGLIDE = param_index("seqglide");
        IDX_SEQRND = param_index("seqrnd"); IDX_SEQINIT = param_index("seqinit");
        char k[8];
        for (int s = 0; s < 16; s++) {
            std::snprintf(k, sizeof k, "sv%d", s + 1); IDX_SV[s] = param_index(k);
            std::snprintf(k, sizeof k, "sl%d", s + 1); IDX_SL[s] = param_index(k);
            std::snprintf(k, sizeof k, "sn%d", s + 1); IDX_SN[s] = param_index(k);
            std::snprintf(k, sizeof k, "st%d", s + 1); IDX_ST[s] = param_index(k);
        }
        IDX_NOTES = param_index("notes"); IDX_ROOT = param_index("root"); IDX_SCALE = param_index("scale");
        IDX_NRANGE = param_index("nrange"); IDX_NLEN = param_index("nlen"); IDX_NCH = param_index("nch");
        IDX_NRND = param_index("nrnd");
        IDX_FLOW = param_index("flow"); IDX_FSPEED = param_index("fspeed"); IDX_FHEIGHT = param_index("fheight");
        IDX_FWIDTH = param_index("fwidth"); IDX_FDRIFT = param_index("fdrift"); IDX_FDENS = param_index("fdens");
    });
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(0); }
    start_values(w);
    w->dsp.setSampleRate(w->sr);
    port_open(w);
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
