# Wave Artifacts: user journey and code map

What happens in the code at each step a user takes with the module, so the logic of any feature can be found quickly.
The README is the user manual (what the controls do); this file is the map from those actions to the functions, the
thread they run on and the state they touch. Search a function name to find it (line numbers drift, names do not).

**Keep it current:** every change to the module updates the matching section here, and the change log at the end.

Last updated: 2026-11-15.

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
| Hits for HIT / LAY | **Per artifact**: `artHits[i]` (`ArtifactHits`), its own slider `MARKER_HIT_SENSITIVITY_PARAM + i` (one `MiniSlider` per artifact, `hitsSliders[i]`, the selected one shown; Shift + drag: every artifact, via `linkStep`). `uiStep()` -> `updateHits(i)` (on GRAIN / SAMPLE; 30 ms drag throttle; copies another artifact's list on the same file at the same setting) -> `wa::findTransients(sample of slotOf(i), its slider)`; handed to audio via `pending` -> `active` (`markerHits(i)`). Old patches: `dataFromJson` copies `SAMPLE_HIT_SENSITIVITY_PARAM` into all five (flag `artifactHits`) |
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
| The per-artifact setting list | `MARKER_SETTING_PARAMS[NUM_MARKER_SETTINGS]` (69): copy / paste, links, step locks (same order as `StepLocks.hpp LOCK_NAMES`; `NOT_LOCKABLE` marks switches and ENGINE) |

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
- **All:** filter per artifact (`Svf` via `FilterParams`), GAIN (`MAX_GAIN` 4), PAN, RETURN (INSERT) comes back after DRIVE and the filter
- **Filter 2 (all engines):** `wa::Filter2` in series after the filter (`MARKER_FILTER2_MODE_PARAM` 0 OFF / LP / BP / HP / NOTCH, `MARKER_CUTOFF2_PARAM`, `MARKER_RES2_PARAM`; `s.filter2Mode` -1 = OFF). MorphEngine: the morph voice (`markerF2`, blended between the two markers, OFF = dry), `renderPoly` / `renderChains` (`DirectChain::f2`); GrainEngine: each cloud's `f2` (stereo). Skipped while OFF (`needed()`, its work out of line: inlined it changed the engines' rounding); on / off fades from / to dry (~20 ms). Panel: the filter page buttons, "1" over "2" in a full-height column on the filter section's left (`FILTER_PAGE_PARAM`, `FilterPageSelector`, `layout::FILTER_PAGE_POS` / `FILTER_PAGE_SIZE` / `FILTER_PAGE_GAP`; CUT / RES and DRIVE / TYPE moved right for it; each button shows its filter's type under its number, OFF dim; the page shown filled in the artifact's color, the hovered one outlined; a chevron between them, filter 1 feeds filter 2, dim while filter 2 is OFF) shows filter 2's CUT / RES / TYPE at filter 1's spots (`editKnobPage`); their labels follow the page (`PanelLabels::addPaged`, `pageParam`: CUT 1 / CUT 2, RES 1 / RES 2, TYPE 1 / TYPE 2). Naming everywhere: CUT 1, CUT 2, RES 1, RES 2, TYPE 1, TYPE 2, DRIVE (one drive, before filter 1); lists offer them in that order without changing saved numbers (`MOD_DEST_ORDER` via `ModeButton::order`, `CV_DEST_ORDER`, the lane menu's group)
  (`insertS` 20 ms); then the master section (section 11).

## 8. Modulation, clock, transport

