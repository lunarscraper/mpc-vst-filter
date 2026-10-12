// vfilter_core.h (DSP-Kern von V-Filter, MIT-Lizenz, siehe ../LICENSE)
// DSP-Kern fuer ein Insert-Filter "Richtung Virus":
//   Vokal-/Formantfilter (abschaltbar) -> Pre-Emphasis -> Filter 1 -> Drive/Saettigung -> Filter 2 -> De-Emphasis
//   -> Ausgangs-Saettigung -> Stereo-Chorus -> Output
// Cutoff-Modulation: statisch/Automation (Variante 1) + audio-getriggerte
// AD-Huellkurve per Envelope Follower (Variante 2, abschaltbar ueber envAmount = 0).
//
// Vokalfilter: drei parallele Bandpaesse auf den Formanten F1..F3 eines Vokals. VOWEL
// blendet A-E-I-O-U ineinander, FORMANT schiebt alle drei gemeinsam (kleiner/groesserer
// "Rachen"), bewegt von der Huellkurve und einem eigenen LFO. vowelMix = 0 schaltet die
// Stufe ganz ab (der Rest rechnet dann wie vorher).
//
// Framework-neutral, keine Abhaengigkeiten ausser der Standardbibliothek.
// Der nichtlineare Teil laeuft mit einfachem 2x-Oversampling.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

class VirusStyleFilter
{
public:
    enum SatType { kSatOff = 0, kSatSoft, kSatHard, kSatFold };
    enum LfoShape { kLfoTri = 0, kLfoRandom, kLfoStep };

    struct Params
    {
        float cutoffHz    = 2000.0f; // 20 .. 18000
        float resonance   = 0.3f;    // 0 .. 1
        float filter2Semi = 0.0f;    // Offset Filter 2 gegen Filter 1, -24 .. +24 Halbtoene
        float driveDb     = 6.0f;    // 0 .. 30 dB vor der Saettigung
        int   satType     = kSatSoft;
        float emphasis    = 0.3f;    // 0 .. 1  Hoehen-Pre-Emphasis vor dem Filterblock
        float envAmount   = 0.0f;    // -4 .. +4 Oktaven (0 = Follower aus)
        float envAttackMs = 2.0f;    // 0.5 .. 200
        float envDecayMs  = 250.0f;  // 10 .. 4000
        float envThreshDb = -36.0f;  // Trigger-Schwelle, -60 .. 0
        float warmth      = 0.2f;    // 0 .. 1  Ausgangs-Saettigung
        float width       = 0.3f;    // 0 .. 1  Stereo-Chorus-Anteil
        float outputDb    = 0.0f;    // -24 .. +12
        // Vokal-/Formantfilter
        float vowel       = 0.0f;    // 0 .. 1  Position A-E-I-O-U
        float formantSemi = 0.0f;    // -12 .. +12 Halbtoene, alle Formanten gemeinsam
        float focus       = 0.6f;    // 0 .. 1  Guete der Formanten (breit .. scharf)
        float vowelMix    = 0.0f;    // 0 .. 1  (0 = Stufe aus)
        float vowelEnv    = 0.0f;    // -1 .. +1 Huellkurve -> Vokalposition
        float lfoRateHz   = 2.0f;    // 0.1 .. 20
        float lfoDepth    = 0.0f;    // 0 .. 1  LFO -> Vokalposition
        int   lfoShape    = kLfoTri;
    };

    void setSampleRate(float sampleRate)
    {
        fs = sampleRate;
        fsOS = 2.0f * fs;
        emphCoef = 1.0f - std::exp(-2.0f * kPi * 2000.0f / fs);  // Shelf-Ecke ~2 kHz
        lvlAtk = 1.0f - std::exp(-1.0f / (0.001f * fs));          // 1 ms
        lvlRel = 1.0f - std::exp(-1.0f / (0.030f * fs));          // 30 ms
        reset();
    }

    void reset()
    {
        std::memset(ch, 0, sizeof(ch));
        std::memset(delayBuf, 0, sizeof(delayBuf));
        writeIdx = 0;
        lfoPhase = 0.0f;
        level = 0.0f;
        env = 0.0f;
        envStage = kIdle;
        gate = false;
        cutoffSm = 1000.0f;
        ctrlCounter = 0;
        vLfoPhase = 0.0f;
        vLfoFrom = vLfoTo = 0.0f;
        vRng = 0x2545F491u;
        vPosSm = -1.0f;
        vOn = false;
        for (int f = 0; f < kFormants; ++f) { fc_[f] = SvfCoefs(); fGain[f] = 0.0f; }
    }

