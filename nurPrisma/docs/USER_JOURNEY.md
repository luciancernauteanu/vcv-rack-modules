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
