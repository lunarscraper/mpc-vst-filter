/* Offline x86 test of vfilter_vst.cpp (test.sh builds it with ASan/UBSan): clean when everything
 * is off, CUTOFF and FILTER 2 darkening, RESONANCE peaking, DRIVE adding harmonics for each
 * saturation type, the audio-triggered envelope opening the filter, WIDTH (0 = both channels
 * equal), channel independence, loud input below 0 dBFS, the vowel filter (formant peaks per vowel,
 * FORMANT shift, VOWEL ENV, LFO), the step sequencer (on MPC's beat grid from ppqPos: gate at 1/16, 1/32 and
 * 1/64, bar position, free clock when stopped, vowel and filter steps, STEPS, RANDOM / RESET), the note lane
 * (pitch per scale, accent, slide, rests, length, 1/32, stop, loop jump, notes off, RND NOTES), FLOW (a line that
 * swings with the wave, small steps, in key, never the same, vowels kept, frozen when off, without notes), chunk restore,
 * NaN/denormal-free output.
 * With "bench" as the second argument it only measures CPU load. Prints PASSED/FAILED. */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

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
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
static VstTimeInfo g_ti;                 /* what MPC reports: tempo, song position in quarters, transport */
static bool g_host_time = false;         /* off: a host without time info (the plugin's own clock at 120) */
static int automated = 0;
static intptr_t master(AEffect *, int32_t op, int32_t, intptr_t, void *, float) {
    if (op == 0) automated++;
    if (op == 7) return g_host_time ? (intptr_t)&g_ti : 0;
    return 0;
}
enum { kPlaying = 1 << 1, kPpqValid = 1 << 9, kTempoValid = 1 << 10 };
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
static const float SR = 44100;
static const int BS = 256;
static bool bad = false;

static int param(AEffect *e, const char *name) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { buf[0] = 0; e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name)) return i; }
    std::printf("FAIL: no parameter %s\n", name); fails++; return 0;
}
static std::string display(AEffect *e, const char *name) { char buf[64] = {0}; e->dispatcher(e, 7, param(e, name), 0, buf, 0); return buf; }
static void set(AEffect *e, const char *name, int v) {
    struct R { const char *n; int lo, hi; };
    static const R r[] = {{"Cutoff", 0, 100}, {"Resonance", 0, 100}, {"Filter 2", -24, 24}, {"Drive", 0, 30},
                          {"Saturation", 0, 3}, {"Emphasis", 0, 100}, {"Env Amount", -48, 48}, {"Attack", 0, 100},
                          {"Decay", 0, 100}, {"Threshold", -60, 0}, {"Warmth", 0, 100}, {"Width", 0, 100}, {"Output", -24, 12},
                          {"Vowel", 0, 100}, {"Formant", -12, 12}, {"Focus", 0, 100}, {"Vowel Mix", 0, 100}, {"Vowel Env", -100, 100},
                          {"LFO Rate", 0, 100}, {"LFO Depth", 0, 100}, {"LFO Shape", 0, 2}};
    for (auto &x : r) if (!std::strcmp(x.n, name)) { e->setParameter(e, param(e, name), (float)(v - x.lo) / (x.hi - x.lo)); return; }
    static const R q[] = {{"Seq Rate", 0, 7}, {"Seq Steps", 1, 16}, {"Seq Vowel", 0, 100}, {"Seq Gate", 0, 100},
                          {"Seq Filter", -48, 48}, {"Seq Glide", 0, 100}, {"Seq Random", 0, 1}, {"Seq Reset", 0, 1}};
    for (auto &x : q) if (!std::strcmp(x.n, name)) { e->setParameter(e, param(e, name), (float)(v - x.lo) / (x.hi - x.lo)); return; }
    if (!std::strncmp(name, "Vowel ", 6) || !std::strncmp(name, "Level ", 6)) { e->setParameter(e, param(e, name), v / 100.0f); return; }
    static const R nq[] = {{"Notes", 0, 1}, {"Root", 24, 96}, {"Scale", 0, 7}, {"Note Range", 1, 3}, {"Note Length", 10, 100},
                           {"MIDI Channel", 1, 16}, {"Random Notes", 0, 1}, {"Flow", 0, 1}, {"Flow Speed", 1, 16},
                           {"Flow Height", 0, 14}, {"Flow Width", 0, 7}, {"Flow Drift", 0, 100}, {"Flow Density", 0, 100}};
    for (auto &x : nq) if (!std::strcmp(x.n, name)) { e->setParameter(e, param(e, name), (float)(v - x.lo) / (x.hi - x.lo)); return; }
    if (!std::strncmp(name, "Note ", 5)) { e->setParameter(e, param(e, name), (v + 14) / 28.0f); return; }
    if (!std::strncmp(name, "Trig ", 5)) { e->setParameter(e, param(e, name), v / 4.0f); return; }
    std::printf("FAIL: set %s\n", name); fails++;
}
/* a sine through the effect; returns the right channel's output (left gets silence unless both) */
static std::vector<float> run(AEffect *e, double f, double amp, double sec, std::vector<float> *left = nullptr, bool leftSilent = false) {
    std::vector<float> outR, il(BS), ir(BS), ol(BS), orr(BS);
    float *in[2] = {il.data(), ir.data()}, *out[2] = {ol.data(), orr.data()};
    static double ph = 0;
    for (int b = 0; b < (int)(sec * SR / BS); b++) {
        for (int i = 0; i < BS; i++) { float s = (float)(amp * std::sin(ph)); ph += 2 * M_PI * f / SR; il[i] = leftSilent ? 0 : s; ir[i] = s; }
        e->processReplacing(e, in, out, BS);
        if (g_ti.flags & kPlaying) g_ti.ppqPos += BS / SR * g_ti.tempo / 60.0;
        for (int i = 0; i < BS; i++) {
            for (float s : {ol[i], orr[i]}) if (!std::isfinite(s) || std::fpclassify(s) == FP_SUBNORMAL) bad = true;
            outR.push_back(orr[i]);
            if (left) left->push_back(ol[i]);
        }
    }
    return outR;
}
static double goertzel(const std::vector<float> &a, double f, double t0) {
    size_t i0 = (size_t)(t0 * SR), i1 = a.size();
    double w = 2 * M_PI * f / SR, c = std::cos(w), s1 = 0, s2 = 0;
    for (size_t i = i0; i < i1; i++) { double s0 = a[i] + 2 * c * s1 - s2; s2 = s1; s1 = s0; }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - 2 * c * s1 * s2)) / (i1 - i0);
}
static double rms(const std::vector<float> &a, double t0 = 0.2) {
    double s = 0; size_t i0 = (size_t)(t0 * SR);
    for (size_t i = i0; i < a.size(); i++) s += (double)a[i] * a[i];
    return std::sqrt(s / (a.size() - i0));
}
/* total harmonic distortion of f0 (harmonics 2..10) */
static double thd(const std::vector<float> &a, double f0) {
    double h1 = goertzel(a, f0, 0.2), hs = 0;
    for (int k = 2; k <= 10; k++) { double h = goertzel(a, f0 * k, 0.2); hs += h * h; }
    return std::sqrt(hs) / std::max(h1, 1e-12);
}
static double db(double x) { return 20 * std::log10(x + 1e-12); }

