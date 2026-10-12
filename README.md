# mpc-vst-vfilter

V-Filter: a filter "towards the Virus" as a VST2 **insert effect** for the Akai MPC OS plugin host
(Force, MPC Live/One/X/Key). Made to push the Waldorf Microwave II/XT emulation (or any other
track) in the direction of an Access Virus filter section. Not an emulation: an insert sits
behind the summed voices, so it shapes filter and saturation character, not the oscillators.

Signal path (`vst/vfilter_core.h`): vowel filter (optional) -> pre-emphasis -> filter 1 -> drive + saturation -> filter 2 ->
de-emphasis -> output warmth -> stereo chorus.

- **CUTOFF** (20 Hz .. 18 kHz), **RESONANCE**: two 2-pole low-pass filters in series
- **FILTER 2**: the second filter's offset against the first, -24..+24 semitones
- **DRIVE** (0..30 dB) and **SATURATION** between the filters: OFF, SOFT, HARD, FOLD
- **EMPHASIS**: treble lift into the filter block, taken out again behind it
- **ENV AMOUNT** (-48..+48 semitones), **ATTACK**, **DECAY**, **THRESHOLD**: an insert gets no
  notes, so the input level triggers an attack/decay envelope on the cutoff. 0 = off. Good on
  bass and lead lines; legato notes and chords do not retrigger.
- **WARMTH** (output saturation), **WIDTH** (stereo chorus, 0 = mono-compatible), **OUTPUT**

Vowel / formant filter (tab VOWEL), in front of everything else, for "talking" leads:

- **VOWEL**: morphs A - E - I - O - U (three parallel band-pass filters on the formants F1..F3)
- **FORMANT** (-12..+12 semitones): moves all three formants together, up = smaller, more alien,
  down = bigger and darker
- **FOCUS**: how sharp the formants are (wide and soft .. narrow and nasal)
- **VOWEL MIX**: dry .. vowel filter only. 0 = off, the stage is skipped and costs nothing
- **VOWEL ENV** (-100..+100): the envelope of the ENV tab (ATTACK, DECAY, THRESHOLD) also moves
  the vowel, so every new note says "wow" or "yoi". Works with ENV AMOUNT at 0
- **LFO RATE** (0.1..20 Hz, free-running), **LFO DEPTH**, **LFO SHAPE**: TRI, RANDOM (gliding
  random values) or STEP (a new random vowel per period)

A bright input (saw, pulse, sync) gives the clearest vowels. With CUTOFF low the upper formants
are filtered away again, so open it for speech-like sounds.

Stereo, the nonlinear part 2x oversampled (linear, so heavy DRIVE with FOLD can alias high up).
`vst/test.sh` runs the offline test (clean when everything is off, cutoff/filter 2 darkening,
resonance peak, harmonics per saturation type, envelope, width, channel independence, output
below 0 dBFS, the vowel filter's formants, envelope and LFO, project restore) and a benchmark
(below 1 % of an x86 core with everything on).

The plug-in shell (`vst/vfilter_vst.cpp`) is hand-written like `mpc-vst-rat`'s, no wrapper.
Build/deploy workflow: see `sd88me/mpc-vst-plugins`' `docs/PORTING.md`. License: MIT.
No affiliation with Access Music or Waldorf.

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
