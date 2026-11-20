# Nur Prisma: user journey and code map

What happens in the code at each step a user takes with the module, so the logic of any feature can be found quickly.
The README is the user manual (what the controls do); this file maps those actions to the functions, the thread they
run on and the state they touch. Search a function name to find it (line numbers drift, names do not).

**Keep it current:** every change to the module updates the matching section here, and the change log at the end.

Last updated: 2026-11-18 (engine).

---

## 0. Ground rules

- **Threads.** The audio thread runs `NurPrisma::process()`. The UI thread runs the widgets; it reads live engine state
  only through atomics (`NurPrisma::corePos`).
- **IDs are append only** (`NurPrisma.hpp` enums): saved patches refer to params and jacks by number.
- **Rack-free DSP** goes in headers under `src/` (namespace `np`), tested offline in `tests/`.

## 1. Files

| File | What it is |
|---|---|
| `src/NurPrisma.hpp/.cpp` | The module (params, `process()`) and its widget `NurPrismaWidget` |
| `src/Layout.hpp` | Panel geometry in mm, mirrored in `res/NurPrisma.svg` |
| `src/widgets/PanelText.hpp` | All panel text, drawn at runtime with the bundled fonts |
| `src/widgets/PrismRing.hpp` | `PrismRing` (SPREAD ring, core dots, glow), `HeroKnob` (CUTOFF), `GoldJack`, `CycleButton` |
| `src/PrismaEngine.hpp` | `PrismaEngine`: the four stereo cores, places, PING, character, level compensation, limiter |
| `src/PrismaCore.hpp` | `Core` (one trapezoidal SVF), `coreCoefs`, `resQ` / `resFeedback` (RES map), `typeWeights`, `pingResponse` |
| `src/Shapes.hpp` | `spreadOffsets` (SHAPE), `SCALES`, `CHORDS`, `quantizeToScale`, `ColoredNoise`, `Rng` |
| `src/Drive.hpp`, `src/Limiter.hpp` | Copied from waveArtifact into namespace `np`: `WarmDrive`, `SafetyLimiter` |
| `tests/test_prisma.cpp` | Offline checks (`np-test`): tuning, ping level, shapes, poly, stereo, stack, BAND, level, per core, safety |

## 2. Turning the hero control

- **CUTOFF** (`HeroKnob`): a knob on top of the ring. Its box is square; `HeroKnob::inside` leaves clicks outside its
  circle unhandled so they reach the ring.
- **SPREAD** (`PrismRing`): a `Knob` drawn as a prism-gradient ring. `drawLayer(1)` adds the glow (stronger with
  SPREAD).
- **Core dots** (`CoreDot`, one per core, `CORE_SPREAD_PARAM + i`): small `Knob`s added last (on top). `step()` moves
  each to `PrismRing::dotPos(i)`: on the ring at the core's live frequency (`corePos`); a dot landing on an earlier
  one steps 2.2 mm inward so all stay grabbable. Dragging sets that core's own spread (-1 .. 2, default 1).

## 2b. Focus on one core (core bullets)

- `CoreSelector` (under the hero): ALL pill + four bullets. Left-click -> `NurPrisma::setFocus(core)` (again or ALL ->
  -1). Right-click a bullet -> `followGlobal` / `resetCore`.
- `NurPrismaWidget::addFocusKnob` makes, for TYPE / RES / BAND / WIDTH / MIX, one global knob plus four per-core twins
  (`CORE_TYPE / CORE_RES / CORE_BAND / CORE_PAN / CORE_LEVEL_PARAM + i`) at the same place; the ring is made five
  times too (`SPREAD_PARAM`, `CORE_SPREAD_PARAM + i`). `step()` shows the set of the focused core. `PanelText`
  `addFocus` labels swap (WIDTH -> PAN, MIX -> LEVEL) and take the core's colour.
- Follow / own: `setFocus` copies `followValue(core, k)` into the core's twins it does not own yet, so focus starts
  from the current sound: the global TYPE / RES / BAND, the core's SPREAD place for CUTOFF (`engine.place[i]`, knob
  units) and WIDTH's placement for PAN (`widthPlace`). The centre knob is a focus set too (`F_CUTOFF`), and a core
  dot with an own cutoff drags `CORE_CUTOFF_PARAM` instead of its spread (`CoreDot::step` swaps `paramId`).