| Thing | Code |
|---|---|
| Clock | `ClockTracker clock`: CLOCK input with PPQN (`CLOCK_PPQN_PARAM`) or the internal BPM; CLOCK OUT pulse per beat; `seqClock` (`GrainClock`) for the sequencer's beat position |
| RESET / RESTART / menu restart | `resetTrigger`, `RESTART_PARAM`, `restartRequest`: clock, every LFO, the stepper, the sequencer (`seqResetCount`) |
| STOP | `STOP_PARAM` -> `holdSilent` (5 ms fade; LFOs and clock keep running) |
| WAVE LFO (per artifact) | `lfos[i]` -> `m.lfo`, only on WAVE with AMT > 0 (`waveLfoLive` grace 1 s) |
| POS LFO | `posLfo` + `posLfoSmoother` (SMOOTH), centred sweep `lfoCentered` |
| Expander LFO 1, LFO 2, AD 1, AD 2 | `modLfos[i]`, `modLfos2[i]`, `modAds[i]`, `modAds2[i]` (`MarkerMod.hpp`; AD 2 starts on the same notes, trigs and GATE as AD 1 and runs only while its DEPTH is not 0 or the sequencer is on), each routed by `ModSum` to its destination with its depth; live only while heard (`modLfoLive` / `modLfoLive2`: depth, its jack via `lfoJack` / `lfoJack2`, or the sequencer on). Settings are the marker's (`MARKER_MOD_LFO_*`, `MARKER_MOD_LFO2_*`: lockable rate, shape, DEST, DEPTH, PHASE). PHASE (`s.modLfoPhase` / `s.modLfo2Phase`) is the `phaseOffset` of `Lfo::process`: the cycle shifted 0..360 deg, gliding ~5 ms when changed, taken at once on a RESET. Synced rates step through **every** note value (`LFO_KIND_ALWAYS` in `LfoRateQuantity` / `LfoRateKnobT`; the old T / D params are unused): an older patch's synced LFO 1 knob moves to the same note value on the full list (`wa::lfoKnobAllKinds`, `dataFromJson`, flag `lfoAllKinds`). Expander panel: two columns, no titles (light, small shape screen + SYNC, DEST + DEPTH, RATE + AMT, jack `LFO_OUTPUT + k` + PHASE), bound to the marker in `WaveArtifactExpanderWidget::rebind` |
| Expander CV | `ToMainMessage`: fast paths `mod[i][...]` (wave, cutoff, drive, fine, pan, gain VCA `gainMul`), slow ones as `knobMods` -> `cvAdd` / `cvMask` read in `refreshSettings` / `mv()` |
| Master CV | `masterCv(...)` per master knob, gain CV as a VCA |

## 9. The step sequencer (left of the module)