/* everything off: the two filters fully open, no saturation, no envelope, no chorus */
static void neutral(AEffect *e) {
    set(e, "Cutoff", 100); set(e, "Resonance", 0); set(e, "Filter 2", 0); set(e, "Drive", 0);
    set(e, "Saturation", 0); set(e, "Emphasis", 0); set(e, "Env Amount", 0); set(e, "Attack", 10);
    set(e, "Decay", 40); set(e, "Threshold", -36); set(e, "Warmth", 0); set(e, "Width", 0); set(e, "Output", 0);
    set(e, "Vowel", 0); set(e, "Formant", 0); set(e, "Focus", 60); set(e, "Vowel Mix", 0); set(e, "Vowel Env", 0);
    set(e, "LFO Rate", 50); set(e, "LFO Depth", 0); set(e, "LFO Shape", 0);
    if (e->numParams > 40) {   /* the step sequencer off */
        set(e, "Seq Vowel", 0); set(e, "Seq Gate", 0); set(e, "Seq Filter", 0); set(e, "Seq Glide", 0); set(e, "Seq Steps", 16); set(e, "Seq Rate", 3);
    }
}
static void silence(AEffect *e, double sec) { run(e, 100, 0.0, sec); }

int main(int argc, char **argv) {
    void *h = dlopen(argc > 1 ? argv[1] : "./vfilter.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("FAILED: dlopen %s\n", dlerror()); return 1; }
    auto mainf = (AEffect * (*)(audioMasterCallback)) dlsym(h, "VSTPluginMain");
    AEffect *e = mainf(master);
    CHECK(e->numInputs == 2 && e->numOutputs == 2, "not a stereo effect");
    e->dispatcher(e, 0, 0, 0, nullptr, 0);
    e->dispatcher(e, 10, 0, 0, nullptr, SR);
    if (argc > 2 && !std::strcmp(argv[2], "bench")) {
        set(e, "Drive", 30); set(e, "Saturation", 3); set(e, "Env Amount", 24); set(e, "Warmth", 100); set(e, "Width", 100);
        set(e, "Vowel Mix", 100); set(e, "Vowel Env", 50); set(e, "LFO Depth", 100); set(e, "LFO Rate", 80);
        set(e, "Seq Vowel", 100); set(e, "Seq Gate", 80); set(e, "Seq Filter", 24); set(e, "Seq Rate", 7); set(e, "Seq Glide", 40);
        auto t0 = std::chrono::steady_clock::now();
        run(e, 110, 0.3, 10.0);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("bench (this CPU, one core): stereo, everything on: %.1f %%\n", 10.0 * s);
        return 0;
    }
    std::printf("defaults: cutoff %s, reso %s, drive %s, sat %s, attack %s, decay %s\n", display(e, "Cutoff").c_str(),
                display(e, "Resonance").c_str(), display(e, "Drive").c_str(), display(e, "Saturation").c_str(),
                display(e, "Attack").c_str(), display(e, "Decay").c_str());

    /* 1. everything off: clean and at unity */
    neutral(e);
    {
        auto a = run(e, 220, 0.3, 1.0);
        double t = thd(a, 220), g = db(rms(a) / (0.3 / std::sqrt(2.0)));
        std::printf("  neutral: THD %.2f %%, gain %+.2f dB\n", 100 * t, g);
        CHECK(t < 0.01, "neutral is not clean (%.2f %%)", 100 * t);
        CHECK(std::fabs(g) < 1.0, "neutral is not at unity (%+.2f dB)", g);
    }

    /* 2. CUTOFF darkens; FILTER 2 lower darkens further */
    double c[3];
    set(e, "Cutoff", 100);                         c[0] = rms(run(e, 4000, 0.3, 0.6), 0.3);
    set(e, "Cutoff", 60);                          c[1] = rms(run(e, 4000, 0.3, 0.6), 0.3);
    set(e, "Filter 2", -24);                       c[2] = rms(run(e, 4000, 0.3, 0.6), 0.3);
    std::printf("  cutoff: 4 kHz at %.1f dB (open), %.1f dB (%s), %.1f dB (filter 2 -24 st)\n", db(c[0]), db(c[1]),
                display(e, "Cutoff").c_str(), db(c[2]));
    CHECK(c[1] < c[0] * 0.2, "CUTOFF does not darken");
    CHECK(c[2] < c[1] * 0.7, "FILTER 2 does not darken further");
    set(e, "Filter 2", 0);

    /* 3. RESONANCE: a peak at the cutoff (~1.18 kHz at 60) */
    {
        double fc = 20.0 * std::pow(900.0, 0.6), r[2];
        for (int k = 0; k < 2; k++) { set(e, "Resonance", k ? 80 : 0); r[k] = rms(run(e, fc, 0.05, 0.6), 0.3); }
        std::printf("  resonance: at the cutoff %.1f dB (0) vs %.1f dB (80)\n", db(r[0]), db(r[1]));
        CHECK(r[1] > r[0] * 2.0, "RESONANCE does not peak");
    }

    /* 4. DRIVE: harmonics rise, for each saturation type; OFF stays clean */
    neutral(e);
    const char *sn[4] = {"OFF", "SOFT", "HARD", "FOLD"};
    for (int s = 0; s < 4; s++) {
        set(e, "Saturation", s);
        double t[2];
        for (int k = 0; k < 2; k++) { set(e, "Drive", k ? 30 : 0); t[k] = thd(run(e, 220, 0.3, 0.6), 220); }
        std::printf("  saturation %-4s: THD %5.1f %% (drive 0), %5.1f %% (drive 30)\n", sn[s], 100 * t[0], 100 * t[1]);
        CHECK(display(e, "Saturation") == sn[s], "saturation readout %s", display(e, "Saturation").c_str());
        if (s == 0) CHECK(t[1] < 0.01, "saturation OFF distorts");
        else CHECK(t[1] > 0.1 && t[1] > t[0], "saturation %s: DRIVE adds no harmonics", sn[s]);
    }

    /* 5. envelope: a tone after silence opens the filter, then it closes again */
    neutral(e);
    set(e, "Cutoff", 35); set(e, "Env Amount", 48); set(e, "Attack", 0); set(e, "Decay", 40);
    silence(e, 0.5);
    {
        auto a = run(e, 2000, 0.3, 2.0);
        std::vector<float> early(a.begin(), a.begin() + (size_t)(0.03 * SR)), late(a.begin() + (size_t)(1.5 * SR), a.end());
        double ea = rms(early, 0.005), la = rms(late, 0);
        std::printf("  envelope: 2 kHz %.1f dB in the first 30 ms, %.1f dB after 1.5 s\n", db(ea), db(la));
        CHECK(ea > la * 4, "the envelope does not open the filter");
        set(e, "Env Amount", 0);
        silence(e, 0.5);
        auto b = run(e, 2000, 0.3, 0.5);
        std::vector<float> e2(b.begin(), b.begin() + (size_t)(0.03 * SR));
        CHECK(rms(e2, 0.005) < ea * 0.5, "ENV AMOUNT 0 still opens the filter");
    }

    /* 6. WIDTH 0: both channels equal; WIDTH 100: they differ */
    neutral(e);
    for (int k = 0; k < 2; k++) {
        set(e, "Width", k ? 100 : 0);
        std::vector<float> L;
        auto R = run(e, 330, 0.2, 1.0, &L);
        double d = 0;
        for (size_t i = (size_t)(0.3 * SR); i < R.size(); i++) d = std::max(d, (double)std::fabs(L[i] - R[i]));
        std::printf("  width %3d: largest L-R difference %.4f\n", k ? 100 : 0, d);
        if (k == 0) CHECK(d < 1e-6, "WIDTH 0 is not mono-compatible");
        else CHECK(d > 0.01, "WIDTH 100 adds no stereo");
    }
    {
        silence(e, 0.5);
        std::vector<float> L;
        auto R = run(e, 220, 0.2, 0.6, &L, true);
        std::printf("  stereo: left (silent in) rms %.6f, right rms %.4f\n", rms(L, 0.3), rms(R, 0.3));
        CHECK(rms(L, 0.3) < 1e-4 && rms(R, 0.3) > 0.01, "channels not independent");
    }

    /* 7. loud input stays below 0 dBFS */
    set(e, "Resonance", 100); set(e, "Cutoff", 30); set(e, "Drive", 30); set(e, "Saturation", 3); set(e, "Warmth", 100); set(e, "Output", 12);
    {
        auto a = run(e, 110, 1.0, 0.5);
        double pk = 0; for (float s : a) pk = std::max(pk, (double)std::fabs(s));
        std::printf("  hot: peak %.3f\n", pk);
        CHECK(pk < 1.0, "output above 0 dBFS");
    }

    /* 8. vowel filter: each vowel passes its own formants, FORMANT moves them, the envelope and
     *    the LFO move the vowel; VOWEL MIX 0 is covered by the tests above (all run with it off) */
    neutral(e);
    set(e, "Vowel Mix", 100); set(e, "Focus", 80);
    {
        const char *vn[5] = {"A", "E", "I", "O", "U"};
        const double f1[5] = {700, 450, 290, 420, 330}, f2[5] = {1150, 1750, 2100, 800, 650};
        double lv[5][2];
        for (int v = 0; v < 5; v++) {
            set(e, "Vowel", v * 25);
            CHECK(display(e, "Vowel") == vn[v], "vowel readout %s", display(e, "Vowel").c_str());
            lv[v][0] = rms(run(e, 700, 0.1, 0.5), 0.3); lv[v][1] = rms(run(e, 2100, 0.1, 0.5), 0.3);
            double on = rms(run(e, f1[v], 0.1, 0.5), 0.3), on2 = rms(run(e, f2[v], 0.1, 0.5), 0.3), off = rms(run(e, 5000, 0.1, 0.5), 0.3);
            std::printf("  vowel %s: F1 %.1f dB, F2 %.1f dB, 5 kHz %.1f dB\n", vn[v], db(on), db(on2), db(off));
            CHECK(on > off * 8 && on2 > off * 3, "vowel %s has no formant peaks", vn[v]);
        }
        CHECK(lv[0][0] > lv[2][0] * 3, "700 Hz: A is not louder than I");
        CHECK(lv[2][1] > lv[4][1] * 3, "2.1 kHz: I is not louder than U");
        set(e, "Vowel", 0);
        double a0 = rms(run(e, 1400, 0.1, 0.5), 0.3);
        set(e, "Formant", 12);
        double a1 = rms(run(e, 1400, 0.1, 0.5), 0.3);
        std::printf("  formant: 1.4 kHz %.1f dB (0), %.1f dB (%s)\n", db(a0), db(a1), display(e, "Formant").c_str());
        CHECK(a1 > a0 * 2, "FORMANT does not shift the formants");
        set(e, "Formant", 0);

        /* VOWEL ENV: a note starts on U and falls back to A */
        set(e, "Vowel Env", 100); set(e, "Attack", 0); set(e, "Decay", 30);
        silence(e, 0.5);
        auto b = run(e, 700, 0.1, 1.5);
        std::vector<float> early(b.begin() + (size_t)(0.005 * SR), b.begin() + (size_t)(0.03 * SR)), late(b.begin() + (size_t)(1.2 * SR), b.end());
        std::printf("  vowel env: 700 Hz %.1f dB in the first 30 ms, %.1f dB after 1.2 s\n", db(rms(early, 0)), db(rms(late, 0)));
        CHECK(rms(late, 0) > rms(early, 0) * 2, "VOWEL ENV does not move the vowel");
        set(e, "Vowel Env", 0);

        /* LFO: the level of one formant frequency goes up and down, for each shape */
        const char *ln[3] = {"TRI", "RANDOM", "STEP"};
        set(e, "Vowel", 50); set(e, "LFO Depth", 100); set(e, "LFO Rate", 75);
        for (int s = 0; s < 3; s++) {
            set(e, "LFO Shape", s);
            CHECK(display(e, "LFO Shape") == ln[s], "LFO shape readout %s", display(e, "LFO Shape").c_str());
            auto c = run(e, 2100, 0.1, 4.0);
            double lo = 1e9, hi = 0;
            for (size_t i = (size_t)(0.3 * SR); i + 441 < c.size(); i += 441) {
                double q = 0; for (int j = 0; j < 441; j++) q += (double)c[i + j] * c[i + j];
                q = std::sqrt(q / 441); lo = std::min(lo, q); hi = std::max(hi, q);
            }
            std::printf("  lfo %-6s (%s): 2.1 kHz between %.1f and %.1f dB\n", ln[s], display(e, "LFO Rate").c_str(), db(lo), db(hi));
            CHECK(hi > lo * 3, "LFO %s does not move the vowel", ln[s]);
        }
        set(e, "LFO Depth", 0);
        auto d = run(e, 2100, 0.1, 1.0);
        double lo = 1e9, hi = 0;
        for (size_t i = (size_t)(0.3 * SR); i + 441 < d.size(); i += 441) {
            double q = 0; for (int j = 0; j < 441; j++) q += (double)d[i + j] * d[i + j];
            lo = std::min(lo, q); hi = std::max(hi, q);
        }
        CHECK(hi < lo * 1.05, "LFO DEPTH 0 still moves the vowel");

        /* loud and sharp stays below 0 dBFS */
        set(e, "Focus", 100); set(e, "Output", 12);
        auto h = run(e, 700, 1.0, 0.5);
        double pk = 0; for (float s : h) pk = std::max(pk, (double)std::fabs(s));
        std::printf("  vowel hot: peak %.3f\n", pk);
        CHECK(pk < 1.0, "vowel filter: output above 0 dBFS");
    }

    /* 10. the step sequencer, on MPC's beat grid */
    {
        auto window = [](const std::vector<float> &a, double t0, double t1) {
            double q = 0; size_t i0 = (size_t)(t0 * SR), i1 = std::min(a.size(), (size_t)(t1 * SR));
            for (size_t i = i0; i < i1; i++) q += (double)a[i] * a[i];
            return std::sqrt(q / std::max<size_t>(1, i1 - i0));
        };
        auto steplevels = [](AEffect *x, int on_every) {   /* level 100 on every n-th step, 0 elsewhere */
            for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Level %d", k); set(x, nm, (k - 1) % on_every == 0 ? 100 : 0); }
        };
        /* a gate pattern loud / quiet alternating, at each rate: the loud and the quiet halves of every step pair,
         * measured inside the steps (2 ms clear of the edges) */
        g_host_time = true;
        g_ti = VstTimeInfo{}; g_ti.sampleRate = SR; g_ti.tempo = 120; g_ti.flags = kTempoValid | kPpqValid | kPlaying;
        const int rates[3] = {3, 5, 7};
        const char *rn[3] = {"1/16", "1/32", "1/64"};
        const double stepSec[3] = {0.125, 0.0625, 0.03125};
        for (int r = 0; r < 3; r++) {
            neutral(e);
            steplevels(e, 2);
            set(e, "Seq Rate", rates[r]); set(e, "Seq Steps", 16); set(e, "Seq Gate", 100);
            CHECK(display(e, "Seq Rate") == rn[r], "rate readout %s", display(e, "Seq Rate").c_str());
            g_ti.ppqPos = 0;
            auto a = run(e, 440, 0.3, 1.5);
            double loud = 1e9, quiet = 0;
            const double st = stepSec[r];
            for (int k = 2; k * st + st < 1.4; k++) {
                const double w = window(a, k * st + 0.003, k * st + st - 0.003);
                if (k % 2 == 0) loud = std::min(loud, w); else quiet = std::max(quiet, w);
            }
            std::printf("  seq gate %s (%.1f ms steps): loud steps >= %.1f dB, quiet steps <= %.1f dB\n", rn[r], 1000 * st, db(loud), db(quiet));
            CHECK(loud > quiet * 8, "SEQ GATE at %s does not follow the steps", rn[r]);
        }
        /* the grid is MPC's: starting at quarter 0.5 (the third sixteenth) the first sound is step 3 */
        neutral(e);
        for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Level %d", k); set(e, nm, k == 3 ? 100 : 0); }
        set(e, "Seq Rate", 3); set(e, "Seq Gate", 100);
        g_ti.ppqPos = 0.5;
        {
            auto a = run(e, 440, 0.3, 0.5);
            const double s3 = window(a, 0.004, 0.121), s4 = window(a, 0.129, 0.246);
            std::printf("  seq position: from quarter 0.5 step 3 %.1f dB, step 4 %.1f dB\n", db(s3), db(s4));
            CHECK(s3 > s4 * 8, "the sequencer is not on MPC's grid (song position)");
        }
        /* STEPS 4: the pattern repeats after four steps */
        steplevels(e, 4);
        set(e, "Seq Steps", 4);
        for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Level %d", k); set(e, nm, k == 1 ? 100 : 0); }
        g_ti.ppqPos = 0;
        {
            auto a = run(e, 440, 0.3, 1.1);
            const double s1 = window(a, 0.504, 0.621), s2 = window(a, 0.629, 0.746);   /* steps 5 and 6 of the bar = 1 and 2 */
            std::printf("  seq steps 4: bar step 5 %.1f dB, step 6 %.1f dB\n", db(s1), db(s2));
            CHECK(s1 > s2 * 8, "STEPS 4 does not repeat after four steps");
        }
        set(e, "Seq Steps", 16);
        /* MPC stopped: the sequencer keeps stepping on its own clock */
        steplevels(e, 2);
        g_ti.flags = kTempoValid;
        {
            auto a = run(e, 440, 0.3, 1.2);
            double lo = 1e9, hi = 0;   /* its own clock runs on from where MPC stopped: 10 ms windows, no grid */
            for (double t = 0.1; t + 0.01 < 1.2; t += 0.01) { const double w = window(a, t, t + 0.01); lo = std::min(lo, w); hi = std::max(hi, w); }
            std::printf("  seq stopped: steps between %.1f and %.1f dB\n", db(lo), db(hi));
            CHECK(hi > lo * 8, "the sequencer stands still while MPC is stopped");
        }
        /* a host without time information: its own clock at 120 */
        g_host_time = false;
        {
            auto a = run(e, 440, 0.3, 1.2);
            double lo = 1e9, hi = 0;   /* its own clock runs on from where MPC stopped: 10 ms windows, no grid */
            for (double t = 0.1; t + 0.01 < 1.2; t += 0.01) { const double w = window(a, t, t + 0.01); lo = std::min(lo, w); hi = std::max(hi, w); }
            CHECK(hi > lo * 8, "no time info: the sequencer stands still");
        }
        g_host_time = true; g_ti.flags = kTempoValid | kPpqValid | kPlaying;
        /* vowel steps: A and I alternating; 2.1 kHz (I's F2) up and down with the steps */
        neutral(e);
        set(e, "Vowel Mix", 100); set(e, "Focus", 80); set(e, "Seq Vowel", 100); set(e, "Seq Rate", 3);
        for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Vowel %d", k); set(e, nm, k % 2 ? 0 : 50); }
        CHECK(display(e, "Vowel 1") == "A" && display(e, "Vowel 2") == "I", "step vowel readout %s %s", display(e, "Vowel 1").c_str(), display(e, "Vowel 2").c_str());
        g_ti.ppqPos = 0;
        {
            auto a = run(e, 2100, 0.1, 1.2);
            double iMin = 1e9, aMax = 0;   /* odd windows are the even steps (I), even windows the odd steps (A) */
            for (int k = 1; k < 9; k++) { const double w = window(a, k * 0.125 + 0.01, k * 0.125 + 0.12); if (k % 2) iMin = std::min(iMin, w); else aMax = std::max(aMax, w); }
            std::printf("  seq vowel: 2.1 kHz on the I steps >= %.1f dB, on the A steps <= %.1f dB\n", db(iMin), db(aMax));
            CHECK(iMin > aMax * 2, "SEQ VOWEL does not move the vowel");
        }
        /* filter steps: CUTOFF low, SEQ FILTER +48: 3 kHz opens on the loud steps */
        neutral(e);
        set(e, "Cutoff", 30); set(e, "Seq Filter", 48); set(e, "Seq Rate", 3);
        steplevels(e, 2);
        g_ti.ppqPos = 0;
        {
            auto a = run(e, 3000, 0.1, 1.2);
            double open = 1e9, closed = 0;
            for (int k = 1; k < 9; k++) { const double w = window(a, k * 0.125 + 0.01, k * 0.125 + 0.12); if (k % 2 == 0) open = std::min(open, w); else closed = std::max(closed, w); }
            std::printf("  seq filter: 3 kHz on open steps >= %.1f dB, on closed steps <= %.1f dB\n", db(open), db(closed));
            CHECK(open > closed * 4, "SEQ FILTER does not open the filter on the steps");
        }
        /* GLIDE: with full glide the gate's edges are slower, the vowel slides (no hard step in 1 ms) */
        /* RANDOM writes new steps, RESET brings the Formant Gate pattern back */
        set(e, "Seq Reset", 1); set(e, "Seq Reset", 0);
        CHECK(display(e, "Vowel 1") == "U" && display(e, "Level 7") == "100 %" && display(e, "Vowel 7") == "I", "RESET: %s %s %s",
              display(e, "Vowel 1").c_str(), display(e, "Level 7").c_str(), display(e, "Vowel 7").c_str());
        std::string before; for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Level %d", k); before += display(e, nm) + "|"; }
        set(e, "Seq Random", 1); set(e, "Seq Random", 0);
        std::string after; for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Level %d", k); after += display(e, nm) + "|"; }
        CHECK(before != after && display(e, "Level 1") == "100 %", "RANDOM did not write a new pattern");
        CHECK(e->getParameter(e, param(e, "Seq Random")) == 0.0f, "RANDOM does not spring back");
        /* like the Force: a tap sends only the 1, never a 0 after it - every tap must act */
        for (int t = 0; t < 3; t++) {
            std::string was; for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Vowel %d", k); was += display(e, nm) + "|"; }
            e->setParameter(e, param(e, "Seq Random"), 1.0f);
            run(e, 440, 0.0, 0.02);   /* a few blocks: the plugin reports the release */
            std::string now; for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Vowel %d", k); now += display(e, nm) + "|"; }
            CHECK(now != was, "RANDOM tap %d (1 only, as the Force sends it) made no new pattern", t + 1);
        }
        e->setParameter(e, param(e, "Seq Reset"), 1.0f);
        run(e, 440, 0.0, 0.02);
        CHECK(display(e, "Vowel 1") == "U", "RESET tap (1 only) did nothing");
        set(e, "Seq Random", 1.0); run(e, 440, 0.0, 0.02);
        e->setParameter(e, param(e, "Seq Reset"), 1.0f);
        run(e, 440, 0.0, 0.02);
        CHECK(display(e, "Vowel 1") == "U" && display(e, "Level 7") == "100 %", "second RESET tap (1 only) did nothing");
        /* the Formant Gate in full: vowel, gate and filter at once, at 1/32, loud input stays below 0 dBFS */
        neutral(e);
        set(e, "Seq Reset", 1); set(e, "Seq Reset", 0);
        set(e, "Vowel Mix", 100); set(e, "Seq Vowel", 100); set(e, "Seq Gate", 80); set(e, "Cutoff", 45); set(e, "Seq Filter", 36);
        set(e, "Seq Rate", 5); set(e, "Seq Glide", 30); set(e, "Output", 12); set(e, "Focus", 100);
        {
            auto a = run(e, 110, 1.0, 1.0);
            double pk = 0; for (float x : a) pk = std::max(pk, (double)std::fabs(x));
            std::printf("  formant gate at 1/32, hot: peak %.3f, rms %.1f dB\n", pk, db(rms(a)));
            CHECK(pk < 1.0 && rms(a) > 0.01, "formant gate: peak %.3f", pk);
        }
        g_host_time = false;
    }

    /* 11. the note lane (offline build: the notes are recorded with their quarter instead of going to ALSA) */
    {
        auto cnt = (int (*)(void))dlsym(h, "vfilter_dbg_midi_count");
        auto get = (int (*)(int, double *, int *, int *, int *))dlsym(h, "vfilter_dbg_midi");
        auto clr = (void (*)(void))dlsym(h, "vfilter_dbg_midi_clear");
        CHECK(cnt && get && clr, "no MIDI test hooks (build with -DNO_ALSA)");
        if (cnt && get && clr) {
            struct Ev { double b; int st, d1, d2; };
            auto events = [&]() { std::vector<Ev> v; for (int i = 0; i < cnt(); i++) { Ev x; get(i, &x.b, &x.st, &x.d1, &x.d2); v.push_back(x); } return v; };
            auto balanced = [&](const std::vector<Ev> &v) {   /* every note-on ends, nothing ends twice */
                int on[128] = {0};
                for (auto &x : v) { if ((x.st & 0xF0) == 0x90 && x.d2 > 0) on[x.d1]++; else if (on[x.d1] > 0) on[x.d1]--; else return false; }
                for (int k = 0; k < 128; k++) if (on[k]) return false;
                return true;
            };
            auto trig = [&](int k, int t) { char nm[16]; std::snprintf(nm, sizeof nm, "Trig %d", k); set(e, nm, t); };
            auto note = [&](int k, int d) { char nm[16]; std::snprintf(nm, sizeof nm, "Note %d", k); set(e, nm, d); };
            g_host_time = true;
            g_ti = VstTimeInfo{}; g_ti.sampleRate = SR; g_ti.tempo = 120; g_ti.flags = kTempoValid | kPpqValid | kPlaying;
            neutral(e);
            set(e, "Notes", 1); set(e, "Scale", 0); set(e, "Root", 60); set(e, "Note Length", 50); set(e, "MIDI Channel", 3);
            CHECK(display(e, "Root") == "C3" && display(e, "MIDI Channel") == "CH 3", "readouts %s %s", display(e, "Root").c_str(), display(e, "MIDI Channel").c_str());
            for (int k = 1; k <= 16; k++) { trig(k, 0); note(k, 0); }
            /* 1 NOTE C3, 2 ACC C#3, 3 rest, 4 SLIDE D#3 -> 5 NOTE E3, 6 SLIDE F3 -> 7 SLIDE F3 (held) -> 8 NOTE G3 */
            note(1, 0); trig(1, 1); note(2, 1); trig(2, 2); note(4, 3); trig(4, 3); note(5, 4); trig(5, 1);
            note(6, 5); trig(6, 3); note(7, 5); trig(7, 3); note(8, 7); trig(8, 1);
            CHECK(display(e, "Note 4") == "D#3" && display(e, "Trig 4") == "SLIDE", "step readouts %s %s", display(e, "Note 4").c_str(), display(e, "Trig 4").c_str());
            clr();
            g_ti.ppqPos = 0;
            run(e, 100, 0.0, 2.0);    /* one bar (344 blocks: just short of quarter 4) */
            auto v = events();
            const Ev want[] = {{0.0, 0x92, 60, 96}, {0.125, 0x82, 60, 0}, {0.25, 0x92, 61, 127}, {0.375, 0x82, 61, 0},
                               {0.75, 0x92, 63, 96}, {1.0, 0x92, 64, 96}, {1.0, 0x82, 63, 0}, {1.125, 0x82, 64, 0},
                               {1.25, 0x92, 65, 96}, {1.75, 0x92, 67, 96}, {1.75, 0x82, 65, 0}, {1.875, 0x82, 67, 0}};
            bool same = v.size() >= 12;
            for (int i = 0; same && i < 12; i++)
                same = std::fabs(v[i].b - want[i].b) < 1e-6 && v[i].st == want[i].st && v[i].d1 == want[i].d1 && v[i].d2 == want[i].d2;
            std::printf("  notes, one bar at 1/16: %d events%s\n", (int)v.size(), same ? ", as written (note, accent, rest, slide, held slide)" : "");
            if (!same) for (auto &x : v) std::printf("    %.3f %02x %d %d\n", x.b, x.st, x.d1, x.d2);
            CHECK(same && v.size() == 12, "the note lane did not play the steps as written (%d events)", (int)v.size());
            /* the second bar repeats it */
            clr(); run(e, 100, 0.0, 2.0);
            CHECK(events().size() == 12 && std::fabs(events()[0].b - 4.0) < 1e-6, "the second bar does not repeat");
            /* MPC stops while a slide holds a note: it ends */
            clr(); g_ti.ppqPos = 1.3; run(e, 100, 0.0, 0.15);   /* into step 7: F3 slides on, held */
            g_ti.flags = kTempoValid; run(e, 100, 0.0, 0.1);
            auto st = events();
            CHECK(!st.empty() && (st.back().st & 0xF0) == 0x80, "STOP left a note hanging");
            g_ti.flags = kTempoValid | kPpqValid | kPlaying;
            /* 1/32: twice as many notes in the same time */
            for (int k = 1; k <= 16; k++) { trig(k, 1); note(k, 0); }
            set(e, "Seq Rate", 5);
            clr(); g_ti.ppqPos = 0; run(e, 100, 0.0, 2.0);
            int ons = 0; for (auto &x : events()) ons += (x.st & 0xF0) == 0x90;
            std::printf("  notes at 1/32: %d note-ons in one bar\n", ons);
            CHECK(ons == 32 && balanced(events()), "1/32: %d notes in a bar", ons);
            set(e, "Seq Rate", 3);
            /* scale: GOA from C2 = C2 C#2 E2 F2 G2 G#2 A#2 C3 */
            set(e, "Scale", 5); set(e, "Root", 48);
            for (int k = 1; k <= 8; k++) note(k, k - 1);
            for (int k = 9; k <= 16; k++) trig(k, 0);
            clr(); g_ti.ppqPos = 0; run(e, 100, 0.0, 2.0);
            std::vector<int> got; for (auto &x : events()) if ((x.st & 0xF0) == 0x90) got.push_back(x.d1);
            const std::vector<int> goa = {48, 49, 52, 53, 55, 56, 58, 60};
            std::printf("  scale GOA from %s:", display(e, "Root").c_str()); for (int n : got) std::printf(" %d", n); std::printf("\n");
            CHECK(got == goa, "the GOA scale is not phrygian dominant from the root");
            /* a loop back: nothing doubles, nothing hangs */
            clr(); g_ti.ppqPos = 0; run(e, 100, 0.0, 0.9); g_ti.ppqPos = 0; run(e, 100, 0.0, 0.9);
            g_ti.flags = kTempoValid; run(e, 100, 0.0, 0.1); g_ti.flags = kTempoValid | kPpqValid | kPlaying;
            CHECK(balanced(events()), "a loop jump left notes hanging or ended them twice");
            /* NOTES OFF: silence on the port */
            set(e, "Notes", 0); clr(); g_ti.ppqPos = 0; run(e, 100, 0.0, 1.0);
            CHECK(cnt() == 0, "NOTES OFF still sends notes");
            set(e, "Notes", 1);
            /* RND NOTES: new notes, all in the scale; RESET: one held note */
            std::string before; for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Note %d", k); before += display(e, nm) + "|"; }
            for (int t = 0; t < 3; t++) { e->setParameter(e, param(e, "Random Notes"), 1.0f); run(e, 100, 0.0, 0.02); }
            g_ti.flags = kTempoValid; run(e, 100, 0.0, 0.02); g_ti.flags = kTempoValid | kPpqValid | kPlaying;   /* stop: nothing left over */
            std::string after; for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Note %d", k); after += display(e, nm) + "|"; }
            CHECK(before != after, "RND NOTES made no new notes");
            clr(); g_ti.ppqPos = 0; run(e, 100, 0.0, 2.0);
            g_ti.flags = kTempoValid; run(e, 100, 0.0, 0.02); g_ti.flags = kTempoValid | kPpqValid | kPlaying;
            bool inkey = true; const int pcs[7] = {0, 1, 4, 5, 7, 8, 10};
            for (auto &x : events()) if ((x.st & 0xF0) == 0x90) { int pc = ((x.d1 - 48) % 12 + 12) % 12; bool ok = false; for (int q : pcs) ok |= q == pc; inkey &= ok; }
            std::printf("  rnd notes: %s\n", after.c_str());
            CHECK(inkey && balanced(events()), "RND NOTES left the scale");
            e->setParameter(e, param(e, "Seq Reset"), 1.0f); run(e, 100, 0.0, 0.02);
            clr(); g_ti.ppqPos = 0; run(e, 100, 0.0, 2.0);
            int ons2 = 0; for (auto &x : events()) ons2 += (x.st & 0xF0) == 0x90;
            std::printf("  reset: %d note-ons in a bar (the Formant Gate: one held note, again after each rest)\n", ons2);
            CHECK(ons2 >= 1 && ons2 <= 4, "RESET: %d note-ons", ons2);
            /* FLOW: a swinging line - eight bars, wave of two bars, height 7 scale steps, width 1 (fixed seed) */
            if (!std::getenv("VFILTER_FLOW_SEED")) setenv("VFILTER_FLOW_SEED", "7", 1);
            set(e, "Notes", 1); set(e, "Scale", 5); set(e, "Root", 48); set(e, "Note Length", 60); set(e, "Seq Rate", 3); set(e, "Seq Steps", 16);
            set(e, "Flow", 1); set(e, "Flow Speed", 2); set(e, "Flow Height", 7); set(e, "Flow Width", 1); set(e, "Flow Drift", 30); set(e, "Flow Density", 85);
            CHECK(display(e, "Flow Speed") == "2 bars" && display(e, "Flow Height") == "+-7", "flow readouts %s %s", display(e, "Flow Speed").c_str(), display(e, "Flow Height").c_str());
            std::string vow0; for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Vowel %d", k); vow0 += display(e, nm) + "|"; }
            g_ti.flags = kTempoValid; run(e, 100, 0.0, 0.05); g_ti.flags = kTempoValid | kPpqValid | kPlaying;
            clr(); g_ti.ppqPos = 0; run(e, 100, 0.0, 16.0);
            g_ti.flags = kTempoValid; run(e, 100, 0.0, 0.05); g_ti.flags = kTempoValid | kPpqValid | kPlaying;
            {
                auto fv = events();
                std::vector<std::pair<double, int>> ons;
                for (auto &x : fv) if ((x.st & 0xF0) == 0x90) ons.push_back({x.b, x.d1});
                bool inkey = true; const int pcs[7] = {0, 1, 4, 5, 7, 8, 10};
                int small = 0;
                for (size_t i = 0; i < ons.size(); i++) {
                    int pc = ((ons[i].second - 48) % 12 + 12) % 12; bool ok = false; for (int q : pcs) ok |= q == pc; inkey &= ok;
                    if (i && std::abs(ons[i].second - ons[i - 1].second) <= 4) small++;
                }
                /* the swing: the pitch's strength at the wave's frequency (one cycle per 8 quarters) against others */
                auto power = [&](double cyclesPerQuarter) {
                    double re = 0, im = 0, mean = 0;
                    for (auto &o : ons) mean += o.second;
                    mean /= std::max<size_t>(1, ons.size());
                    for (auto &o : ons) { double a = 2 * M_PI * cyclesPerQuarter * o.first; re += (o.second - mean) * std::cos(a); im += (o.second - mean) * std::sin(a); }
                    return std::sqrt(re * re + im * im) / std::max<size_t>(1, ons.size());
                };
                const double pw = power(1.0 / 8);
                double other = 0; int no = 0;
                for (double f : {1.0 / 32, 1.0 / 3, 1.0 / 2, 3.0 / 8, 1.0 / 5, 1.0 / 1.5}) { other += power(f); no++; }
                other /= no;
                std::string b1, b3;
                for (auto &o : ons) { if (o.first < 4) b1 += std::to_string(o.second) + " "; else if (o.first >= 8 && o.first < 12) b3 += std::to_string(o.second) + " "; }
                std::printf("  flow: %d notes in 8 bars, %.0f %% small steps (<= 4 st), swing at the wave %.2f vs other rates %.2f\n",
                            (int)ons.size(), 100.0 * small / std::max<size_t>(1, ons.size() - 1), pw, other);
                std::printf("  flow bar 1: %s\n  flow bar 3: %s\n", b1.c_str(), b3.c_str());
                CHECK(ons.size() >= 90 && inkey, "flow: %d notes, in key %d", (int)ons.size(), (int)inkey);
                CHECK(small > 0.6 * (ons.size() - 1), "flow jumps around: %d small steps of %d", small, (int)ons.size() - 1);
                CHECK(pw > 3 * other, "flow does not swing with the wave (%.2f vs %.2f)", pw, other);
                CHECK(b1 != b3, "flow repeats itself");
                CHECK(balanced(fv), "flow left notes hanging");
            }
            std::string vow1; for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Vowel %d", k); vow1 += display(e, nm) + "|"; }
            CHECK(vow0 == vow1, "FLOW changed the vowels");
            /* FLOW off: what it wrote last stays as the pattern - two bars alike */
            set(e, "Flow", 0);
            clr(); g_ti.ppqPos = 0; run(e, 100, 0.0, 2.0);
            std::vector<int> f1; for (auto &x : events()) if ((x.st & 0xF0) == 0x90) f1.push_back(x.d1);
            clr(); run(e, 100, 0.0, 2.0);
            std::vector<int> f2; for (auto &x : events()) if ((x.st & 0xF0) == 0x90) f2.push_back(x.d1);
            g_ti.flags = kTempoValid; run(e, 100, 0.0, 0.05); g_ti.flags = kTempoValid | kPpqValid | kPlaying;
            CHECK(!f1.empty() && f1 == f2, "FLOW off: the pattern is not kept (%d / %d notes)", (int)f1.size(), (int)f2.size());
            /* FLOW without notes: the levels (filter, gate) keep moving */
            set(e, "Notes", 0); set(e, "Flow", 1);
            std::string l0; for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Level %d", k); l0 += display(e, nm) + "|"; }
            clr(); g_ti.ppqPos = 0; run(e, 100, 0.0, 2.0);
            std::string l1; for (int k = 1; k <= 16; k++) { char nm[16]; std::snprintf(nm, sizeof nm, "Level %d", k); l1 += display(e, nm) + "|"; }
            CHECK(l0 != l1 && cnt() == 0, "FLOW without notes: levels %s, %d notes", l0 == l1 ? "stand still" : "move", cnt());
            set(e, "Flow", 0);
            set(e, "Notes", 0);
            g_ti.flags = kTempoValid; run(e, 100, 0.0, 0.05);
            g_host_time = false;
        }
    }

    /* 9a. MPC sets parameter 0 (CUTOFF) to the far end and straight back on insert and STOP: no cutoff jump */
    {
        neutral(e);
        set(e, "Cutoff", 40);
        run(e, 4000, 0.3, 0.3);
        const float before = e->getParameter(e, 0);
        e->setParameter(e, 0, 1.0f); e->setParameter(e, 0, before);
        auto a = run(e, 4000, 0.3, 0.3);
        double pk = 0; for (size_t i = 0; i < (size_t)(0.02 * SR); i++) pk = std::max(pk, (double)std::fabs(a[i]));
        std::printf("  parameter 0 toggle: cutoff %s, 4 kHz peak in the next 20 ms %.4f\n", display(e, "Cutoff").c_str(), pk);
        CHECK(std::fabs(e->getParameter(e, 0) - before) < 1e-6f && pk < 0.05, "the parameter 0 toggle moved the cutoff");
        e->setParameter(e, 0, 0.7f);
        CHECK(std::fabs(e->getParameter(e, 0) - 0.7f) < 1e-6f, "a real CUTOFF move is lost");
    }

    /* 9. chunk */
    set(e, "Vowel", 75); set(e, "Formant", -5); set(e, "Vowel Mix", 60); set(e, "LFO Shape", 2);
    set(e, "Seq Rate", 5); set(e, "Seq Gate", 70); set(e, "Vowel 5", 38); set(e, "Level 12", 64); set(e, "Seq Steps", 12);
    set(e, "Saturation", 2); set(e, "Filter 2", 7); set(e, "Env Amount", -12); set(e, "Output", -4);
    e->setParameter(e, param(e, "Cutoff"), 0.4321f);
    void *chunk = nullptr;
    intptr_t len = e->dispatcher(e, 23, 0, 0, &chunk, 0);
    std::vector<uint8_t> copy((uint8_t *)chunk, (uint8_t *)chunk + len);
    AEffect *e2 = mainf(master);
    e2->dispatcher(e2, 10, 0, 0, nullptr, SR);
    e2->dispatcher(e2, 24, 0, (intptr_t)copy.size(), copy.data(), 0);
    for (const char *k : {"Saturation", "Filter 2", "Env Amount", "Output", "Cutoff", "Decay", "Vowel", "Formant", "Vowel Mix", "LFO Shape", "LFO Rate",
                          "Seq Rate", "Seq Gate", "Vowel 5", "Level 12", "Seq Steps"})
        CHECK(display(e2, k) == display(e, k), "chunk: %s %s vs %s", k, display(e2, k).c_str(), display(e, k).c_str());
    CHECK(std::fabs(e2->getParameter(e2, param(e2, "Cutoff")) - 0.4321f) < 1e-4f, "chunk: cutoff position not exact");
    std::printf("  chunk %ld bytes: %s, %s, %s, %s, %s\n", (long)len, display(e2, "Saturation").c_str(), display(e2, "Filter 2").c_str(),
                display(e2, "Env Amount").c_str(), display(e2, "Output").c_str(), display(e2, "Cutoff").c_str());
    e2->dispatcher(e2, 1, 0, 0, nullptr, 0);

    CHECK(!bad, "NaN, Inf or denormals in the output");
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