- Engine: an own cutoff puts the core at `coreCutoff` (+ quantize, V/OCT, character), without
  CUTOFF or its SPREAD offset; while not owned `s.coreCutoff` tracks the place, so taking it is seamless. An own pan
  replaces WIDTH's balance gains with mid / side panning (`panMid`, `panSide`): mid by the same sqrt law, side
  narrowed by 1 - |pan|, so a stereo core moves as a whole. `checkOwnership()` (UI thread, every frame) marks a twin owned (`own[i]` bits
  `OWN_TYPE / RES / BAND / CUTOFF / PAN`, atomic) once its value changes (knob, MIDI map, automation). `readParams` passes
  `ownType / coreType...` to the engine; the engine smooths each core's effective value (`s.coreType[i]` = own ?
  core : global). Level and pan are always per core (100% / centre = as before). Focus and `own` are saved in the
  patch. At defaults (all follow) the output is bit-identical to before (6 golden scenarios).

## 3. Every sample (`NurPrisma::process`, audio thread)

1. Every `PrismaEngine::BLOCK` (16) samples `readParams()` copies knobs, menu settings and CV into `engine.p`
   (V/OCT + CUTOFF CV in octaves and RES CV per core: poly channel i -> core i), sets the SHAPE lights
   and publishes `corePos` for the ring.
2. PING: a Schmitt trigger per channel. Poly -> `engine.ping(c % 4, velocity)`; mono -> `engine.pingAll(velocity)`,
   which follows the ping mode (All = strummed by STRUM in pitch order, Cycle, Random). Velocity = volts / 10.
3. Inputs (5 V = 1.0): with 2+ channels and "one voice per core" on, `routeVoices` puts voice v in core v mod 4;
   else one signal feeds all cores. R unpatched = L.
4. `PrismaEngine::process`: `updateBlock()` every 16 samples ->
   pending pings fire (`PingBurst`, injected into the core state with `Core::excite`) -> noise -> `DriveStage`
   (constant 31-sample latency) -> `runCores` (or `runCoresDigital`, noinline) -> gain compensation -> MIX with the
   dry delayed to match -> `SafetyLimiter` (10 V, DC block, NaN guard).

## 4. Control rate (`PrismaEngine::updateBlock`)

- Knobs are smoothed (5 ms); CV is added unsmoothed.
- Each core's pitch (log2 Hz): CUTOFF (+ quantize to scale) + `spreadOffsets` (SPREAD + CV) x each core's own spread
  (`coreSpread`) + V/OCT/CUTOFF CV + ANALOG mismatch - pitch bloom (ring level sags the pitch a little) +- WIDTH's small left/right detune.
- `coreCoefs` -> integrator gain g (interpolated per sample across the block), damping, BP gain, feedback.
  DIGITAL quantizes g and k (`quantizeCoef`), sets the sample-and-hold length and the state step.
- BAND: `bandOctaves` (3 -> 1/24 octave), `bandStageK` (two 2-pole band-passes in series, 12 dB/oct skirts),
  `bandMakeup` (up to about +10 dB), faded in over the first 10% of the knob. `runCoresBand` / `runCoresDigitalBand`
  (noinline) run `BandStage` after each core; at 0 the plain `runCores` runs, bit-identical to before BAND existed.
  Pings are scaled down by the band's gain so they ring as loud as without it.
- RES: `resQ` (0.7 .. 300 at 92%, squared curve), then `resFeedback` lowers the damping; the very top self-oscillates
  (clip on the raw state v1, settles near 3.7 V).

## 5. Ping

- `pingAll` sorts the cores by pitch; STRUM gap = STRUM^2 x 150 ms, up or down. `pending[]` counts down per sample.
- `PingBurst::start`: amplitude = velocity x `PING_LEVEL` / `pingResponse` (so LP / HP ring at ~0.9 x 5 V whatever
  the pitch or RES). The mallet is an impulse through a two-pole "felt" (STRIKE 0..50%: corner 20 kHz -> 300 Hz),
  then a noise burst (breath) takes over to 100%.
- Level compensation does not learn for 1 s after a ping (the ring is not in the input).

## 6. Level compensation (`PrismaEngine::updateLevelComp`, once per block)