| Step | Code |
|---|---|
| Pages | 64 steps per row (`STEPS`), shown 16 at a time, each row on its own page (`PAGE_STEPS`, `rowPage[r]`, `pageFirst(r)`; saved as "rowPages"). Each row's PAGE column (`PAGE_COL_X0`, between the shift arrows and the steps, titled PAGE; drawn in `StepRowLabels`, `pageButtonAt` / `onButton` in `StepGrid`): 4 stacked tabs 1..4 (`pageTabY`): shown = filled in the row's color, inside the length = dark, past it = outline, playing = white edge; click a tab = that page, Shift + click = every row, right-click = menu; it makes that row the lane row. A row on page 2..4 shows its first step's number in its first cell. `StepNumbers` under the grid: the lane row's page numbers with "PAGE n" (the lane shows that page). Grid / lane hit-tests return the step on the row's page (`seqlayout::columnAt` + `pageFirst(r)`), drawing maps step -> column; drags stop at the page's edges; a tie can be made across a page's left edge. LENGTH 1..64 (row menu: whole pages, then exact lengths by page; Ctrl+click on any page); default and old patches 16. LAY still lays out at most 16 steps (`PAGE_STEPS`) |
| Rows and steps | `WaveArtifactSequencer`: one row per artifact, up to 64 steps (`STEPS`), `velocity[r][s]` (0 = no trig), `length[r]`, row clock `CLOCK_MULT_PARAM + r` (-4..4: /8 /4 /3 /2 x1 x2 x3 x4 x8 steps per beat, `stepsPerBeat`) |
| Playing | `process()` -> `tickRow(r)` on each new step index (`beatPos` from the main module's `ToSequencerMessage`): trig conditions (`conditionPasses`), ties (long gates), lock conditions; sends `FromSequencerMessage` (`fireCount`, velocity, lock values / mask, slide, `stepBeats`, `legato`, `holdOpen`); GATE / MOD outputs per row |
| Editing steps | `StepGrid` widget: click / drag velocity, ties (`toggleTie`), shift (`shiftRow`), copy / paste (`SeqClipboard`, `pasteRow`), delete (`deleteStep`), select a step (`select(r, s)`) |
| Locks on a step | With a step selected, the main module's knobs edit its locks: `lockTarget`, `lockCaptureStep` (switches / wheel), `LinkedKnob` drags -> `setStepLock` / `clearStepLock`; tooltips `lockTooltip`, `stepValueFor`; map line `stepWaveShown`. Lock dots + value bars: `StepValueBars` (main panel: the selected marker; an expander: its own marker, `stepBars->marker` in `rebind`; the SEND / RETURN module: each row's SEND knob is its own artifact's, `controlMarkers`; off the main panel `stripVertical` = false, bars always under the control), counted by `stepsLockingInRow(paramId, marker)`: a dot in the middle of a knob (outlined), above a screen / button (DEST, filter type, shape, toggles); white when the selected step locks it |
| Lane | `LockLane`: one setting of a row as bars (`showLock(k)`, `laneSetting`), or the CONDITION view; `showInLane(paramId)` from a knob press. Setting menu `openSettingMenu` (groups of setting indexes, incl. SEND, LFO 2, AD 2, PHASE) |
| Phantom (trigless) locks | Locks on an unlit step (`isPhantom`, `hasStep`): drawn in the lane on an empty step (a drag begun there only makes phantoms, `dragPhantom`; one begun on a trig only edits trigs). Yellow (`PHANTOM_COLOR`): outline + lock dot on the grid, bars in the lane. `tickRow`: its locks (lock conditions applied) go on top of the row's last step (`mergeLocks`), `lockFires` -> `FromSequencerMessage::lockCount` -> main `seqLockSeen`: `seqLocks.apply` without a note, GATE or MOD. Lasts until the row's next trig. Selectable; knobs add locks to it (`lockTarget`); SLIDE strip works; a grid click makes it a trig keeping its locks. `tidyStep` clears it when its last lock goes. Grid drags delete only steps they touched (`touched`). Old patches (no `phantomLocks` key) drop locks on unlit steps |
| REC | `recordStep()`: knob moves go into the step playing now |
| RND / CLR | `randomizeLocks`, `clearConditions` |
| In the main module | `process()`: `seqFired`, `seqLocks[i].apply(...)` (glides with slide), `refreshSettings` at once for that marker |

## 10. LAY: chopping a loop onto a row

Entry: the row's LAY button (`WaveArtifactSequencer.cpp pressHeadButton`), the marker / map menus ("Lay out the loop on
the row", `appendLoopLayoutMenu` / `appendLoopLayoutItems`), Shift + drag on the map to pick a part (`WavetableMap`,
`wa::laySelection`, snaps to a hit or the sample's edge and to 1, 2, 4... beats, or all the beats up to the edge). The drag and LAY count beats in **one** tempo,
`layGridBpm(slot)` (the loop's own grid, else the audio's, else the name's): with the audio's alone a 120 bpm loop of
16ths read as 160 (4 / 3), so a "4 beats" drag was 3 beats to LAY and gave 12 steps.

| Step | Code |
|---|---|
| What is laid out | `loopLayoutInfo(marker)`: only GRAIN / SAMPLE artifacts with the sequencer on; the slice from START to LENGTH (or the next active marker on the same file); counts its hits |
| Which hits | `layHits(marker)`: exactly that artifact's own HITS slider's list (the map's ticks while it is selected; fewer .. more), all worked out first when the file or the slider changed (`updateHits(marker, true)`: no drag throttle, whatever its engine). The LAY menu shows the count and its slider. At 0 % a loud hit hides the ones near it (6 dB range, 250 ms gap): raise the slider for more slices |
| On the file's beats | First `wa::loopFileBpm(fileSeconds, bpmFromName, fileTempoOf, moduleTempo)`: the name's tempo (also a leading `90_` / `120 `), else the one found in the whole file, at an octave where the **whole file is whole beats** (within 1 %). If there is one, the part starts on the hit nearest START (within 1/8 beat) and lasts whole beats, and that is the row's tempo (a slice set by hand a few ms off the beat stretched the steps off the hits). Else: |
| How many beats | `wa::loopTempo(onsetOf(slot), ...)`: the beat count whose grid is strongest in the onset curve, the octave nearest the module's tempo; else the file name's (`bpmFromName`, only a number next to "bpm"); else `guessLoopBeats` |
| Which row | `wa::layoutRowForHits(hits, ...)`: of the row clocks whose steps cover the part in whole steps (at most 16), the one with the most hits right on a step (within the snap), then the most trigs; a tie goes to the finer (a triplet loop gets x3 / /3). "On every step": `wa::layoutRow` (the finest). The LAY menu shows the steps, clock and "N of the M hits on trigs" per length. 16 steps is the limit: a 3-beat part of 32nds (24 steps) keeps its 16ths only; lay out a shorter part (Shift + drag) for finer |
| Which trigs | `wa::layoutTrigs(layHits(marker), from, frames, steps, grid, laySnapSteps(stepSeconds))`: **every** hit gets a trig on its nearest step (the nearest per step). Its slice starts on the hit when it is within **12 ms / 0.2 step** (`LAY_SNAP_SECONDS`, `LAY_SNAP_STEPS`); further off (a live break's early 16th, a 16th on an 8th row) it starts on the step's own place in the audio, so the hit plays at its own time inside its slice or at the end of the one before (no micro-timing in the sequencer). Step 1 always from the loop's start; each trig plays up to the next. "On every step": `grid` |
| Writing the row | `layoutLoopOnRow`: clears the row, sets length / clock, per trig START lock and LENGTH (`setSliceLocks`; GRAIN: HOLD too), velocity from the hit's peak (`hitPeakDb`, `layVelocity`), sets the artifact for clean playback (FREE length, ONE, no attack, SEQ; GRAIN: real-time scan, Tukey grains); `layoutLoopWithUndo` wraps it in one undo for both modules |
| Random slices | `randomizeSlices`: the row's lit steps play random slices from the same list (`layoutTrigs` with the same tolerance), each up to the next trig |

## 11. SEND / RETURN, master section, outputs

| Thing | Code |
|---|---|
| SEND per artifact | **Only on the stereo SEND / RETURN module** (the main panel's SEND / RETURN jacks are gone; `SEND_OUTPUT` / `RETURN_INPUT` IDs reserved, old cables dropped as they have no port widget). Engines' `getSend(i)` (its sound after DRIVE and its filter: type, CUTOFF, RES; before its level) -> `chainSendL/R` -> `ToExpanderMessage::sendL/R` along the expander chain |
| RETURN per artifact | The SEND / RETURN module's RET L/R (`ToMainMessage::stereoReturn`). `MARKER_RETURN_MODE_PARAM + i` (`settings[i].returnBus`): **INSERT** (default): comes back in right after that artifact's DRIVE and filter, which stay in the flow (20 ms crossfade; the filter always runs, its output is the SEND), then its GAIN, note level and PAN, so a shared bus returned into one artifact is gated by its notes. **BUS**: `in.busSend[i]` makes the engines' send the finished sound (after filter, level, gain, pan: GrainEngine cloud loop, MorphEngine `renderPoly` / `renderChains`), x the SEND level `MARKER_SEND_LEVEL_PARAM + i` (lockable per trig, default 0, slewed 5 ms: `sendLevelS`); the return (`busRet[i]`) is added to the mix before the master, ungated, so a delay's tail rings out after the trig that fed it (Elektron style). Slot tag BUS (`shownBus`) |
| SEND / RETURN module | `WaveArtifactSendReturn::process`: sits at the end of the expander chain, reads `ToExpanderMessage::sendL/R`, writes `ToMainMessage` returns. Each row's second line: a SEND `LinkedKnob<Trimpot>` (a plain knob cannot lock: with smoothed params the per-frame revert of `lockCaptureStep` fights it; LinkedKnob writes the selected trig's lock directly) and an INS / BUS `ModeButton`, bound in `WaveArtifactSendReturnWidget::step()` to the main module's params (`mainOnLeft()` walks the expander chain), so trig locks work as on the main knobs (`lockCaptureStep` with a step selected) |
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

- 2026-11-18: a GRAIN note starts as a SAMPLE note does (`GrainEngine::schedule`, `Cloud::notePre` / `noteAttack`):
  its first grains fade in over the note fade-in (`SAMPLE_FADE_IN_PARAM`, 1 ms default, now for both engines; menu
  and tooltip say so), reading that far before START (forward), so the hit from START on plays untouched; the old
  note still fades out over 5 ms. Before, the new grains faded in over 5 ms from START over a sounding note (every
  trig's attack softened under the old note's fade) and not at all from silence. On the user's LAY row: per-slice
  match with the file 0.88-1.00 -> 0.98-1.00 (as SAMPLE), level wobble 3.2 -> 1.3 dB, the 10 ms after a trig 5.3 ->
  19 dB under SAMPLE's difference; dense clouds no longer dip 1-2 dB at a trig (`testRestartOverNote`,
  `build/probe_userlay.cpp`, `build/eng_transparent.cpp`: a held LAY cloud nulls as well as SAMPLE).

- 2026-11-18: a loop's tempo from a number in the middle of its name (`wa::bpmFromLoopName`): "ARSS_120_A_Bouzuki_String"
  is 120 when the file lasts whole beats of it within 5 ms (2+ beats, the number on its own, 60..200), else nothing, as
  before. Used by `layGridBpm`, `loopLayoutInfo` and `fileTempoOf` (the drag's beats, LAY's grid). A soft string loop
  had no tempo at all (the audio shows no clear beat): the drag could not snap and LAY guessed. Also found on that
  patch: LAY's grains need WINDOW TUKEY (set by LAY); on HANN at LAY's spacing (100 ms, one per 75 ms) every trig dips
  7-10 dB for 25 ms; and a row twice the slices' length leaves each slice silent half its step (both engines). Check:
  `build/probe_userlay.cpp` (the autosave patch, GRAIN vs SAMPLE per 5 ms).

- 2026-11-17: the map's Shift + drag can pick the whole sample again (`wa::laySelection`): the press snaps to the
  sample's start / end like a hit, a beat count up to 1 % short of the room still fits (the part ends at the edge), and
  dragged to the edge it can take all the beats up to it (12, 24 ...) when LAY has a row for them, not only 1, 2, 4 ...
  A 16-beat loop a hair short, or pressed a few pixels in, halved to 8 beats: half the sample, 2 pages. LAY's own
  whole-beats check (`loopLayoutInfo`) takes the same 1 % (`testLaySelection`).

- 2026-11-16: LAY: a loop within 8 % of the module's tempo (octaves too) goes on whole module beats, regular step counts,
  following the module by those few percent (`wa::layNearTempoBeats`: a 100 bpm loop at 103 is 64 steps x4 at 1/16, not
  33 steps); only a tempo clearly apart keeps its own speed with an exact count (`testLayNearTempo`).
- 2026-11-16: SLICE per artifact (`MARKER_HIT_MODE_PARAM`, a button right of the HITS slider on GRAIN / SAMPLE; settings
  list 74, not lockable): AUTO = the hits found (as before), or equal slices of 1/1 .. 1/32 at the file's tempo
  (`wa::sliceGrid`; no tempo: the file as one bar); the HITS slider hides then. Its hits feed the map's ticks, HIT, the
  arrows, LAY and RND. LAY lays one slice per step when a straight clock plays them at their own speed
  (`wa::layOneSlicePerStep`), else its own-tempo layout. LAY never picks a triplet clock (x3, /3:
  `wa::layClockAllowed`). A tempo in the file name counts the beats at its own value when the file is whole beats of
  it (`loopFileBpm`, and `loopLayoutInfo` when the audio agrees): a 174 loop is 32 beats at 174, not 16 at 87.
  Checks: `testLayRowForHits`, `build/probe_slice.cpp`.
- 2026-11-16: LAY keeps the loop's own tempo (`layLayout` with beats 0, `wa::layoutRowAtOwnTempo`): the row's cycle lasts
  as long as the part at the module's tempo (`layModuleBeats`), any step count up to 64 within 2 % of its speed (a 174 bpm
  loop at 120: 44 steps x2, speed 1.003; it played 1.38x laid on its beats). A part lasting whole module beats is laid
  out exactly as before. The menu's "on a number of the module's beats" lengths still fit to those beats; RND slices
  use LAY's layout (`testLayOwnTempo`, `build/probe_laytempo.cpp`).
- 2026-11-16: LAY lays a long part over pages: `wa::layMaxSteps(beats)` = one page up to 4 beats (as before), then 16th
  notes onto pages 2-4 (8 beats: 32 steps, 16 beats: 64), coarser only past 16 beats; the row shows page 1 after LAY
  (`testLayPages`). GRAIN has DRIVE (`GrainEngine::grainDrive`, before the filter, skipped at 0, fades in over the
  drive's latency; the expander's DEST DRIVE stays P.SPRD on GRAIN, its CV DRIVE reaches it); old patches set a GRAIN
  artifact's leftover DRIVE to 0 (no "grainDrive" key). Filter ROUTE per artifact (`MARKER_FILTER_ROUTE_PARAM`, lockable,
  settings list 73): SER (filter 1 then 2) or PAR (both on the same input, mixed; `Filter2::apply`; a change fades
  filter 2 out and in); on the filter 2 page at the DRIVE spot (`testFilterRoute`, `build/probe_graindrive.cpp`).
- 2026-11-16: main panel: the selected artifact's color as a soft wash (15 % light / 12 % dark, `ArtifactTint`, under the
  labels and controls) on everything that follows the selection: WAVE / START, GAIN / PAN, FILTER, PITCH, the envelope
  strip (POLY), KEEP (GRAIN) and a halo behind the ENGINE button; global sections stay plain. The filter page
  buttons: two tall buttons "1" / "2" in a column filling the filter section's height on its left, each with its
  filter's type (LP / BP / HP / NOTCH, OFF dim) and a chevron between them (series); CUT / RES and DRIVE / TYPE moved
  right by ~3 mm for it. The SAMPLE / GRAIN HITS slider reads "HITS 46%" (`MiniSlider::prefix`). UI only, no audio change.
