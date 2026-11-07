# Wave Artifacts

A VCV Rack 2 wavetable morph oscillator. It turns one imported WAV file (max 10 MB) into
50-100 single-cycle wavetables ("artifacts") and morphs between up to 5 user-placed markers.
A wavetable map along the bottom shows the whole sample, with each marker's bullet at the
artifact it plays. The **Wave Artifacts Expander** (up to 5, to the right) adds per-marker CV
inputs, a gate with an attack / decay envelope, and two LFOs.

![Wave Artifacts](docs/images/waveArtifact.png)

*Wave Artifacts as it looks before a WAV is loaded. Top row: the selected marker's controls,
then the tempo section. Middle: the marker bar (the five slots, the bullets and the playhead).
Bottom: POSITION and its LFO, FREQ / V/OCT, the master section and the wavetable map.
Right: the five ARTIFACT OUT pairs.*

Each artifact also has its own stereo output (ARTIFACT OUT, right of the panel): its own chain
before the master section, at its share of the position crossfade (full level while the
playhead is on its marker, fading out toward the neighbors, silent elsewhere). Patching an
artifact's output takes that artifact out of the main L/R (a 20 ms fade, no click), so it
becomes a separate audio stream; unpatch it to bring it back. The screen tags it "OUT" while it
is routed out.

The plugin and module slugs stay `waveArtifact` and `waveArtifactExpander`, so patches saved
with earlier builds still load.

Status: playable. Oscillator, markers, per-artifact controls, master filter, internal tempo and
clock, artifact outputs and the expander (CV, gate envelope, LFOs) are done. Saving the loaded
WAV inside the patch is still to come.

## Quick start: your first patch

You need Wave Artifacts, something that sends pitch (and optionally a gate), and an audio
output. The smallest patch is just Wave Artifacts into **Audio**, which already makes sound.

1. **Add the module.** Right-click the rack, search for *Wave Artifacts* and add it.
2. **Load a WAV.** Click the screen where it says *Load a WAV (max 10 MB)* (or the file line
   at the bottom of the screen later on), or right-click the module and choose **Load WAV...**
   Any WAV works: a voice, a pad, a field recording. The module analyses it for a moment and
   the file line then shows the name and the number of artifacts, e.g. `pad.wav  64 artifacts`.
   The **◀ ▶** arrows at the right end of the file line load the previous or next WAV in the
   same folder, sorted by name and wrapping around (also in the right-click menu).
3. **Patch the output.** Cable **L** and **R** (bottom right of the main section, in the dark
   box next to STOP) to an **Audio** module's left and right inputs.
4. **Hear it.** The module starts with one marker, so you hear one wavetable at a steady pitch
   (C4 with the FREQ knob in the middle).
5. **Add markers.** Click an empty spot on the marker bar (the line with the bullets) to add a
   marker there, up to five. Each new marker picks the artifact under that spot of the sample.
   Or right-click the module and choose **⚠ Auto arrange artifacts → Auto arrange now** to
   place all five evenly. It resets the artifacts you have set up, so it asks first.
6. **Move through them.** Turn **POSITION**, or drag the white playhead on the bar. The sound
   crossfades between the two markers the playhead is between. Drag a bullet to move a marker.
