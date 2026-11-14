# Wave Artifacts: user journey and code map

What happens in the code at each step a user takes with the module, so the logic of any feature can be found quickly.
The README is the user manual (what the controls do); this file is the map from those actions to the functions, the
thread they run on and the state they touch. Search a function name to find it (line numbers drift, names do not).

**Keep it current:** every change to the module updates the matching section here, and the change log at the end.

Last updated: 2026-11-14.

---

## 0. The ground rules of the code

- **Threads.** The audio thread runs `process()` of each module. The UI thread runs widgets, menus, `uiStep()`, file
  loading and analysis starts; analysis itself runs on a worker thread per sample slot. Only the UI thread frees memory
  (banks, hit lists); the audio thread hands old ones back through `retired*` atomics.
- **State lives in params** (marker on/off, positions, every per-artifact setting), so it is saved, undoable and read by
  `process()` without locks. Param and enum IDs are **append only** (`WaveArtifact.hpp` enums): old patches keep loading.
- **Patch format flags in `dataToJson`:** `artifactSamples` (each artifact's own file), `artifactEngines` (each artifact
  has its own engine; without it `dataFromJson` copies the old ENGINE into all five), expander `cvSlots`.
- **Engines are Rack-free headers** (`MorphEngine.hpp`, `GrainEngine.hpp`, `SamplePlayer.hpp`, `PolyVoices.hpp`,
  `Filters.hpp`, `Limiter.hpp`, `Lfo.hpp`, `StepLocks.hpp`, `LoopLayout.hpp`, `Transients.hpp`, `MarkerMod.hpp`), tested
  offline in `tests/`. The module (`WaveArtifact.cpp`) turns params into engine inputs every sample.
- **Settings at control rate.** `refreshSettings(i)` reads each artifact's knobs / step locks / CV into
  `MarkerSettings settings[i]` every `SETTINGS_INTERVAL` (32) samples, at once when its locks change or glide, or when a
  bank changes. Events (clock, trigs, gates, note starts) stay sample exact.
- **Click-free rule.** Anything that changes what sounds fades: whole engine 5 ms (`holdSilent`), one artifact 5 ms
  (`markerHold` -> `markerGate` / `cloudGate`), RETURN 20 ms (`insertS`), mute through the gain smoothing.

## 1. Files

| File | What it is |
|---|---|
| `src/WaveArtifact.hpp/.cpp` | The main module (params, `process()`, loading, markers, locks, LAY) and its widget `WaveArtifactWidget` |
| `src/MorphEngine.hpp` | WAVE engine: wavetable oscillators, MORPH crossfade, POLY voices, per-artifact chains |
| `src/GrainEngine.hpp` | GRAIN engine (grain clouds) and SAMPLE engine (clouds as read heads, `SamplePlayer.hpp`) |
| `src/SamplePlayer.hpp` | `PlayerVoice`: one read head (ONE / LOOP / PING, crossfades, BEAT / FIT lengths) |
| `src/WavetableBank.hpp/.cpp` | `analyzeWav`: a WAV into `SampleBuffer` (the sample, mip levels) and wavetables |
| `src/Transients.hpp` | `findTransients` (hits, HITS slider), `onsetStrength`, zero crossings, `hitPeakDb` |
| `src/LoopLayout.hpp` | LAY: tempo from audio / name, `layoutRow`, `layoutTrigs`, `laySnapSteps`, `laySelection`, velocities |
| `src/PolyVoices.hpp` | ADSR, `MarkerTrigger` (playhead fires markers), `MarkerStepper` (STEP / ALL), `PolyVoices` |
| `src/StepLocks.hpp` | Sequencer step data: locks, trig / lock conditions, slide (`MarkerLocks`, `StepData`, `LOCK_NAMES`) |
| `src/MarkerMod.hpp` | Each artifact's expander LFO + AD routing (`ModSum`, `ModEnvelope`) |
| `src/Lfo.hpp` | `Lfo`, `ClockTracker` (CLOCK in / internal BPM), `LfoSmoother`, `fold01`, `lfoCentered` |
| `src/Filters.hpp` | `Svf`, `FilterParams`, `Drive` / `WarmDrive` (2x OS, ~31 samples latency), `MasterFilter` |
| `src/Limiter.hpp` | `SafetyLimiter`: 1.5 ms look-ahead, DC blocker, NaN guard, clamp at 10 V |
| `src/ExpanderMessages.hpp` | All messages between modules (`ToExpanderMessage`, `ToMainMessage`, sequencer messages), CV destinations |
| `src/WaveArtifactSequencer.hpp/.cpp` | The step sequencer (left of the main module) and its widgets `StepGrid`, `LockLane` |
| `src/WaveArtifactExpander.hpp/.cpp` | The expander (right): one artifact's CV / GATE / V/OCT / LFO out |
| `src/WaveArtifactSendReturn.cpp` | Stereo SEND / RETURN module (end of the expander chain) |
| `src/widgets/*.hpp` | Panel widgets: `MarkerBar` (bar, slots, file line), `WavetableMap`, `AdsrScreen`, `ModeButton`, `LinkedKnob`... |
| `tests/test_*.cpp` | Offline tests (`make run` in `tests/`) |