- 2026-11-16: sequencer panel laid out again: one frame (2 mm margin, nothing outside it), one toolbar line (REC, COND
  CLR ending at the grid; LOCKS RND and the amount ending at its end; a round "!" with all the help in its tooltip,
  `HelpBadge`, instead of three lines of text), one line of column titles (LEN, CLOCK, PAGE, GATE, MOD), two-line row
  headers (chip + LEN over LAY + RND), rows 13.4 mm (were 12.6), the lane lower with its name left-aligned and CLR on
  its right. All in `seqlayout` (section 9).
- 2026-11-16: 64 steps per sequencer row in 4 pages of 16, each row with its own PAGE button (click: next page,
  Shift: every row; the grid narrowed by 4.8 mm for it), LENGTH up to 64; old patches and new rows stay 16 steps;
  LAY still 16. Check `build/probe_64steps.cpp` (section 9).
- 2026-11-15: filter naming and order made one: CUT 1 / CUT 2, RES 1 / RES 2, TYPE 1 / TYPE 2, DRIVE on the panel
  labels (following the page), the expander's DEST buttons and CV menus, the lane's lock labels and menu, and the
  tooltips; the lists offer them side by side (display order only, saved values unchanged). The F2 toggle became a
  1 / 2 page selector left of CUT / RES (section 7).
