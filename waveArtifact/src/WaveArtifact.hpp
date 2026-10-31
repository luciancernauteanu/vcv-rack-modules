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
		/** Filter cutoff (was band-pass frequency; same ID). 0 = filter off. */
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
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		POS_INPUT,
		/** 1 pulse per quarter note, for LFO sync */
		CLOCK_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
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
	std::atomic<bool> analyzing{false};

	/** Table auditioned instead of the morph, or -1. Written by the UI, read by process(). Not saved. */
	std::atomic<int> previewTable{-1};

	// Written by process(), read by the displays.
	std::atomic<float> shownIndex[NUM_MARKERS];
	std::atomic<float> shownHeard[NUM_MARKERS];
	std::atomic<float> shownPosition{0.f};
	std::atomic<float> shownPreview{0.f};

	// Audio thread only
	wa::MorphEngine engine;
	wa::ClockTracker clock;
	wa::Lfo lfos[NUM_MARKERS];
	int displayCounter = 0;

	// UI thread only
	std::thread worker;
	std::vector<uint8_t> wavBytes;
	std::string fileName;
	wa::TableCountMode tableCountMode = wa::TABLES_AUTO;
	int selectedMarker = 0;

	/** Closest two markers may get on the bar. */
	static constexpr float MIN_MARKER_GAP = 0.01f;

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
	/** Range marker i may be dragged in without crossing its neighbors. */
	void markerDragRange(int i, float* lo, float* hi);
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