7. **Shape each marker.** Click a slot at the top of the screen (or a bullet) to select a
   marker. The panel's top row now edits that marker: WAVE (which artifact it plays) and its
   SMOOTH, the WAVE LFO, GAIN and PAN, CUTOFF / DRIVE / RES / filter TYPE, and PITCH / FINE.
   You can also click the wavetable map along the bottom to give the selected marker that
   artifact; double-click an artifact to audition it, right-click it to assign it to a marker.
   To start a marker from another one, right-click the first (its slot or bullet), choose **Copy
   settings**, then right-click the other and choose **Paste settings of marker N**. Every setting
   of both engines is overwritten, including WAVE / START; the marker keeps its on/off state and its
   place on the bar. Ctrl+Z undoes a paste. CV patched on an expander stays with its marker.
   **Double-click a marker's bullet on the bar to mute it** (again to unmute; also in its right-click menu). A muted
   marker turns grey, keeps its number, place and settings, and makes no sound on either engine, its own output
   included. It fades out and in over 5 ms, so muting while playing does not click. Ctrl+Z undoes it.
   **Shift + a control moves that setting on every marker at once.** Hold Shift and drag any knob in the
   per-marker section (START, GAIN, CUTOFF, the envelope and so on). Every active marker moves by the same
   amount from where it is, so the differences between them stay; if one hits its limit, moving back restores
   the spacing. When the knob you are holding reaches its own limit, keep dragging: it stays there and the
   others carry on until each reaches its limit (not in Rack's rotary knob modes). Shift-clicking a choice (filter TYPE, SYNC, the LFO or WINDOW screen) sets them all to the same
   value. **Shift + double-click a knob resets that setting to its default on every active marker.** Knobs keep their normal speed, Ctrl + Shift is the fine version, and Ctrl+Z undoes the whole move.
8. **Play it.** Cable a keyboard or sequencer's **1V/oct** to **V/OCT** (0 V is C4). Turn the
   POSITION LFO's AMT up to have the playhead move on its own; with SYNC on it follows the
   internal tempo (BPM) or a clock patched to **CLOCK**.
9. **Finish the sound** with the master section: pick a filter type (OFF / LP / BP / HP /
   NOTCH), then CUTOFF, RES, DRIVE, SPREAD and FDBK, each with its own CV trimpot and jack,
   and the master GAIN with its own **AMT** trimpot and **CV** jack (a VCA: GAIN at 0%, AMT at
   100%, then 0-10 V = 0-200%; negative AMT ducks). The red **!** bullet (STOP) fades every output to silence
   until you press it again.

Useful to know:

- **POS CV / CV AMT** move the playhead from a CV (0-10 V sweeps the whole bar).
- **NOTCH** (artifact and master filter) cuts a wide hole at CUTOFF. RES adds two resonant
  peaks on its sides (up to about +7.6 dB at full RES) while the hole stays fully deep. Sweep
  CUTOFF slowly (or modulate it from the expander) to hear it move through the harmonics.
- **RESET** (or a gate on the RESET jack) restarts the beat grid and every LFO; **CLK OUT**
  sends a 10 V trigger per beat.
- Right-click menu: *Artifact count* (50 / 64 / 100 or Auto), *Artifact tone* (Natural /
  Smooth / Soft), *Clock resolution* for the CLOCK input, *Restart LFOs* and *Add expander*.
- **Randomize** (right-click menu) leaves the STOP button alone and also randomizes the
  expanders snapped on to the right.
- To use the artifacts as separate voices, patch **ARTIFACT OUT 1-5**: each artifact then
  leaves the main L/R and plays on its own jacks, following the position.

## Play modes: MORPH and POLY

The **MORPH | POLY** button under the wavetable map chooses what the POSITION playhead does, on both engines.

**MORPH** (the default) is one voice: the playhead crossfades between the two neighboring markers.

**POLY** makes each marker its own voice, so up to five sound together:

- The playhead **starts a marker's note when it reaches it**, from either direction. A jump (a saw LFO's
  reset, a square, a fast knob move) starts only the last marker it passes. POS CV and the POS LFO count too;
  the POS LFO's SMOOTH does not affect the notes.
- Each note **plays for its own length (H, HOLD), whatever the playhead does next**, so notes sound together.
  The same marker reached again restarts its note. The marker nearest the playhead plays once when you enter
  POLY. With H fully up, a note stays on until the playhead starts another marker, so a stopped playhead drones.
- **A, D, S, H, R** set the selected marker's envelope: attack 1 ms to 5 s, decay and release 5 ms to 10 s,
  sustain 0-100%, hold 10 ms to 10 s. The little screen shows the shape. For a pluck, turn S down and D short.
  They only appear in POLY. To use one envelope everywhere, right-click a marker: **Copy ADSR to all markers**.
