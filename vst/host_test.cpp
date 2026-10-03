/* Offline x86 test of vfilter_vst.cpp (test.sh builds it with ASan/UBSan): clean when everything
 * is off, CUTOFF and FILTER 2 darkening, RESONANCE peaking, DRIVE adding harmonics for each
 * saturation type, the audio-triggered envelope opening the filter, WIDTH (0 = both channels
 * equal), channel independence, loud input below 0 dBFS, chunk restore, NaN/denormal-free output.
 * With "bench" as the second argument it only measures CPU load. Prints PASSED/FAILED. */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
static int automated = 0;
static intptr_t master(AEffect *, int32_t op, int32_t, intptr_t, void *, float) { if (op == 0) automated++; return 0; }
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
                          {"Decay", 0, 100}, {"Threshold", -60, 0}, {"Warmth", 0, 100}, {"Width", 0, 100}, {"Output", -24, 12}};
    for (auto &x : r) if (!std::strcmp(x.n, name)) { e->setParameter(e, param(e, name), (float)(v - x.lo) / (x.hi - x.lo)); return; }
}
/* a sine through the effect; returns the right channel's output (left gets silence unless both) */
static std::vector<float> run(AEffect *e, double f, double amp, double sec, std::vector<float> *left = nullptr, bool leftSilent = false) {
    std::vector<float> outR, il(BS), ir(BS), ol(BS), orr(BS);
    float *in[2] = {il.data(), ir.data()}, *out[2] = {ol.data(), orr.data()};
    static double ph = 0;
    for (int b = 0; b < (int)(sec * SR / BS); b++) {
        for (int i = 0; i < BS; i++) { float s = (float)(amp * std::sin(ph)); ph += 2 * M_PI * f / SR; il[i] = leftSilent ? 0 : s; ir[i] = s; }
        e->processReplacing(e, in, out, BS);
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

    /* 8. chunk */
    set(e, "Saturation", 2); set(e, "Filter 2", 7); set(e, "Env Amount", -12); set(e, "Output", -4);
    e->setParameter(e, param(e, "Cutoff"), 0.4321f);
    void *chunk = nullptr;
    intptr_t len = e->dispatcher(e, 23, 0, 0, &chunk, 0);
    std::vector<uint8_t> copy((uint8_t *)chunk, (uint8_t *)chunk + len);
    AEffect *e2 = mainf(master);
    e2->dispatcher(e2, 10, 0, 0, nullptr, SR);
    e2->dispatcher(e2, 24, 0, (intptr_t)copy.size(), copy.data(), 0);
    for (const char *k : {"Saturation", "Filter 2", "Env Amount", "Output", "Cutoff", "Decay"})
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