- Input and wet energies are followed per sample (300 ms). With input (above `COMP_GATE`, ~16 mV RMS) and no ping
  ring, the target is the ratio in dB, limited by the menu setting (`EngineParams::levelComp`: Off 0 dB, Gentle
  +-6 dB, Full -12..+24 dB), reached with 150 ms smoothing; the linear gain steps across the block (`compStep`).
- No input: the gain holds (gaps in the music do not swell); after 2 s it drifts back to 0 dB over ~1 s, so noise
  or self-oscillation alone is never left boosted. MIX blends the compensated wet with the dry.

## Change log

- 2026-11-20: gain and noise follow. (1) Measured ANALOG 1 + 24 dB at RES 85% (click ping, flat detector): ping -5.9 dB,
  noise -12.6 dB, input -2.5 dB. Fixes: bass loss coefficient 1.8 -> 0.5; `slopeNoiseComp` (noise gets back the second
  stage and loss, plus ANALOG's level-dependent squash); ring and the rest get `coreGain *= 1 + slopeShare an resN^2`
  at the output (a clipped ring cannot be boosted at the strike); pingResp /= 1 + 0.2 an resN^2. After: ping -0.8,
  noise +0.8, input -1.6 dB. (2) `EngineParams::noiseFollow` (menu, saved): per-core `followGain` from the ping
  envelope `pingEnv` (min(1, 2 x velocity), decays at half the ring rate exp(-pi fc k t / 2)) and/or the input follower
  `followIn` (x8), smoothed per block, applied to each core's noise (`noiseCore`); `noiseLive` lets idle skip when the
  noise is gated off. Following the core's state instead latched on (noise excites the ring, the ring opens the
  noise: 10 V forever) - never derive a noise gate from a state the noise feeds. (3) Review fix: the ring shaper is
  crossfaded by the drive amount (`ringMix`); a straight tanh at DRIVE 1% bent a 5 V sine to H3 -23 dB (now -49 dB,
  level unchanged). 147 offline checks.

- 2026-11-20: DRIVE before the cores is heard on pings. A ping bypasses the input drive and a ring is a clean resonance
  whatever it is fed (measured: even a 8x louder strike left H2 / H3 at -43 / -49 dB), and the strike shaping cannot
  change a one-sample click. Now each core's output goes through `tanh(g y) / sqrt(g)` (g = DRIVE 1..12, before / both
  at 70%) in the SAT instantiation (`ringDriveOn`, flag raised by `resSat > 0 || ringDriveOn`): click ping 3rd
  harmonic -45 -> -10 dB at full drive, ring loudness within 0.35 dB (rms), after-the-cores unchanged; input drive
  unchanged (THD -7.7 dB at 100%). Tried and dropped: lowering the state saturation limit (it only squashed the ping).
  The old assumption "before = the cores filter the drive's harmonics" no longer holds. 144 offline checks.

- 2026-11-20: digital and analog feel. (1) `ShiftNoise` (Shapes.hpp): LFSR noise, register 17..3 bits stepped by COLOR,
  maximal period 2^n - 1 (tested), +-1 x 0.3; `EngineParams::noiseType` (menu, saved). (2) PCM gate: `digitalGate` =
  (DIGITAL - 0.75) x 0.24 (engine units), per sample, in the DIGITAL block of the core loop. (3) Thermal drift:
  `thermalNow` (per core two random walks, targets every 7-20 s, smoothstep) adds +-4 cents of pitch and +-8% of Q
  under ANALOG. (4) `LoudnessWeight` (1-pole HP 120 Hz + 4 dB shelf at 1.7 kHz) on both sides of the level
  compensation (`weightIn/Out`, `EngineParams::levelWeight`, menu, saved; default ear; flat = the old RMS).
  Measured: ear mode matches weighted loudness within 0.1 dB (LP 200 Hz / HP 5 kHz / BAND 80%; flat was -4.2 / +3.4 /
  +1.2 dB), thermal drift 6.9 cents over 60 s, PCM gate zeroes a quiet sine at 100% and nothing at 70%. 141 offline
  checks. CPU: 111 ns/sample at defaults (+6 for the weighting).