- A level bar on each marker's slot shows which voices are sounding.
- **Play a marker directly, without POSITION.** Patch a gate to the **GATE** of an expander set to that marker:
  the note starts when the gate rises, holds while it is high (H does not apply) and releases when it falls. That
  marker then ignores the playhead; the others keep following it. For a keyboard or pads, Rack's MIDI-Gate module
  gives each note its own gate output. In POLY the expander's ATTACK and DECAY only shape its ENV output.
- **Or hold a marker's slot down** on the screen: it plays until you let go, and selects the marker as usual.
- Every marker keeps its own WAVE, filter, drive, pan and pitch, with nothing blended between markers.
- Markers on the same spot of the bar start together, as a chord. V/OCT still sets one note for the module.
- An artifact output carries that marker's whole note. As in MORPH, patching it takes the voice out of L/R.
- On GRAIN, a note starts at once (even with SYNC) with its grains already overlapping, so a short attack is
  heard however long the grains are. Free SCAN restarts from START with each note.
- One or two voices play at full level; beyond that they share the headroom.

Copy / paste carries the ADSR too.

**STEP** (press the PLAY button a second time) plays the markers in turn from the POS LFO's clock instead of the
playhead: every cycle of the POS LFO is one step, and step 1 plays the leftmost marker on the bar, step 2 the next
one, and so on, wrapping round. With the POS LFO's SYNC on, RATE sets the step length: at 1/2, marker 1 plays on
the first half note, marker 2 on the second. POSITION and the POS LFO's AMT do not matter here; the playhead jumps
to show the marker playing. Markers that are off or muted are skipped, and RESET starts again from the leftmost.
Each note keeps its marker's own ADSR and sound. H sets its length; **H fully up keeps it on until that marker's next
turn**, so every marker keeps sounding and each is struck again on its own step.

**ALL** (the fourth press of PLAY) is the same clock, but every cycle plays every marker together: a chord on each
POS LFO cycle. With H fully up, every marker sounds all the time, struck again on each cycle, whatever the position.
Off and muted markers stay silent. Expander GATEs and slot presses still
play their marker directly.

## The step sequencer (SEQ)

For a drum-machine style pattern, right-click Wave Artifacts and choose **Add step sequencer (SEQ mode)** (or add
*Wave Artifacts Sequencer* from the module browser and snap it to the **left**; the expanders go on the right).
Then press PLAY until it shows **SEQ**. SEQ plays nothing until a sequencer is snapped on.

- **The grid:** one row per marker (its color), 16 steps. **Draw steps by dragging**: press on a step and drag along
  the row, and every step you pass is switched on at the height of the mouse (the top of a cell is 100%). Drag up and
  down to set a velocity. Click a step that is on, without moving, to switch it off, or hold Shift and drag along a
  row to erase the steps you pass.
- **Clock:** the module's own clock: the **CLOCK input** when something is patched, otherwise the **internal tempo**
  (BPM). The POS LFO is not involved. RESET goes back to step 1 on every row and plays it at once. A white frame
  shows each row's current step.
- **Clock per row:** the **CLOCK** knob left of each row multiplies (to the right) or divides (to the left) the
  module's clock for that row: /8 /4 /3 /2 | x1 | x2 x3 x4 x8, where x1 is a step per beat (quarter note), the
  default x4 gives 16th notes, and /2 a step every two beats. Rows at different speeds drift against each other.
- **Length per row:** Ctrl + click a step to end that row there (1 to 16), or use the right-click menu; rows of
  different lengths drift against each other (polyrhythms). Steps past the end are shown dimmed.
- **Velocity** scales the note's level (linearly: 50% is half the level) and opens the marker's filter, so a soft
  step is quieter and a little darker (at 0% it would be 2.5 octaves lower on the CUTOFF scale).
