#pragma once
#include <atomic>
#include <mutex>
#include <thread>

#include "plugin.hpp"
#include "GrainEngine.hpp"
#include "Lfo.hpp"
#include "MorphEngine.hpp"
#include "PolyVoices.hpp"
#include "WavetableBank.hpp"

struct WaveArtifact : Module {
	static const int NUM_MARKERS = wa::NUM_MARKERS;

	enum ParamId {
		POSITION_PARAM,
		POS_ATTEN_PARAM,
		FREQ_PARAM,
		LOAD_PARAM,
		// Marker state lives in params so it is saved automatically and the UI never writes
		// module fields that process() reads. Used from M4 on.
		ENUMS(MARKER_ACTIVE_PARAM, NUM_MARKERS),
		ENUMS(MARKER_POS_PARAM, NUM_MARKERS),
		/** Wavetable position 0..1 across the bank (start..end of the sample) */
		ENUMS(MARKER_WAVE_PARAM, NUM_MARKERS),
		/** Filter cutoff (was band-pass frequency; same ID). 20 Hz .. 20 kHz; the filter is always on. */
		ENUMS(MARKER_CUTOFF_PARAM, NUM_MARKERS),
		ENUMS(MARKER_PITCH_PARAM, NUM_MARKERS),
		ENUMS(MARKER_PAN_PARAM, NUM_MARKERS),
		ARRANGE_PARAM,
		// Appended (not grouped with the other marker params) so existing param IDs stay stable.
		ENUMS(MARKER_GAIN_PARAM, NUM_MARKERS),
		ENUMS(MARKER_RES_PARAM, NUM_MARKERS),
		/** WAVE glide time 0..1 -> 20 ms * 100^v (20 ms .. 2 s) */
		ENUMS(MARKER_SMOOTH_PARAM, NUM_MARKERS),
		/** WAVE LFO: rate knob 0..1 (Hz when free, note value when synced), amount, free/sync */
		ENUMS(MARKER_LFO_RATE_PARAM, NUM_MARKERS),
		ENUMS(MARKER_LFO_AMT_PARAM, NUM_MARKERS),
		ENUMS(MARKER_LFO_SYNC_PARAM, NUM_MARKERS),
		/** Note values the synced rate knob offers besides straight ones (set in RATE's right-click menu) */
		ENUMS(MARKER_LFO_TRIPLET_PARAM, NUM_MARKERS),
		ENUMS(MARKER_LFO_DOTTED_PARAM, NUM_MARKERS),
		/** wa::FilterMode: LP, BP, HP, notch */
		ENUMS(MARKER_FILTER_MODE_PARAM, NUM_MARKERS),
		ENUMS(MARKER_DRIVE_PARAM, NUM_MARKERS),
		/** Fine tune, semitones -1..1 (shown in cents) */
		ENUMS(MARKER_FINE_PARAM, NUM_MARKERS),
		/** Master filter: 0 = off, 1..4 = LP, BP, HP, notch */
		MASTER_MODE_PARAM,
		MASTER_CUTOFF_PARAM,
		MASTER_RES_PARAM,
		MASTER_SPREAD_PARAM,
		MASTER_FDBK_PARAM,
		MASTER_DRIVE_PARAM,
		/** CV amounts for the master knobs (-100%..100%, default 0) */
		MASTER_CUTOFF_AMT_PARAM,
		MASTER_RES_AMT_PARAM,
		MASTER_DRIVE_AMT_PARAM,
		MASTER_SPREAD_AMT_PARAM,
		MASTER_FDBK_AMT_PARAM,
		/** wa::LfoShape of each marker's WAVE LFO */
		ENUMS(MARKER_LFO_SHAPE_PARAM, NUM_MARKERS),
		/** The position LFO: same controls as a marker's. It moves POSITION, folding at the ends. */
		POS_LFO_SHAPE_PARAM,
		POS_LFO_RATE_PARAM,
		POS_LFO_AMT_PARAM,
		POS_LFO_SYNC_PARAM,
		POS_LFO_TRIPLET_PARAM,
		POS_LFO_DOTTED_PARAM,
		/** 0 = PITCH steps in whole semitones, 1 = free (continuous) */
		ENUMS(MARKER_PITCH_FREE_PARAM, NUM_MARKERS),
		/** Output gain 0..200% (100% = unity), before the output protection */
		MASTER_GAIN_PARAM,
		/** Index into wa::PPQN_VALUES: clock pulses per quarter note (right-click menu) */
		CLOCK_PPQN_PARAM,
		/** Internal tempo, used while CLOCK is unpatched (20..300 BPM) */
		INTERNAL_BPM_PARAM,
		/** Momentary: restart the beat grid and every LFO (same as a RESET gate) */
		RESTART_PARAM,
		/** Latching: silences every output (main and artifact outs) with a 5 ms fade */
		STOP_PARAM,
		/** Position LFO smoothing 0..1 -> 20 ms * 100^v (20 ms .. 2 s): a one-pole on the LFO alone, so the
		knob and CV stay fast and a saw / square edge becomes a ramp instead of a jump of the playhead */
		POS_LFO_SMOOTH_PARAM,
		/** Master GAIN CV amount -100%..100% (default 0): 10 V at 100% = the whole knob range (+200%) */
		MASTER_GAIN_AMT_PARAM,
		// ---- Granular engine (appended, so every earlier ID and saved patch stays valid) ----
		/** 0 = wavetable engine (default), 1 = granular engine: the knobs change meaning */
		ENGINE_MODE_PARAM,
		/** DENSITY: grains per second (free) or note value (SYNC), 0..1 */
		ENUMS(MARKER_GRAIN_DENSITY_PARAM, NUM_MARKERS),
		/** SIZE: ms (free) or x grain interval (SYNC), 0..1 */
		ENUMS(MARKER_GRAIN_SIZE_PARAM, NUM_MARKERS),
		/** SPRAY: random offset of the grain start, 0..1 */
		ENUMS(MARKER_GRAIN_SPRAY_PARAM, NUM_MARKERS),
		/** wa::GrainWindow */
		ENUMS(MARKER_GRAIN_WINDOW_PARAM, NUM_MARKERS),
		/** Grains on the clock grid / SCAN locked to the beat (default on), note kinds for DENSITY */
		ENUMS(MARKER_GRAIN_SYNC_PARAM, NUM_MARKERS),
		ENUMS(MARKER_GRAIN_TRIPLET_PARAM, NUM_MARKERS),
		ENUMS(MARKER_GRAIN_DOTTED_PARAM, NUM_MARKERS),
		/** SPREAD: random pan scatter 0..1 */
		ENUMS(MARKER_GRAIN_SPREAD_PARAM, NUM_MARKERS),
		/** P.SPRD: random pitch scatter 0..1 (up to +-12 st) */
		ENUMS(MARKER_GRAIN_PSCATTER_PARAM, NUM_MARKERS),
		/** SCAN -1..1: speed (free) or slice length in beats (SYNC); 0 = stays at START */
		ENUMS(MARKER_GRAIN_SCAN_PARAM, NUM_MARKERS),
		/** wa::GrainDirection (right-click menu) */
		ENUMS(MARKER_GRAIN_DIR_PARAM, NUM_MARKERS),
		/** 1 = the random scatter repeats every loop cycle while synced */
		GRAIN_SEEDED_PARAM,
		// ---- POLY play mode (appended) ----
		/** 0 = MORPH (the playhead crossfades neighboring markers, default), 1 = POLY (it starts their notes) */
		PLAY_MODE_PARAM,
		/** Each marker's ADSR in POLY: attack and decay / release 0..1 on a log scale (wa::adsrAttackSeconds,
		wa::adsrFallSeconds), sustain 0..1 */
		ENUMS(MARKER_ATTACK_PARAM, NUM_MARKERS),
		ENUMS(MARKER_DECAY_PARAM, NUM_MARKERS),
		ENUMS(MARKER_SUSTAIN_PARAM, NUM_MARKERS),
		ENUMS(MARKER_RELEASE_PARAM, NUM_MARKERS),
		/** How long a marker's note stays on (wa::adsrHoldSeconds); at the top, until another marker starts */
		ENUMS(MARKER_HOLD_PARAM, NUM_MARKERS),
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		POS_INPUT,
		/** Clock for LFO sync; pulses per quarter note set by CLOCK_PPQN_PARAM */
		CLOCK_INPUT,
		MASTER_CUTOFF_INPUT,
		MASTER_RES_INPUT,
		MASTER_DRIVE_INPUT,
		MASTER_SPREAD_INPUT,
		MASTER_FDBK_INPUT,
		/** Restarts the beat count and every LFO (next clock pulse = start of a bar) */
		RESET_INPUT,
		/** Master GAIN CV (scaled by MASTER_GAIN_AMT_PARAM): a VCA on the main outputs */
		MASTER_GAIN_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		/** Each artifact's own output (its chain alone, after the position crossfade, before the master). L carries mono when R is unpatched. */
		ENUMS(ARTIFACT_L_OUTPUT, NUM_MARKERS),
		ENUMS(ARTIFACT_R_OUTPUT, NUM_MARKERS),
		/** Beat clock: a 10 V, 5 ms trigger per quarter note from the clock in use (internal or CLOCK) */
		CLOCK_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	// Bank handoff. The worker publishes to pendingBank. process() moves pending -> active and
	// the old active -> retired, but only once retired is empty. The UI thread deletes retired.
	// So only the UI thread ever frees a bank, and never while process() can still read it.
	std::atomic<wa::WavetableBank*> pendingBank{nullptr};
	std::atomic<wa::WavetableBank*> activeBank{nullptr};
	std::atomic<wa::WavetableBank*> retiredBank{nullptr};
	/** Changes whenever the active bank changes (load, re-analysis, Initialize), so displays that
	cache something per bank (the wavetable map) know when to rebuild. Never compare bank pointers:
	a new bank can reuse a freed one's address. */
	std::atomic<int> bankSerial{0};
	std::atomic<bool> analyzing{false};
	/** Bumped by Initialize: an analysis started before it is discarded when it finishes. */
	std::atomic<int> analysisGeneration{0};
	/** Held while the worker publishes a bank and while Initialize clears them. */
	std::mutex publishMutex;