- 2026-11-19: pings react to DRIVE and ANALOG. `PingBurst::start(..., shapeG)`: when shapeG > 1.01 the whole strike is
  generated once into `buf`, saturated (tanh of gain shapeG relative to its own peak), energy-matched back, and
  replayed (`buffered`); otherwise the streaming path is unchanged (bit-identical at DRIVE 0 / ANALOG 0).
  shapeG = exp(preAmt ln 12) x (1 + 1.5 ANALOG), preAmt = DRIVE before the cores (70% in Both, 0 for After: the post
  stage drives the ring). ANALOG also scales each ping by 1 + 0.12 ANALOG rnd. Measured: 5 kHz ring +3.8 dB at DRIVE
  100%, low ring +1.6 dB; ping strength spread 0.8 dB at ANALOG 1 (0.03 at 0). 133 offline checks.

- 2026-11-19: 24 dB slope per core (`EngineParams::slope24`, module `coreSlope`, saved; core bullet menu + module menu).
  `LowStage` (a 12 dB TPT low-pass, Q 0.7, same g as the core) after the core: the LP share (TYPE's LP weight,
  `slopeShare`) goes through it. Ladder bass loss: `slopeGain` = 1 - share (1 - 1 / (1 + 1.8 ANALOG (RES / 0.92)^2)).
  Pings scale for the stage (0.707) and the loss. The core-loop dispatch is now a 32-case flag switch
  (`runCoresAny`: DIGITAL, BAND, ANALOG saturation, LPG, S24 as compile-time flags). Measured: 2 octaves above the
  cutoff -49 dB (12 dB core -24.5 dB); passband and BP unchanged; bass -8.3 dB at ANALOG 1 / RES 88%; pings in tune,
  +0.25 dB. 130 offline checks. CPU: 148 ns/sample with all four cores at 24 dB (104 otherwise).

- 2026-11-19: series stacking no longer piles up level (found with the user's patch: STACK 100%, SPREAD 0, BAND 100%,
  core LEVELs 120-163%, NOISE 33%, a ping into it: the resonator was far too loud). Three causes, all in the series
  chain: (1) per-core LEVEL and the analog gain mismatch were applied inside the chain and compounded (four levels
  = +12 dB): now they scale what a core adds to the output, the chain carries the core's own output; (2) BAND's
  make-up gain compounded the same way (about +47 dB at BAND 100%): now `bandOut` on the output contribution only;
  (3) cores on one pitch multiply their resonances in series (Q^4): each core's ping is scaled by what the cores after
  it add at its pitch (`seriesNorm`, analytic `coreMagnitude`, only a reduction, weighted by STACK) and its noise by
  the same (`noiseScale`, `noiseCore`). Same-pitch LP cores at RES 72%: ping peak 10 V -> 1.1 V, continuous noise in
  series now as loud as in parallel. Stack 0 is unchanged; the golden scenario that pings in series (3) changes on
  purpose. 123 offline checks.

- 2026-11-19: "Quantize to the scale" now snaps every core's place (CUTOFF + its SPREAD offset), not only the CUTOFF
  knob (`updateBlock`: `po` for cores without an own CUTOFF); SHAPE Chord and Scale are left alone (they build their
  notes from the root). Cores with an own CUTOFF were already snapped. Test: all four cores within 0.001 cents of a
  C-major note for Even / Random / Harmonic at four SPREAD amounts; off leaves them free; a Chord in D keeps its
  major third. 119 offline checks.

- 2026-11-19: low-pass gate (append-only `LPG_PARAM`, `LPG_DECAY_PARAM`; source `lpgSource` in the menu / patch).
  One vactrol per core (`lpgV`): lit by the ping pulse (6 ms, set where the ping fires) and / or the core's input
  level (`lpgEnv`, 30 ms follower; t = min(1, sqrt(10 env))); rises in 2 ms, falls with tau = DECAY x (0.3 + 2.2
  (1 - v)) (DECAY 40 ms .. 1.5 s). Open amount o = 1 - depth (1 - v): gain o^2 and a one-pole cutoff 40 Hz .. 20 kHz
  (log), both worked out per block and interpolated per sample. Applied after each core (and BAND / DIGITAL) before
  the series chain and the pan. Only in the LPG instantiations of `coresImpl` (`runCoresLpg`), so depth 0 is
  bit-identical (golden). The level compensation holds 0 dB while the gate is on; the engine idles cores as before.
  Measured: ring to -20 dB 69 ms with a fast gate (208 ms without), 159 ms slow; the release slows as it closes;
  ping peak -1.4 dB; click-free onset. 114 offline checks. CPU: +17 ns/sample (124).