- **Sound:** each marker keeps its own ADSR, pitch, filter and everything else; H sets the note length. Muted and off
  markers stay silent. Right-click a row (or the module) for Every 4th / Every 2nd step, Random, Clear and the
  default pattern. The pattern is saved with the patch.

## The granular engine (GRAIN)

The toggle in the header switches the module from the wavetable engine (**WAVE**) to a granular
engine (**GRAIN**) that plays the same loaded sample. The panel stays the same: the markers, the
POSITION playhead and its LFO, the master section and the artifact outputs all work as before. Each
marker is now a **grain cloud** that reads the sample from its own START, and the playhead
crossfades the two neighboring clouds exactly like the wavetable morph. Several knobs change title
and meaning (the labels, tooltips and values follow):

| Knob (WAVE) | In GRAIN | What it does |
|---|---|---|
| WAVE | **START** | where in the sample the cloud reads (same value, so the wavetable map and bullets still work) |
| SMOOTH | **DENSITY** | grains per second (0.5-500), or the note value of the grain grid with SYNC |
| LFO shape | **WINDOW** | grain window: Hann, Tukey, Gauss, Triangle, Percussive, Swell, Box, Smear (click the little screen) |
| RATE | **SIZE** | grain length (2 ms-1 s), or a multiple of the grain interval with SYNC |
| AMT | **SPRAY** | random offset of each grain's start (up to 2 s) |
| SYNC / T / D | **SYNC / T / D** | lock the grains (and SCAN) to the tempo; triplet and dotted note values |
| DRIVE | **P.SPRD** | random pitch per grain (up to 12 semitones) |
| FINE | (empty) | SCAN now sits with the grain controls |
| (grain group) | **SCAN** | moves the read position through the marker's slice (see below) |
| (grain group) | **FADE** | dips the level at both ends of the slice (0-50% of it each side), so the loop never clicks |
| (grain group) | **LOOP / PING** | at the end of the slice: jump back to START, or turn round and run back |
| FINE | **LENGTH** | the scan loop's length: 10 ms to the end of the sample, or (fully up, the default) to the next marker |
| PAN | **PAN** | where the whole cloud sits, after its filter (on a stereo sample it works as a balance) |
| (bottom strip) | **SPREAD** | random pan per grain, around PAN |
| (bottom strip) | **KEEP** | keep speed: pitch (PITCH, FREQ, V/OCT) no longer slows down or speeds up the sample |

GAIN, the filter (CUTOFF / RES / TYPE) and PITCH / FREE work as before, per cloud. The expander's six
CV inputs follow the new titles (WAVE = START, DRIVE = P.SPRD, FINE = DENSITY; PAN stays PAN). SCAN and SPREAD are knob-only:
to move through the sample by CV, use START. At a hard-panned PAN one side is off, so SPREAD can only vary the level
there; it scatters left and right best with PAN away from the ends.

**Clock-locked grains.** With SYNC on, grains start on a grid of note values locked to the internal
tempo or CLOCK, with sub-sample timing; RESET is beat 0.

In GRAIN the WAVE / LFO section becomes one grain group: START, DENSITY, SIZE, SCAN and FADE on top; the WINDOW
screen, SYNC / T / D (under DENSITY), SPRAY and LOOP / PING below. WAVE mode keeps its own layout.