	/** Table auditioned instead of the morph, or -1. Written by the UI, read by process(). Not saved. */
	std::atomic<int> previewTable{-1};
	/** POLY: the marker whose slot the mouse is holding down, which plays it, or -1. Written by the UI. */
	std::atomic<int> pressedMarker{-1};

	// Written by process(), read by the displays.
	std::atomic<float> shownIndex[NUM_MARKERS];
	std::atomic<float> shownHeard[NUM_MARKERS];
	std::atomic<float> shownPosition{0.f};
	std::atomic<float> shownPreview{0.f};
	/** An expander is modulating this artifact (shown as a CV tag on the screen) */
	std::atomic<bool> shownCv[NUM_MARKERS];
	/** Granular engine: where each cloud reads (0..1 of the sample), and a counter of its grain starts
	with the latest start, so the display can flash a grain. */
	std::atomic<float> shownGrainRead[NUM_MARKERS];
	std::atomic<float> shownGrainPos[NUM_MARKERS];
	std::atomic<float> shownGrainSeconds[NUM_MARKERS];
	std::atomic<uint32_t> shownGrainCount[NUM_MARKERS];

	// Audio thread only
	wa::MorphEngine engine;
	wa::GrainEngine grainEngine;
	/** Engine in use (0 wavetable, 1 granular); follows ENGINE_MODE_PARAM after a fade to silence */
	int engineApplied = 0;
	uint32_t grainEventsSeen = 0;
	wa::PolyVoices voices;
	bool polyRunning = false;
	wa::ClockTracker clock;
	wa::Lfo lfos[NUM_MARKERS];
	wa::Lfo posLfo;
	/** The position LFO after its SMOOTH one-pole */
	wa::LfoSmoother posLfoSmoother;
	dsp::SchmittTrigger resetTrigger;
	dsp::PulseGenerator clockOutPulse;
	bool restartHeld = false;
	/** Set by the UI (menu) to restart the LFOs on the next sample */
	std::atomic<bool> restartRequest{false};
	int displayCounter = 0;

