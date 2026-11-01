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

Step sequencer (tabs SEQ, STEPS 1-8, STEPS 9-16, since 0.3.0) - the vowel filter no longer static:

- 16 steps on the Force's beat grid: the step comes from MPC's song position (ppqPos), so the
  pattern sits on the beat however a clip starts; while MPC stands it runs on at its tempo
- **RATE**: 1/4, 1/8, 1/8T, 1/16, 1/16T, **1/32**, 1/32T, **1/64** - up to four times as fast as the
  Force's 1/16 grid. **STEPS** (1..16): the pattern's length
- per step a **VOWEL** (A-E-I-O-U, top row of the STEPS tabs, on the Q-Links) and a **LEVEL**
  (bottom row)
- **SEQ VOWEL** (0..100): how far the steps pull the vowel from the VOWEL knob to their own
- **SEQ GATE** (0..100): the level ducks the signal (a trancegate, 1.5 ms edges, no clicks)
- **SEQ FILTER** (-48..+48 st): the level opens (or closes) the low-pass CUTOFF - with CUTOFF low
  and SEQ FILTER up, quiet steps are dark and loud ones bright
- **GLIDE** (0..100): slides vowel and filter between the steps (a share of the step length)
- **RANDOM** writes a new pattern (vowels mostly on A E I O U, an accent on the one, a few rests),
  **RESET** brings back the start pattern: the step LFO of the Serum preset "Formant Gate" by
  ultramarinedist (closed steps U/O, open ones E/I)
- SEQ VOWEL, SEQ GATE and SEQ FILTER at 0 = sequencer off (the start state: old projects sound as
  before). VOWEL MIX is on the SEQ tab too: the vowel steps need it above 0

The Formant Gate on a bright synth: VOWEL MIX 100, SEQ VOWEL 100, SEQ GATE 70-80, CUTOFF around
1 kHz, SEQ FILTER +36, RATE 1/16 or 1/32, GLIDE 10-30. Delay 1/8 behind it.

0.3.1: RANDOM and RESET act on every tap (0.3.0 only on the first: the Force sends a tap as a 1 with no 0
after it, and the button waited for one; it now re-arms when it reports its release).

Since 0.3.0 the shell also keeps two host rules of mpc-vst-plugins (tools 2026-10-25, now the
workflow's tools as well): MPC's insert/STOP toggle of parameter 0 (here CUTOFF) is held back
and dropped when it ends where it started, and the trigger buttons are drawn large.

A bright input (saw, pulse, sync) gives the clearest vowels. With CUTOFF low the upper formants
are filtered away again, so open it for speech-like sounds.

Stereo, the nonlinear part 2x oversampled (linear, so heavy DRIVE with FOLD can alias high up).
`vst/test.sh` runs the offline test (clean when everything is off, cutoff/filter 2 darkening,
resonance peak, harmonics per saturation type, envelope, width, channel independence, output
below 0 dBFS, the vowel filter's formants, envelope and LFO, the step sequencer at 1/16, 1/32 and
1/64 on MPC's song position, stopped and without time info, vowel and filter steps, STEPS, RANDOM /
RESET, the parameter 0 toggle, project restore) and a benchmark (below 1 % of an x86 core with
everything on).

The plug-in shell (`vst/vfilter_vst.cpp`) is hand-written like `mpc-vst-rat`'s, no wrapper.
Build/deploy workflow: see `sd88me/mpc-vst-plugins`' `docs/PORTING.md`. License: MIT.
No affiliation with Access Music or Waldorf.

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