**Seeing and setting the loop.** On the wavetable map along the bottom, each scanning marker's loop is shaded in its
color from START to its end, with a small flag at the end: drag the flag to make the loop shorter or longer (it can
reach past the next marker, up to the end of the sample), or right-click it for *Up to the next marker*. LENGTH
(in FINE's spot) sets the same thing, and its tooltip shows the length in seconds.

**Scan and loop.** A marker's *slice* is the part of the sample from its START up to the next
marker's START (in sample order), or to the end of the sample. SCAN at the middle keeps the cloud
at START. Free: it moves through the slice at -2x to +2x real time (looping at the slice end). With
SYNC: the slice is fitted to 1/4 to 32 beats, locked to the beat grid, so the sample plays as a
tempo-locked loop without changing pitch. A negative SCAN runs it backwards.

**Keep speed.** Normally a lower pitch also plays the sample slower, like tape: with long grains, hits and words
inside them stretch. With **KEEP** on (per marker), pitch changes only the pitch and the sample keeps its timing.
The grains are centered on where the sample would be and made short enough to stay within about 20 ms of it, and
more of them fire (whole subdivisions of the grid with SYNC), so the overlap and level stay as set. The cost: at big
pitch changes SIZE acts as a maximum (40 ms at an octave up, 80 ms at an octave down, never under 30 ms), so a long
smeared grain gets shorter and grainier. At the original pitch nothing changes.

**Smear.** The Smear window turns a cloud into one continuous sound, with no gaps or jumps between
grains. Three things change: a grain lasts at least 3 grain intervals (so SIZE becomes a minimum and
DENSITY sets the smallest length: at 2 grains/s that is 1.5 s, at 8 grains/s it is 375 ms); each
grain starts where it best matches the grain it fades in over (up to 20 ms from where it would
start, so tones add up instead of cancelling); and the cloud runs through a short diffuser that
spreads sharp attacks. Grains with P.SPRD or a random direction cannot always be matched, so they
shimmer a little more.

**Repeating randomness.** While synced, the random SPRAY / P.SPRD / SPREAD / direction values of a
grain depend only on its place in the loop cycle, so the pattern repeats every cycle instead of
drifting (right-click menu: *Grain scatter repeats every loop*).

Pitch: 0 V plays the sample at its original speed; V/OCT, PITCH and the expander's V/OCT transpose
the grains (up to 3 octaves up without aliasing; the sample is kept low-passed at half rates).
Level: a dense cloud has about the loudness of the sample whatever DENSITY and SIZE are; sparse
grains (little overlap) stay lighter, like pulses. Grains that all start at the same place (SCAN
off, no SPRAY) repeat one short loop, so tonal samples are pulled toward multiples of the grain
rate: add SPRAY or SCAN for a steady pitch.

Right-click menu: *Engine*, *Grain direction (selected marker)* (forward / reverse / random) and
*Grain scatter repeats every loop*. The wavetable audition (double-click the map) is for the WAVE
engine only. Not yet: grain clock from the played pitch (PITCH trigger mode).

## The expander

![Wave Artifacts Expander](docs/images/waveArtifactExpander.png)

The **Wave Artifacts Expander** adds CV control to **one marker**. Choose which marker with the
**MARKER** button at the top (1-5).

### Adding one

Right-click Wave Artifacts and choose **Add expander** (it shows how many you have, up to 5),
or add *Wave Artifacts Expander* from the module browser and snap it against the right side of
Wave Artifacts (or of another expander). The LINK light at the top turns on when it is
connected. Added from the menu, an expander takes the first marker no other expander uses and
is placed in marker order: with an expander on marker 5, a new one gets marker 1 and sits
first, next to Wave Artifacts. Everything to its right moves over by one panel.

### What it does

From the top of the panel down:

- **V/OCT** adds pitch to this marker only (1 V/oct), on top of the main V/OCT.
- **GATE** turns the marker into a note: while the jack is patched, the marker sounds only
  while the gate is high (1 V or more) and is silent otherwise. Nothing is gated while it is
  unpatched. In POLY it plays the marker's own ADSR instead (see *Play modes*).
- **ATTACK / DECAY** shape the gate in MORPH, and the ENV output in both modes (5 ms to 2 s and 5 ms to 4 s). At the minimum the gate
  opens and closes instantly (still click-free). The level holds while the gate is high and
  DECAY starts when it falls. **PERC** makes it a true AD: attack then decay even if the
  gate is held.
- **ENV** outputs the envelope (0-10 V) so you can patch it to cutoff or anything else. It
  reads 0 V without a gate.
- **CV inputs with an AMT trimpot** for WAVE, GAIN, CUTOFF, DRIVE, FINE and PAN. AMT starts at
  0, so nothing moves until you turn it up. At 100%, 10 V sweeps the whole sample for WAVE,
  the whole cutoff range (about 1 V/oct) for CUTOFF, the whole range for DRIVE, 2 semitones
  for FINE and hard left to hard right for PAN. **GAIN works as a VCA**: at AMT 0 it does
  nothing, at 100% the marker's level follows the CV (silent at 0 V, full at 10 V).
  On the GRAIN engine the inputs become START, GAIN, CUTOFF, P.SPRD, DENSITY and PAN, and the
  START and DENSITY trimpots go 0-100% (no inverting). They keep their own values, separate from
  WAVE and FINE. At 100%, 10 V adds the whole sample to START and the whole knob to DENSITY. PAN
  works as on WAVE, with the same trimpot: 10 V at 100% goes hard left to hard right.
- **Two LFOs** (shape, RATE, AMT, SYNC / T / D) with their own outputs at +-5 V. Patch them
  anywhere, including the expander's own CV inputs. Synced LFOs follow Wave Artifacts' tempo
  or CLOCK, and restart on RESET.

Several expanders can control the same marker: their CVs add up, and their gates and GAIN
VCAs multiply (the marker sounds only while all of their gates are high).

### A first expander patch

1. Add an expander (right-click Wave Artifacts, **Add expander**) and make sure its MARKER
   shows the marker you want to move.
2. Patch **LFO 1** to the **WAVE** input and turn the WAVE **AMT** up to about 30%. The marker
   now sweeps through the sample's artifacts by itself.
3. To play that marker like a voice, send a keyboard's **gate** to **GATE** and its **1V/oct**
   to the main **V/OCT**. Set DECAY to a second or so and the marker rings out after each key.
   Turn PERC on for a pluck even if you hold the key. (In the main L/R you hear a marker
   while the POSITION playhead is on it, so park the playhead on that marker, or patch the
   marker's **ARTIFACT OUT** jacks, which follow the position the same way.)
4. For a second voice, add another expander: it gets the next free marker, ready for its own
   gate and CV.

## Build (Windows, MSYS2 MinGW 64-bit shell)

```bash
cd /e/Project/vcv-rack-plugins/waveArtifact
make            # RACK_DIR defaults to ../Rack-SDK-2.6.6-win-x64/Rack-SDK
make install    # copies the plugin into the Rack user folder
```

If the module does not appear in Rack, check `log.txt` in the Rack user folder.

## Offline tests (no Rack needed)

```bash
cd tests && make run
```

## Regenerating the images

The pictures in `docs/images/` are Rack's own module screenshots. With a Rack source build:

```bash
cd Rack && ./Rack.exe --dev --screenshot 3   # writes screenshots/waveArtifact/*.png, 3x zoom
```

Existing files are skipped, so delete the old PNGs first, then copy the new ones into
`docs/images/`.

## How the wavetables are made

`src/WavetableBank.cpp` splits the sample into N equal segments (Auto: 20 per second, clamped
to 50-100, or a fixed 50 / 64 / 100 from the context menu). For each segment it:

1. Detects the fundamental (McLeod normalized square difference function, 40 Hz-2 kHz).
2. Pitched segments: resamples one period to 2048 samples and averages up to 4 periods.
   Unpitched segments (noise): takes 2048 raw samples and crossfades the loop point.
3. Removes DC, drops harmonics the source could not contain, and rotates the phase so the
   fundamental starts at zero. Adjacent tables then morph without cancelling.
4. Builds 11 band-limited mip levels for alias-free playback at any pitch.

Analysis runs on a worker thread and is handed to the audio thread lock-free.

## Panel

Labels are drawn at runtime by `src/widgets/PanelLabels.hpp`, because panel SVGs must not
contain text and converting it to paths needs Inkscape. Once the panel is finished in
Inkscape (text converted with Path > Object to Path), remove `PanelLabels`.

## License

GPL-3.0-or-later. See LICENSE.txt. `src/dep/dr_wav.h` is public domain / MIT-0.