## 2. Adding the module, opening and saving a patch

| Step | Code |
|---|---|
| Module created | `WaveArtifact::WaveArtifact()`: `config*` of every param (per artifact in a loop), lock tables (`lockIndex`, `lockJumpMask`, `settingMin/Max`), expander message buffers |
| Patch / preset loads | Rack: params, then `dataFromJson`: old-engine migration (`artifactEngines` missing -> ENGINE into every `MARKER_ENGINE_PARAM`), table mode / tone / mono, `slotFileFromJson` per slot (marks `savedPending`) |
| Samples come back | `uiStep()` -> `loadSavedSample(slot)`: the patch's stored copy (`patchSampleName`: `sample.wav`, `artifactN.wav`) if its FNV hash (`wavHashOf`) matches, else the original path; an artifact saved without a file is cleared |
| Save (Ctrl+S, quit) | `onSave`: writes each slot's WAV into the patch storage folder once per change (`storedPath` / `storedHash`). Rack's 15 s autosave does **not** call `onSave` (only `toJson`), so the stored copies update on save / quit only |
| Initialize | `onReset`: all knobs default, every slot cleared (engine locked, so banks are freed here) |
| Randomize | `onRandomize`: params with `randomizeEnabled` (excluded: marker on/off, position, mute, engine, play mode, LFO shapes and most switches), then the expanders in chain order |

## 3. Loading a sample

Slots: `slots[0]` is the main sample; `slots[1 + i]` is artifact i's own. An artifact without its own file plays the
main one (`slotOf(i)` UI side, `markerSlot[i]` audio side).

| Step | Code |
|---|---|
| Click the screen's file line (or the empty screen) | `MarkerBar::onButton` / `onDragEnd` -> `openLoadDialog(module, screenLoadSlot())`: the **selected artifact's own** slot; the main slot only while nothing is loaded at all |
| Screen arrows < > | `MarkerBar::onButton` -> `screenStep(dir)`: next WAV in the folder of what the selected artifact plays, into its own slot (stepping from the main sample gives it its own) |
| Artifact menu: load / previous / next / use the main sample | `MarkerMenu.hpp addArtifactSampleItems` -> `openLoadDialog(m, 1 + i)`, `loadSibling(dir, 1 + i)`, `clearArtifactSample(i)` |
| Module menu "Load WAV (main sample)" | `openLoadDialog(m)` (slot 0), `loadSibling(+-1)` |
| Read the file | `loadFile(path, slot)`: size checks (`MAX_FILE_BYTES`), `wavBytes`, `wavHash`, `fileName`, then `startAnalysis(slot)` |
| Analysis | `startAnalysis`: worker thread -> `wa::analyzeWav` (WavetableBank.cpp) -> `pendingBank` (generation check drops stale results) |
| The new bank plays | `process()`: main slot swaps when the engines are silent (`swapWanted` fades everything 5 ms); an artifact's own slot swaps when that artifact is silent (`markerSwap[i]`, checked every 32 samples) and sets `markerSlot[i]`; the old bank goes to `retiredBank`, freed by `uiStep()` |
| Hits for HIT / LAY | `uiStep()` -> `updateTransients(slot)` (only when an artifact on GRAIN / SAMPLE plays that file: `slotHeardAsSample`) -> `wa::findTransients(sample, HITS slider)`; handed to audio via `hitsPending` -> `hitsActive` |
| Status line | `setStatus` / `getStatus(slot)`; `MarkerBar::drawStatus` prefixes "Main:" when the selected artifact plays the main sample and another has its own |