	// UI thread only
	std::thread worker;
	std::vector<uint8_t> wavBytes;
	std::string fileName;
	/** Full path of the last WAV asked for (even if it failed to load), for previous / next in its folder */
	std::string filePath;
	/** WAVs in filePath's folder, sorted; scanned on each load (not per frame) */
	std::vector<std::string> folderWavs;
	wa::TableCountMode tableCountMode = wa::TABLES_AUTO;
	/** How soft the artifacts are (see wa::TableTone); re-analyzes when changed. */
	wa::TableTone tableTone = wa::TONE_SMOOTH;
	int selectedMarker = 0;
	/** First param ID of every setting a marker copy carries: everything per marker except on/off and bar position */
	static const int NUM_MARKER_SETTINGS = 33;
	static const int MARKER_SETTING_PARAMS[NUM_MARKER_SETTINGS];
	/** The settings copied last (not saved with the patch), and the marker they came from, or -1 */
	float markerClipboard[NUM_MARKER_SETTINGS] = {};
	int markerClipboardFrom = -1;
	/** The selected marker's settings at the last linkStep, to see what changed */
	float linkLast[NUM_MARKER_SETTINGS] = {};
	int linkSelected = -1;
	/** First param ID of each part of a marker's envelope */
	static const int NUM_ENVELOPE_PARAMS = 5;
	static const int MARKER_ENVELOPE_PARAMS[NUM_ENVELOPE_PARAMS];
	/** Shift + a per-marker control: the setting being moved on every marker, or -1 (see linkStep) */
	int linkSetting = -1;
	/** The marker the move is made on */
	int linkMarker = -1;
	/** How far a Shift drag has gone past the moved knob's own limit, in its units (see LinkedKnob) */
	float linkBeyond = 0.f;
	/** Each marker's value and the moved marker's own value when the move began */
	float linkBase[NUM_MARKERS] = {};
	float linkBaseValue = 0.f;


