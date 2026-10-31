#pragma once
#include <atomic>
#include <mutex>
#include <thread>

#include "plugin.hpp"
#include "Lfo.hpp"
#include "MorphEngine.hpp"
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
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		/** Each artifact's own output (its chain alone, before the master). L carries mono when R is unpatched. */
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

	// Written by process(), read by the displays.
	std::atomic<float> shownIndex[NUM_MARKERS];
	std::atomic<float> shownHeard[NUM_MARKERS];
	std::atomic<float> shownPosition{0.f};
	std::atomic<float> shownPreview{0.f};
	/** An expander is modulating this artifact (shown as a CV tag on the screen) */
	std::atomic<bool> shownCv[NUM_MARKERS];

	// Audio thread only
	wa::MorphEngine engine;
	wa::ClockTracker clock;
	wa::Lfo lfos[NUM_MARKERS];
	wa::Lfo posLfo;
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
	wa::TableCountMode tableCountMode = wa::TABLES_AUTO;
	/** How soft the artifacts are (see wa::TableTone); re-analyzes when changed. */
	wa::TableTone tableTone = wa::TONE_SMOOTH;
	int selectedMarker = 0;


	WaveArtifact();
	~WaveArtifact();

	void process(const ProcessArgs& args) override;
	void onSampleRateChange(const SampleRateChangeEvent& e) override;
	void onReset(const ResetEvent& e) override;

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

	/** UI thread. Reads the file and starts analysis on the worker thread. */
	void loadFile(const std::string& path);
	/** UI thread. Re-runs analysis on the loaded bytes, e.g. after the table count changes. */
	void startAnalysis();
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