- 2026-11-15: filter 2 per artifact, in series after the filter (OFF / LP / BP / HP / NOTCH, CUTOFF 2, RES 2; lockable;
  settings list 72): OFF by default and skipped (bit-identical, no cost); on: +7..40 ns settled, +44..96 ns moving
  (GRAIN / SAMPLE, user patch, 5 artifacts). F2 toggle on the panel swaps CUTOFF / RES / TYPE to filter 2. Checks:
  `tests/test_engine.cpp` (`testFilter2`), `build/probe_filter2.cpp` (3 engines x MORPH / POLY), golden IDENTICAL (section 7).
  The expander reaches it too: LFO / AD DEST `MOD_DEST_CUTOFF2`, `MOD_DEST_RES2` ("CUT 2", "RES 2", appended to
  `ModDest`) and CV destinations `CV_CUTOFF2`, `CV_RES2` (appended to `CvDest`, through the settings like RES); checks
  `build/probe_f2dest.cpp`, `build/probe_f2cv.cpp`.
- 2026-11-15: lock dots: in the middle of each knob (larger, outlined) instead of above it, where it sat on the label;
  screens and buttons keep theirs above. Fix: an expander's dots counted the main panel's selected marker's row, so
  they showed only when that marker was the expander's own (`stepsLockingInRow(paramId, marker)`; section 9). The
  SEND / RETURN module's SEND knobs get them too (they had none).