## 4. Artifacts (markers): add, select, move, mute, copy, link

| Step | Code |
|---|---|
| Click the empty bar | `MarkerBar::onButton` -> `addMarker(pos)` (lowest free marker, defaults, table under the position) |
| Drag a bullet / the playhead | `MarkerBar::onDragMove`: `MARKER_POS_PARAM + i` / `POSITION_PARAM` |
| Select (bullet or slot) | `selectedMarker` (UI only); the edit knobs of that artifact are shown (`editKnobs[i][k]` visibility in `WaveArtifactWidget::step`) |
| Double-click a bullet | `toggleMuteWithUndo` -> `MARKER_MUTE_PARAM` (engine gain 0 through its smoothing) |
| Right-click a bullet / slot | `MarkerBar::openMarkerMenu` (`MarkerMenu.hpp`): delete (`deleteMarker`), copy / paste (`copyMarker` / `pasteMarker` over `MARKER_SETTING_PARAMS`), sample items, LAY |
| Shift + a control | `WaveArtifactWidget::step` finds the Shift-held / hovered param -> `linkStep(paramId, dragging)`: every other active artifact moves by the same amount (choices take the same value); undo in `pushLinkedMoveUndo` |
| Shift + double-click | `resetSettingOnAllMarkers` |
| The per-artifact setting list | `MARKER_SETTING_PARAMS[NUM_MARKER_SETTINGS]` (54): copy / paste, links, step locks (same order as `StepLocks.hpp LOCK_NAMES`; `NOT_LOCKABLE` marks switches and ENGINE) |

## 5. Engine per artifact (WAVE / GRAIN / SAMPLE)

| Step | Code |
|---|---|
| ENGINE button | One `ModeButton` per artifact on `MARKER_ENGINE_PARAM + i`, only the selected one shown (`engineButtons[i]`); Shift + click goes through `linkStep` (sets every artifact). Right-click menu: "Engine (artifact N)", "Engine (every artifact)" |
| What the knobs mean | The selected artifact's engine: `shownEngine()`; the old `ENGINE_MODE_PARAM` mirrors it (set in `WaveArtifactWidget::step`) so `PanelLabels`, `WavetableMap`, `ArtifactSections` follow; per-artifact readers use `MARKER_ENGINE_PARAM + i` (`MarkerBar`, `ScanLength`, `WaveQuantity::grain`, `sampleBeatMode`, sequencer) |
| Which engines run | `process()`: at the settings rate `wantEngine[i]` and `wantLayout` (bit 0 WAVE = `engine`, bit 1 GRAIN / SAMPLE = `grainEngine`, from the active artifacts). A new layout fades the whole module (`switching` -> `holdSilent`), then every `engineOf[i]` takes its wanted engine |
| One artifact changes engine (same layout) | `handover[i]`: it fades out alone in the engine it leaves (`markerHold`), then `engineOf[i]` moves once `isMarkerSilent(i)`; GRAIN <-> SAMPLE is per cloud in `grainEngine` (`GrainInput::playerOf`, `cloudPlayer[m]`, `isPlayer(m)`) |
| Another engine's artifacts | Held silent there: gain 0, `markerHold`, no note, no return; they keep their place on the bar (MORPH neighbours are the same in both engines). WAVE: `EngineInput::otherEngine` -> in `renderMorph` a neighbour on the other engine is not morphed into; this engine plays its own at the equal-power share (`crossLevel`) |
| WAVE engine starting again | `wavePrime` (4 ms): `EngineInput::prime` renders silently with `holdSilent`, so the drive's latency fills with the live sound before the fade-in |
| Master section and output protection | Always once, in the module: both engines return their mix before their own master (`rawOut`); `mixMaster` (`MasterFilter`), `mixMasterGainS`, `mixLimiter` (`SafetyLimiter`). Reset fresh on the first layout change while nothing has sounded (`mixSounded`), as the engines' own did |
| Expanders | `ToExpanderMessage::engines[i]`: each expander labels / routes its CV inputs by its artifact's engine (`WaveArtifactExpander.cpp`) |

