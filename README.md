# mpc-vst-vfilter

V-Filter: a filter "towards the Virus" as a VST2 **insert effect** for the Akai MPC OS plugin host
(Force, MPC Live/One/X/Key). Made to push the Waldorf Microwave II/XT emulation (or any other
track) in the direction of an Access Virus filter section. Not an emulation: an insert sits
behind the summed voices, so it shapes filter and saturation character, not the oscillators.

Signal path (`vst/vfilter_core.h`): pre-emphasis -> filter 1 -> drive + saturation -> filter 2 ->
de-emphasis -> output warmth -> stereo chorus.

- **CUTOFF** (20 Hz .. 18 kHz), **RESONANCE**: two 2-pole low-pass filters in series
- **FILTER 2**: the second filter's offset against the first, -24..+24 semitones
- **DRIVE** (0..30 dB) and **SATURATION** between the filters: OFF, SOFT, HARD, FOLD
- **EMPHASIS**: treble lift into the filter block, taken out again behind it
- **ENV AMOUNT** (-48..+48 semitones), **ATTACK**, **DECAY**, **THRESHOLD**: an insert gets no
  notes, so the input level triggers an attack/decay envelope on the cutoff. 0 = off. Good on
  bass and lead lines; legato notes and chords do not retrigger.
- **WARMTH** (output saturation), **WIDTH** (stereo chorus, 0 = mono-compatible), **OUTPUT**

Stereo, the nonlinear part 2x oversampled (linear, so heavy DRIVE with FOLD can alias high up).
`vst/test.sh` runs the offline test (clean when everything is off, cutoff/filter 2 darkening,
resonance peak, harmonics per saturation type, envelope, width, channel independence, output
below 0 dBFS, project restore) and a benchmark (~0.5 % of an x86 core).

The plug-in shell (`vst/vfilter_vst.cpp`) is hand-written like `mpc-vst-rat`'s, no wrapper.
Build/deploy workflow: see `sd88me/mpc-vst-plugins`' `docs/PORTING.md`. License: MIT.
No affiliation with Access Music or Waldorf.

## Hinweis

Entwickelt mit Unterstützung von Claude (Anthropic)