    void process(const Params& p, const float* inL, const float* inR,
                 float* outL, float* outR, uint32_t frames)
    {
        // Parameter, die nur einmal pro Block gebraucht werden
        // Ohne Saettigung waere Drive nur ein Pegelregler, deshalb dort neutral
        const float drive   = p.satType == kSatOff ? 1.0f : dbToGain(p.driveDb);
        const float makeup  = 1.0f / std::sqrt(drive);
        const float emphG   = 2.0f * clamp(p.emphasis, 0.0f, 1.0f);
        const float deEmph  = 1.0f / (1.0f + emphG);
        const float thresh  = dbToGain(p.envThreshDb);
        const float atkInc  = 1.0f / (std::max(0.5f, p.envAttackMs) * 0.001f * fs);
        const float decCoef = std::exp(-1.0f / (std::max(10.0f, p.envDecayMs) * 0.001f * fs));
        const float warmth  = clamp(p.warmth, 0.0f, 1.0f);
        const float warmG   = 1.0f + 3.0f * warmth;
        const float warmMk  = 1.0f / std::sqrt(warmG);
        const float width   = clamp(p.width, 0.0f, 1.0f);
        const float outGain = dbToGain(p.outputDb);
        const float k       = 2.0f - 1.95f * clamp(p.resonance, 0.0f, 1.0f); // Daempfung
        const float lfoInc  = 0.6f / fs;                 // Chorus-Rate 0.6 Hz
        const float baseDly = 0.012f * fs;               // 12 ms
        const float depth   = 0.003f * fs;               // +-3 ms
        // Vokalfilter
        const float vMix    = clamp(p.vowelMix, 0.0f, 1.0f);
        const float focus   = clamp(p.focus, 0.0f, 1.0f);
        const float vQ      = 3.0f * std::pow(8.0f, focus);            // Guete 3 .. 24
        const float vK      = 1.0f / vQ;
        const float vMakeup = 2.8f * std::pow(4.0f, focus);            // schmale Baender tragen weniger Pegel
        const float vShift  = std::exp2(clamp(p.formantSemi, -12.0f, 12.0f) / 12.0f);
        const float vLfoInc = 16.0f * clamp(p.lfoRateHz, 0.1f, 20.0f) / fs;   // pro Kontrollschritt
        if (vMix <= 0.0005f && vOn) { for (int c = 0; c < 2; ++c) std::memset(ch[c].fm, 0, sizeof(ch[c].fm)); vOn = false; }

        for (uint32_t i = 0; i < frames; ++i)
        {
            float x[2] = { inL[i], inR[i] };

            // ---------- Envelope Follower + AD-Huellkurve (mono, beide Kanaele) ----------
            const float rect = std::max(std::fabs(x[0]), std::fabs(x[1]));
            level += (rect > level ? lvlAtk : lvlRel) * (rect - level);

            if (!gate && level > thresh)            { gate = true; envStage = kAttack; }
            else if (gate && level < thresh * 0.5f) { gate = false; }   // Hysterese

            if (envStage == kAttack)
            {
                env += atkInc;
                if (env >= 1.0f) { env = 1.0f; envStage = kDecay; }
            }
            else
            {
                env *= decCoef;
            }

            // ---------- Kontrollrate: Filterkoeffizienten alle 16 Samples ----------
            if (ctrlCounter == 0)
            {
                cutoffSm += 0.25f * (clamp(p.cutoffHz, 20.0f, 18000.0f) - cutoffSm);
                const float fc1 = cutoffSm * std::exp2(p.envAmount * env);
                const float fc2 = fc1 * std::exp2(p.filter2Semi / 12.0f);
                setCoefs(c1, fc1, k);
                setCoefs(c2, fc2, k);

                if (vMix > 0.0005f)
                {
                    vOn = true;
                    // LFO: Dreieck, weicher Zufall oder Zufallsstufen (ein neuer Wert pro Periode)
                    vLfoPhase += vLfoInc;
                    if (vLfoPhase >= 1.0f)
                    {
                        vLfoPhase -= 1.0f;
                        vLfoFrom = vLfoTo;
                        vRng = vRng * 1664525u + 1013904223u;
                        vLfoTo = static_cast<float>(vRng >> 8) * (2.0f / 16777216.0f) - 1.0f;
                    }
                    float l;
                    if (p.lfoShape == kLfoStep)        l = vLfoTo;
                    else if (p.lfoShape == kLfoRandom) l = vLfoFrom + (vLfoTo - vLfoFrom) * vLfoPhase;
                    else                               l = -tri(vLfoPhase);
                    const float target = clamp(p.vowel + p.vowelEnv * env + 0.5f * clamp(p.lfoDepth, 0.0f, 1.0f) * l, 0.0f, 1.0f);
                    if (vPosSm < 0.0f) vPosSm = target;
                    vPosSm += 0.3f * (target - vPosSm);          // ~1 ms: Stufen ohne Knackser

                    const float pos = vPosSm * (kVowels - 1);
                    const int   i0  = std::min(kVowels - 2, static_cast<int>(pos));
                    const float fr  = pos - static_cast<float>(i0);
                    for (int f = 0; f < kFormants; ++f)
                    {
                        // Frequenz in Oktaven (logarithmisch), Pegel linear ueberblenden
                        const float hz = kFormHz[i0][f] * std::pow(kFormHz[i0 + 1][f] / kFormHz[i0][f], fr) * vShift;
                        const float g  = std::tan(kPi * clamp(hz, 60.0f, 0.45f * fs) / fs);
                        fc_[f].a1 = 1.0f / (1.0f + g * (g + vK));
                        fc_[f].a2 = g * fc_[f].a1;
                        fc_[f].a3 = g * fc_[f].a2;
                        fGain[f]  = (kFormGain[i0][f] + (kFormGain[i0 + 1][f] - kFormGain[i0][f]) * fr) * vK * vMakeup
                                    * (f == 1 ? -1.0f : 1.0f);   // Nachbarn gegenphasig: keine Loecher zwischen den Formanten
                    }
                }
            }
            ctrlCounter = (ctrlCounter + 1) & 15;

            // ---------- Chorus-LFO (Dreieck, L/R gegenphasig) ----------
            lfoPhase += lfoInc;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
            float ph2 = lfoPhase + 0.5f;
            if (ph2 >= 1.0f) ph2 -= 1.0f;
            const float lfo[2] = { tri(lfoPhase), tri(ph2) };

            for (int c = 0; c < 2; ++c)
            {
                Channel& s = ch[c];

                // Vokalfilter: drei parallele Bandpaesse, mit dem Original gemischt
                if (vOn)
                {
                    float v = 0.0f;
                    for (int f = 0; f < kFormants; ++f)
                        v += fGain[f] * svfBp(s.fm[f], fc_[f], x[c] + kDenorm);
                    x[c] += vMix * (softSat(v) - x[c]);
                }

                // Pre-Emphasis: Hoehen anheben, damit die Saettigung oben mehr greift
                s.preLp += emphCoef * (x[c] - s.preLp);
                const float pre = x[c] + emphG * (x[c] - s.preLp);

                // 2x Oversampling: linear hochrechnen, zwei Durchlaeufe, mitteln
                const float up[2] = { 0.5f * (s.prevIn + pre), pre };
                s.prevIn = pre;
                float acc = 0.0f;
                for (int n = 0; n < 2; ++n)
                {
                    float y = svfLp(s.f1, c1, up[n] + kDenorm);
                    y = saturate(y * drive, p.satType) * makeup;
                    y = svfLp(s.f2, c2, y);
                    acc += y;
                }
                float y = 0.5f * acc;

                // De-Emphasis (naeherungsweise invers zur Pre-Emphasis)
                s.deLp += emphCoef * (y - s.deLp);
                y = s.deLp + (y - s.deLp) * deEmph;

                // "Analoge" Ausgangs-Saettigung
                if (warmth > 0.001f)
                    y = softSat(y * warmG) * warmMk;

                // Stereo-Chorus
                delayBuf[c][writeIdx] = y;
                float wet = 0.0f;
                if (width > 0.001f)
                {
                    const float d   = baseDly + depth * lfo[c];
                    const float pos = static_cast<float>(writeIdx) - d;
                    const int   ip  = static_cast<int>(std::floor(pos));
                    const float fr  = pos - static_cast<float>(ip);
                    const float a   = delayBuf[c][ip & kDelayMask];
                    const float b   = delayBuf[c][(ip + 1) & kDelayMask];
                    wet = a + fr * (b - a);
                }
                x[c] = (y * (1.0f - 0.3f * width) + wet * 0.6f * width) * outGain;
            }

            writeIdx = (writeIdx + 1) & kDelayMask;
            outL[i] = x[0];
            outR[i] = x[1];
        }
    }

private:
    static constexpr float kPi = 3.14159265358979f;
    static constexpr float kDenorm = 1.0e-20f;   // gegen Denormals auf ARM
    static constexpr int kDelaySize = 4096;      // reicht bis 96 kHz (15 ms = 1440 Samples)
    static constexpr int kDelayMask = kDelaySize - 1;