## 6. Play modes (`PLAY_MODE_PARAM`: MORPH, POLY, STEP, ALL, SEQ)

| Mode | Code in `process()` |
|---|---|
| MORPH | No notes: the engines crossfade the playhead's two neighbouring artifacts (`MorphEngine::renderMorph`, GrainEngine `heard` / `tw` equal power) |
| POLY | `voices.process(...)` with `MarkerTrigger`: the playhead (POSITION + POS LFO, `triggerPosition`) fires a marker when it reaches / jumps over it; each marker has an ADSR (`settings[i].adsr`, HOLD: `adsrHoldSeconds`, top = until another fires) |
| STEP / ALL | `stepper.process(posLfo.getPhase(), ...)`: each POS LFO cycle plays the next marker (STEP) or every marker (ALL) |
| SEQ | The sequencer's fires (`seqFired`, `seqLegato`, `seqHoldOpen`, velocity `seqVelocity`) drive `voices.process(... steps ...)`; locks applied with `seqLocks[i].apply` |
| Direct play | Expander GATE (`VoiceGates`), a press on a slot (`pressedMarker`) |
| To the engines | `in.voiceEnv[i]` (ADSR level x SEQ velocity), `in.voiceStart[i]`; the AD of the expander starts on each note (`modAdNote`) |

## 7. What each engine does with an artifact

- **WAVE** (`MorphEngine`): tables glide (`TableState`, SMOOTH, jump crossfade), `readAt` (mip level from pitch, cubic,
  table blend); MORPH: one shared drive + filter with settings interpolated between the two neighbours; POLY:
  `renderPoly` per-voice chain (drive, filter, ADSR, pan, headroom trim); SEND / RETURN / own chains: `renderChains`.
- **GRAIN** (`GrainEngine::schedule`): grain grid free / synced (`GrainClock`), DENSITY, SIZE, SPRAY, P.SPRD, SPREAD,
  windows (`grainWindowValue`, SMEAR diffuser), SCAN (loop / ping, length, fade), note restart, seeded scatter; grains
  render in `process()` into each cloud's bus; then each cloud's filter, gain, level, pan.
- **SAMPLE** (`GrainEngine::play` -> `PlayerVoice`): START, slice to LENGTH or the next marker (`sliceFrames`), ONE / LOOP
  / PING with loop point, BEAT (CUT / FIT, `sampleBeatDivision`), HIT snap (`nearestHit`), zero-crossing snap, note fade
  in (`SAMPLE_FADE_IN_PARAM`), DRIVE (`sampleDrive`, latency-aligned).
- **All:** filter per artifact (`Svf` via `FilterParams`), GAIN (`MAX_GAIN` 4), PAN, RETURN replaces the filter
  (`insertS` 20 ms); then the master section (section 11).

## 8. Modulation, clock, transport