	WaveArtifact();
	~WaveArtifact();

	void process(const ProcessArgs& args) override;
	void onSampleRateChange(const SampleRateChangeEvent& e) override;
	void onReset(const ResetEvent& e) override;
	/** Randomize also randomizes the expanders snapped on to the right (each keeps its own
	randomizeEnabled flags: MARKER, LFO shape, SYNC, T, D and PERC stay as they are). */
	void onRandomize(const RandomizeEvent& e) override;

	// Marker editing, UI thread. All state is in params, so these are saved and undo-free like knobs.
	bool isMarkerActive(int i);
	int activeMarkerCount();
	/** Turns on the lowest-numbered free marker at `pos`, with default settings and the wavetable
	under that bar position. Returns it, or -1 when all 5 are in use. */
	int addMarker(float pos);
	/** Turns a marker off. The last active marker cannot be deleted. */
	void deleteMarker(int i);
	/** Sets a marker's wavetable to table `t` of the loaded bank. Turns the marker on (in the
	widest gap of the bar) if it was off, and selects it. */
	void assignTable(int i, int t);
	/** Nearest table to a marker's WAVE value, or -1 without a bank. */
	int markerTable(int i);
	/** Remembers all of a marker's settings, as they are now, for pasteMarker. */
	void copyMarker(int i);
	/** Overwrites a marker's settings with the copied ones and selects it. Its on/off and bar position stay. */
	void pasteMarker(int i);
	/** Call every frame, with the param under a Shift-held mouse (-1 if none) and whether its button is down.
	When the selected marker's setting changes there, every other active marker moves by the same amount from
	where it was when the move began, so the spacing comes back if one hits a limit. A drag past the moved knob's
	own limit keeps moving the others (linkBeyond). Switches and lists take the same value instead. Returns true once when a move has ended: the UI makes its undo step, then calls
	linkFinish(). */
	bool linkStep(int paramId, bool dragging);
	void linkFinish();

	/** UI thread. Reads the file and starts analysis on the worker thread. */
	void loadFile(const std::string& path);
	/** UI thread. Re-runs analysis on the loaded bytes, e.g. after the table count changes. */
	void startAnalysis();
	/** UI thread. Loads the previous (dir = -1) or next (dir = +1) WAV in filePath's folder,
	sorted by name, wrapping around. Does nothing without a path. */
	void loadSibling(int dir);
	/** UI thread. True when filePath's folder holds another WAV to step to. */
	bool hasSiblings();
	/** UI thread. Frees a bank that process() has retired. */
	void uiStep();
	/** UI thread. All 5 markers on, evenly spaced left to right, wavetables spread from the start
	to the end of the sample, per-marker settings back to default. */
	void autoArrange();

	std::string getStatus();
	void setStatus(const std::string& s);

private:
	std::mutex statusMutex;
	std::string status;
};

/** UI thread. File dialog, then loadFile(). */
void openLoadDialog(WaveArtifact* module);
