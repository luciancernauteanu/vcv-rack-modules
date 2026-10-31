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
		ENUMS(MARKER_BP_PARAM, NUM_MARKERS),
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

	// Audio thread only
	wa::MorphEngine engine;
	wa::ClockTracker clock;
	wa::Lfo lfos[NUM_MARKERS];

	// UI thread only
	std::thread worker;
	std::vector<uint8_t> wavBytes;
	std::string fileName;
	wa::TableCountMode tableCountMode = wa::TABLES_AUTO;
	int selectedMarker = 0;

	WaveArtifact();
	~WaveArtifact();

	void process(const ProcessArgs& args) override;
	void onSampleRateChange(const SampleRateChangeEvent& e) override;

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