| Thing | Code |
|---|---|
| Clock | `ClockTracker clock`: CLOCK input with PPQN (`CLOCK_PPQN_PARAM`) or the internal BPM; CLOCK OUT pulse per beat; `seqClock` (`GrainClock`) for the sequencer's beat position |
| RESET / RESTART / menu restart | `resetTrigger`, `RESTART_PARAM`, `restartRequest`: clock, every LFO, the stepper, the sequencer (`seqResetCount`) |
| STOP | `STOP_PARAM` -> `holdSilent` (5 ms fade; LFOs and clock keep running) |
| WAVE LFO (per artifact) | `lfos[i]` -> `m.lfo`, only on WAVE with AMT > 0 (`waveLfoLive` grace 1 s) |
| POS LFO | `posLfo` + `posLfoSmoother` (SMOOTH), centred sweep `lfoCentered` |
| Expander LFO + AD | `modLfos[i]`, `modAds[i]` (`MarkerMod.hpp`), routed by `ModSum` to a destination with depth; live only while heard (`modLfoLive`) |
| Expander CV | `ToMainMessage`: fast paths `mod[i][...]` (wave, cutoff, drive, fine, pan, gain VCA `gainMul`), slow ones as `knobMods` -> `cvAdd` / `cvMask` read in `refreshSettings` / `mv()` |
| Master CV | `masterCv(...)` per master knob, gain CV as a VCA |

## 9. The step sequencer (left of the module)

| Step | Code |
|---|---|
| Rows and steps | `WaveArtifactSequencer`: one row per artifact, 16 steps (`STEPS`), `velocity[r][s]` (0 = no trig), `length[r]`, row clock `CLOCK_MULT_PARAM + r` (-4..4: /8 /4 /3 /2 x1 x2 x3 x4 x8 steps per beat, `stepsPerBeat`) |
| Playing | `process()` -> `tickRow(r)` on each new step index (`beatPos` from the main module's `ToSequencerMessage`): trig conditions (`conditionPasses`), ties (long gates), lock conditions; sends `FromSequencerMessage` (`fireCount`, velocity, lock values / mask, slide, `stepBeats`, `legato`, `holdOpen`); GATE / MOD outputs per row |
| Editing steps | `StepGrid` widget: click / drag velocity, ties (`toggleTie`), shift (`shiftRow`), copy / paste (`SeqClipboard`, `pasteRow`), delete (`deleteStep`), select a step (`select(r, s)`) |
| Locks on a step | With a step selected, the main module's knobs edit its locks: `lockTarget`, `lockCaptureStep` (switches / wheel), `LinkedKnob` drags -> `setStepLock` / `clearStepLock`; tooltips `lockTooltip`, `stepValueFor`; map line `stepWaveShown` |
| Lane | `LockLane`: one setting of a row as bars (`showLock(k)`, `laneSetting`), or the CONDITION view; `showInLane(paramId)` from a knob press |
| REC | `recordStep()`: knob moves go into the step playing now |
| RND / CLR | `randomizeLocks`, `clearConditions` |
| In the main module | `process()`: `seqFired`, `seqLocks[i].apply(...)` (glides with slide), `refreshSettings` at once for that marker |

## 10. LAY: chopping a loop onto a row

Entry: the row's LAY button (`WaveArtifactSequencer.cpp pressHeadButton`), the marker / map menus ("Lay out the loop on
the row", `appendLoopLayoutMenu` / `appendLoopLayoutItems`), Shift + drag on the map to pick a part (`WavetableMap`,
`wa::laySelection`, snaps to a hit and to 1, 2, 4... beats).