- 2026-11-19: silence and CPU when nothing plays. ANALOG's noise floor (hiss) now follows activity: `hissGate`
  (per block, 50 ms) = (input RMS + strongest ring) / 0.02, fully open while a core self-oscillates (so RES at the
  top still starts singing on its own). Nothing connected at ANALOG 84 % / RES 85 % went from -73 dB to digital
  silence. Idle skip: with no input, noise, hiss or ping, rings below -200 dB and no self-oscillation, the cores are
  skipped (`coresIdle`, states cleared once) and restart from exactly zero; dispatch moved to `runCoresAny`.
  Golden identical. CPU: idle 50 ns/sample (was ~107), playing unchanged. 107 offline checks.

- 2026-11-19: SHAPE Bar removed (7 shapes; released values 0-4 unchanged, Bell = 5, Vowel = 6). Random's LED is
  `RandomLed`: bigger, prism rainbow, centred, glows while Random is selected; click = select Random +
  `rerollRandomShape` (`seedRandomShape(seed, glide = true)`: places glide to the new targets, 40 ms time constant).
  104 offline checks.

- 2026-11-19: an own CUTOFF is a detune from the core's *place* (`engine.place[i]`: CUTOFF + SHAPE / SPREAD offset),
  kept by `readParams` (`cutoffOffset`, saved as `cutoffDetune`), so CUTOFF, SPREAD and SHAPE move every core (before:
  owned cores sat at a fixed frequency and SHAPE did nothing once all four were owned). Menu: "All cores follow the
  global knobs again". Focus probe: SHAPE Even -> Harmonic moves an owned core exactly with its place (0.900 oct).

- 2026-11-19: new SHAPEs (enum appended: `SHAPE_BAR`, `SHAPE_BELL`, `SHAPE_VOWEL`) in `spreadOffsets`; Bar / Bell
  exact at SPREAD 50% (2 s log2 ratio, like Harmonic); Vowel interpolates `VOWELS` (F1-F4, A E I O U) in log
  frequency, offsets relative to 1 kHz. Just tuning (`EngineParams::justTuning`, menu, saved): `justOctaves` maps
  semitones to 5-limit ratios (7/4 for Dominant 7) for SHAPE Scale and Chord. SHAPE shows 8 LEDs and its name
  (`PanelText::addDynamic`, + JUST). Default (Even, equal) bit-identical. 103 offline checks. CPU unchanged.

- 2026-11-19: DRIVE voicings and position. `Drive::voice` (Warm = the original code, untouched; Diode / Fold run
  `processVoiced`: same 2x oversampled ADAA path with `curve` / `curveInt`). Diode: negative side tanh, positive side
  8 tanh(u/8), no bias (H2 -10.6 dB at full vs -17.7 Warm); Fold: sin (H5 +1.6 dB). `WarmDrive::tone`: one-pole high
  cut growing with the amount, Diode / Fold only. `EngineParams::drivePos`: PRE / POST (`postDrives`, on the wet
  before the level compensation) / BOTH (70% each); the dry delay follows (31 or 62 samples) so MIX stays aligned.
  A voicing / position change fades out 5 ms, switches, waits for the drive paths to fill, fades in (click-free).
  Menu + saved in the patch; the DRIVE label shows the choice (`PanelText::addDynamic`) and is a control
  (`DriveLabelButton`, over the label): click = next voicing, hold 0.5 s = next position (fires while held, in
  `step()`), hover = underline + tooltip; right-click falls through to the module menu. The knob stays a plain knob. Defaults bit-identical
  (6 golden scenarios). 96 offline checks. CPU: POST 300 ns, BOTH + Fold 367 ns (defaults 105).

- 2026-11-19: level-dependent resonance (ANALOG). `Core::saturatedK` raises each core's damping with its own ringing
  level, k (1 + RES_SAT x ANALOG x v1^2) (v1^2 capped at 16), like an OTA / ladder loop running into saturation.
  It runs only in the SAT instantiation of `coresImpl` (`runCoresSat`, chosen when ANALOG > 0), so ANALOG 0 is
  bit-identical to before (6 golden scenarios). Measured: quiet input linear (-0.42 dB between -66 and -54 dBFS),
  loud input squashed 9.3 dB more than at ANALOG 0, a hard ping settles to -20 dB in 0.26 s (0.30 s at ANALOG 0),
  self-oscillation bounded (0.56 = 2.8 V). 90 offline checks. CPU: 105 ns/sample at defaults (+10).