- 2026-11-15: phantom (trigless) locks: draw a lock in the lane on a step with no trig; it changes the playing note's
  settings from that step (on top of the trig's locks) without a note; yellow on the grid and lane. The lane menu now
  offers SEND, LFO 2, AD 2 and both PHASEs. A grid drag no longer cleans up untouched empty steps. Checks:
  `tests/test_seq.cpp` (`testMergeLocks`), `build/probe_phantom.cpp` (section 9).
- 2026-11-15: SEND / RETURN (INSERT) keeps the artifact's DRIVE and filter (type, CUTOFF, RES) in the flow: SEND is
  taken after the filter (it was before it) and the RETURN comes back there, then GAIN, note level and PAN (it used to
  replace the filter). `MorphEngine` `renderPoly` / `renderChains`, `GrainEngine` cloud loop: the filter always runs.
  BUS unchanged. Checks: `tests/test_poly.cpp` (`checkReturn`), `build/probe_sendret2.cpp` (real modules, 3 engines x
  5 filter settings); `build/probe_sendret.cpp` is out of date (it compares with the removed main-panel jacks) (section 11).
- 2026-11-15: a PHASE per expander LFO (`MARKER_MOD_LFO_PHASE_PARAM`, `MARKER_MOD_LFO2_PHASE_PARAM`, 0..360 deg,
  lockable): `wa::Lfo::process(..., phaseOffset)` shifts the periodic shapes (a RESET starts there; synced it stays
  that far after the beat; a change glides ~5 ms; random shapes ignore it; 0 = bit-identical). Expander: the
  "LFO 1 / LFO 2" titles are gone, the rows moved up 2 mm, the bottom row is OUT (under RATE) + PHASE (under AMT).
  Settings list 69 (section 8).
- 2026-11-15: a second expander AD per marker (attack, decay, PERC, DEST, DEPTH; a second row on the panel, the CV
  section a little shorter). Settings list 67, past 64: the lock masks are `wa::LockMask` (128 bits, `StepLocks.hpp`)
  instead of `uint64_t`; `NOT_LOCKABLE` is a list (section 8, 9).

- 2026-11-15: the main panel is 36 HP (182.88 mm, was 42): the empty SEND / RETURN column is gone (`PANEL_W`, both
  SVGs). In an existing patch the expanders to its right sit 6 HP away until moved back against it.
- 2026-11-15: the main panel's SEND / RETURN jacks and their box are removed (no replacement; the column is empty):
  sends and returns only through the stereo SEND / RETURN module (section 11).
- 2026-11-15: a second expander LFO per marker (rate, shape, sync, DEST, DEPTH, its own jack and level); both LFOs
  step through every note value (no T / D; old synced rates keep their note value); smaller shape screens. Fresh
  modules' LFO 1 starts at 1/4 (knob 0.565). Settings list 62 (section 8).
- 2026-11-15: RETURN mode per artifact (INSERT / BUS) and a SEND level lockable per trig, on the SEND / RETURN module's
  rows: a delay only on the trigs that send to it, its tail ringing out (section 11). Settings list 57.
- 2026-11-15: every found hit gets a trig again; one off its step by more than 12 ms starts its slice on the step's
  place (a break's 16th 21 ms early was dropped: 1 trig for 2 hits). The row clock scores hits right on steps first
  (section 10).
- 2026-11-15: the map's Shift + drag counts beats in LAY's tempo (`layGridBpm`): a 4-beat drag now lays 4 beats
  (16 steps), not 3 (12) (section 10).
- 2026-11-15: LAY picks the row clock that puts the most hits on trigs (`layoutRowForHits`), not the finest; the
  LAY menu shows how many hits each length places (section 10).
- 2026-11-15: each artifact has its own HITS slider and hit list (`ArtifactHits`, `updateHits`; sections 3, 10):
  LAY, HIT, the map's ticks and the hit arrows use the artifact's own. Settings list 55 (HITS not lockable).
- 2026-11-15: LAY uses exactly the HITS slider's hits again (the 50 % floor removed, at the user's request), always
  up to date when it lays out; the LAY menu shows the slider (section 10).
- 2026-11-15: LAY puts the part on the loop file's own beat grid (`loopFileBpm`, name prefix tempos): a 90 bpm
  break's 1.38 s slice (7 ms late, 3.5 % long) got 1 trig, now 7 on its 16ths (section 10).
- 2026-11-15: LAY finds its own hits at the HITS slider but at least 50 % (`layHits`): at 0 % a drum break's slice
  got 3 trigs of its 8 (section 10).
- 2026-11-14: each artifact has its own engine (section 5); one shared master section; WAVE priming; the screen loads
  into the selected artifact (section 3); LAY only cuts at hits close to a step (section 10). File created.