    enum EnvStage { kIdle = 0, kAttack, kDecay };

    struct SvfState { float ic1, ic2; };
    struct SvfCoefs { float a1 = 0, a2 = 0, a3 = 0; };
    struct Channel  { SvfState f1, f2; float preLp, deLp, prevIn; SvfState fm[3]; };

    // Formanten F1..F3 (Hz) und ihre Pegel fuer A, E, I, O, U (Maennerstimme, gerundet;
    // die Pegel der oberen Formanten sind angehoben, damit der Vokal auch mit einem
    // Saegezahn deutlich bleibt)
    static constexpr int kVowels = 5, kFormants = 3;
    static constexpr float kFormHz[kVowels][kFormants] = {
        { 700.0f, 1150.0f, 2600.0f },   // A
        { 450.0f, 1750.0f, 2600.0f },   // E
        { 290.0f, 2100.0f, 2900.0f },   // I
        { 420.0f,  800.0f, 2500.0f },   // O
        { 330.0f,  650.0f, 2400.0f },   // U
    };
    static constexpr float kFormGain[kVowels][kFormants] = {
        { 1.00f, 0.70f, 0.45f },
        { 1.00f, 0.55f, 0.45f },
        { 1.00f, 0.50f, 0.40f },
        { 1.00f, 0.60f, 0.30f },
        { 1.00f, 0.45f, 0.25f },
    };