| Step | Code |
|---|---|
| What is laid out | `loopLayoutInfo(marker)`: only GRAIN / SAMPLE artifacts with the sequencer on; the slice from START to LENGTH (or the next active marker on the same file); counts hits (`updateTransients`) |
| How many beats | `wa::loopTempo(onsetOf(slot), ...)`: the beat count whose grid is strongest in the onset curve, the octave nearest the module's tempo; else the file name's (`bpmFromName`, only a number next to "bpm"); else `guessLoopBeats` |
| Which row | `wa::layoutRow(beats, 16)`: the finest row clock whose steps cover the loop in whole steps (at most 16) |
| Which trigs | `wa::layoutTrigs(hits, from, frames, steps, grid, laySnapSteps(stepSeconds))`: a trig only on a hit within **12 ms / 0.2 step** of a step (`LAY_SNAP_SECONDS`, `LAY_SNAP_STEPS`), the nearest per step; a hit between steps plays inside the slice before it, at its own time (no micro-timing in the sequencer); step 1 always from the loop's start; each trig plays up to the next. "On every step": `grid` |
| Writing the row | `layoutLoopOnRow`: clears the row, sets length / clock, per trig START lock and LENGTH (`setSliceLocks`; GRAIN: HOLD too), velocity from the hit's peak (`hitPeakDb`, `layVelocity`), sets the artifact for clean playback (FREE length, ONE, no attack, SEQ; GRAIN: real-time scan, Tukey grains); `layoutLoopWithUndo` wraps it in one undo for both modules |
| Random slices | `randomizeSlices`: the row's lit steps play random slices from the same list (`layoutTrigs` with the same tolerance), each up to the next trig |

## 11. SEND / RETURN, master section, outputs

| Thing | Code |
|---|---|
| SEND per artifact | Engines' `getSend(i)` (what its filter gets, before its level) -> `SEND_OUTPUT + i` (2 ch, or L+R mono when a mono module answers: `monoSend`), and `chainSendL/R` to the SEND / RETURN module through the expander chain |
| RETURN per artifact | `RETURN_INPUT + i` or the SEND / RETURN module's RET L/R (`ToMainMessage::stereoReturn`, wins): replaces that artifact's filter (20 ms crossfade). It is **after** the artifact's GAIN and note level, so a shared effect bus returned into one artifact gets gated by that artifact's notes (use a mixer return for a bus) |
| SEND / RETURN module | `WaveArtifactSendReturn::process`: sits at the end of the expander chain, reads `ToExpanderMessage::sendL/R`, writes `ToMainMessage` returns |
| Master | `mixMaster` (filter type, cutoff, res, spread, feedback, drive + CV), master gain (+CV VCA), `mixLimiter` (10 V), mono option (`monoOutput`, 10 ms crossfade) -> `LEFT_OUTPUT` / `RIGHT_OUTPUT` |

## 12. What the screens show

`process()` stores display values every 64 samples into atomics (`shownIndex`, `shownHeard`, `shownGrainRead`,
`shownScanStart/End`, `shownBeats`, `shownPosition`, `shownPreview`, `shownCv`, `shownReturn`, grain flashes
`shownGrainPos/Seconds/Count`), each artifact from the engine that plays it. Widgets read only those and params:
`MarkerBar` (bar, bullets, playhead, slots with their tags CV / RET / WAV and W / G / S when engines differ, file
line), `WavetableMap` (tables or waveform, hits, scan loops, loop point, preview, TRIG line, LAY selection),
`AdsrScreen`, `PanelLabels` (texts per engine), `ArtifactSections`, `StepValueBars`.

## 13. Things that cost time before (check them first)

- A RETURN carrying another artifact's sound clicks with this artifact's notes (section 11).
- Dropping an engine's limiter / master path loses its look-ahead and DC-blocker tail: a click. Hence one shared master.
- A stage with latency (the 2x drive, ~31 samples) after a fade that starts cold lets the sound in with a step: prime it.
- Loads from the screen went to the main sample and changed every artifact that plays it (fixed: `screenLoadSlot`).
- LAY moved hits between steps onto steps: half a step late and a gap before them (fixed: `laySnapSteps`).
- `check.sh --build-only` can print "built" with the old DLL when the compile failed: check the DLL time.
- The public Rack needs the MINGW64 build (`release_public.sh`); a UCRT64 DLL crashes it.

## Change log

- 2026-11-14: each artifact has its own engine (section 5); one shared master section; WAVE priming; the screen loads
  into the selected artifact (section 3); LAY only cuts at hits close to a step (section 10). File created.