- 2026-11-19: fixes from testing in Rack.
  - Ping clicks: `Core::excite(amount, bpShare)` puts the ping into the band state for LP / HP and into the low state
    for BP / notch (by TYPE weight), so every output's ring starts from zero: the onset is no steeper than the ring
    (1.05x, was a full-level step on BP), also when re-pinging a ringing core.
  - STACK series kept the NOISE only in core 1: each core's series input now gets the noise too (`noiseNow`).
  - CUTOFF in ALL moves every core: `readParams` keeps each own-CUTOFF core at CUTOFF + its offset
    (`cutoffOffset`, saved in the patch). The offset is remembered apart from the value it plays at, so a core
    pinned at 20 Hz / 20 kHz returns exactly to where it was set when CUTOFF comes back. An edit we did not write
    (`cutoffWritten` differs: knob in focus, MIDI map) sets a new offset.
  - "Quantize CUTOFF to the scale" shows SCALE (gold) inside the bottom of the ring (`PanelText::addIf`).
  - DRIVE: gain 1..12 (was 1..3), bias 0.3, no make-up: H3 -21 dB at 42% (was -27), -11 dB at full.
  85 offline checks + focus checks on the module. CPU unchanged (95 ns/sample at defaults).

- 2026-11-18: MOTION moved to its own branch (`feat/nur-prisma-motion`): this branch has no MOTION column, no LFOs and
  no clock, and the panel is back to 18 HP (the original concept). The MOTION param / jack / light IDs stay reserved
  in the enums (not configured, no widgets), so patches are the same on both branches. 74 offline checks.

- 2026-11-18: per-core CUTOFF (append-only `CORE_CUTOFF_PARAM` x4, centre knob in focus) and PAN that replaces
  WIDTH's placement (was added to it, so a core could not reach hard left) and moves both channels. 77 offline
  checks, 16 focus checks on the module, golden identical while nothing is owned. CPU unchanged (102 ns/sample).

- 2026-11-18: per-core TYPE, RES, BAND, LEVEL and PAN (append-only `CORE_TYPE / RES / BAND / LEVEL / PAN_PARAM` x4),
  edited through the fixed core bullets (focus); own / follow per core, saved. Character row moved to y 87 with a
  tighter label gap; hero label smaller at y 75. 73 offline checks + 12 focus checks on the real module; golden
  identical; CPU unchanged (100 ns/sample at defaults).

- 2026-11-18: each core's own spread (append-only `CORE_SPREAD_PARAM` x4), set by dragging its dot on the ring.
  At 100% (default) the output is bit-identical to before (6 golden scenarios). 68 offline checks.

- 2026-11-18: level compensation reworked: matches the input's loudness up to +24 dB (was +-6 dB), holds through
  silence then relaxes to 0 dB; right-click "Level compensation" Off / Gentle / Full (default Full, saved in the
  patch). Measured on pink noise: BAND 100% +0.06 dB, narrow BP +0.01, notch -0.24, HP 5 kHz +0.08, LP 200 Hz
  -0.37 (was -8.35 dB for BAND without it). 66 offline checks. CPU unchanged (101 ns/sample at defaults).

- 2026-11-18: BAND (append-only param `BAND_PARAM`): each core can be narrowed to a steep band, for a drastic filter
  bank. Panel: BAND left of the hero above STACK; DRIVE moved to the character row (5 knobs). BAND 0 output
  bit-identical to before (6 golden scenarios); 57 offline checks. CPU: BAND 70% 169 ns/sample (+57), poly 203.

- 2026-11-18: engine. `PrismaEngine` with all controls of the panel, offline tests (50 checks), module wiring, menu
  (ping mode, scale, chord, quantize CUTOFF, re-roll Random / character, poly split), seeds saved in
  the patch. CPU: 112 ns/sample at defaults, 155 poly, 302 all on, 867 DRIVE + poly (8 oversampled drives).

- 2026-11-18: scaffold. Panel, all params and jacks, ring and labels. `process()` passes audio through and fills
  `corePos` from CUTOFF / SPREAD (Even spacing) until the engine lands.