    // TPT-State-Variable-Filter (2-Pol), bleibt auch bei schneller Modulation stabil
    void setCoefs(SvfCoefs& c, float fc, float k) const
    {
        fc = clamp(fc, 20.0f, 0.45f * fs);       // bezogen auf die Basisrate begrenzen
        const float g = std::tan(kPi * fc / fsOS);
        c.a1 = 1.0f / (1.0f + g * (g + k));
        c.a2 = g * c.a1;
        c.a3 = g * c.a2;
    }

    static inline float svfLp(SvfState& s, const SvfCoefs& c, float x)
    {
        const float v3 = x - s.ic2;
        const float v1 = c.a1 * s.ic1 + c.a2 * v3;
        const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
        s.ic1 = 2.0f * v1 - s.ic1;
        s.ic2 = 2.0f * v2 - s.ic2;
        return v2;
    }

    // Bandpass-Ausgang desselben Filters (der Aufrufer normiert mit k auf Spitzenpegel 1)
    static inline float svfBp(SvfState& s, const SvfCoefs& c, float x)
    {
        const float v3 = x - s.ic2;
        const float v1 = c.a1 * s.ic1 + c.a2 * v3;
        const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
        s.ic1 = 2.0f * v1 - s.ic1;
        s.ic2 = 2.0f * v2 - s.ic2;
        return v1;
    }

    static inline float softSat(float x)          // schnelle tanh-Naeherung
    {
        x = clamp(x, -3.0f, 3.0f);
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    static inline float saturate(float x, int type)
    {
        switch (type)
        {
        case kSatSoft: return softSat(x);
        case kSatHard: return clamp(x, -1.0f, 1.0f);
        case kSatFold: {                           // Dreieck-Wavefolder
            float t = x * 0.25f + 0.25f;
            t -= std::floor(t);
            return 1.0f - std::fabs(t * 4.0f - 2.0f);
        }
        default:       return x;
        }
    }

    static inline float tri(float ph)   { return 4.0f * std::fabs(ph - 0.5f) - 1.0f; }
    static inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
    static inline float clamp(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

    float fs = 44100.0f, fsOS = 88200.0f;
    float emphCoef = 0.0f, lvlAtk = 0.0f, lvlRel = 0.0f;

    Channel  ch[2];
    SvfCoefs c1, c2;
    float    delayBuf[2][kDelaySize];
    int      writeIdx = 0;
    float    lfoPhase = 0.0f;

    float level = 0.0f, env = 0.0f;
    int   envStage = kIdle;
    bool  gate = false;

    float cutoffSm = 1000.0f;
    int   ctrlCounter = 0;

    // Vokalfilter
    SvfCoefs fc_[3];
    float    fGain[3] = { 0, 0, 0 };
    float    vLfoPhase = 0.0f, vLfoFrom = 0.0f, vLfoTo = 0.0f, vPosSm = -1.0f;
    uint32_t vRng = 0x2545F491u;
    bool     vOn = false;
};
