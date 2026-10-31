#pragma once
#include "plugin.hpp"
#include "ExpanderMessages.hpp"

/** Per-marker CV expander. Snap it to the right of waveArtifact (or of another expander, up to 5
in a row). It adds CV + attenuverter for one marker's WAVE, GAIN, CUTOFF, DRIVE, FINE and PAN,
picked with the MARKER button, and has two LFOs (same as the markers' WAVE LFO: Hz or synced to
the main module's CLOCK) on outputs that can be patched anywhere, including its own inputs. */
struct WaveArtifactExpander : Module {
	static const int NUM_LFOS = 2;

	enum ParamId {
		/** 0..4 = marker 1..5 */
		MARKER_PARAM,
		ENUMS(AMT_PARAM, wa::NUM_MOD_TARGETS),
		ENUMS(LFO_RATE_PARAM, NUM_LFOS),
		ENUMS(LFO_SYNC_PARAM, NUM_LFOS),
		ENUMS(LFO_TRIPLET_PARAM, NUM_LFOS),
		ENUMS(LFO_DOTTED_PARAM, NUM_LFOS),
		/** wa::LfoShape of each LFO */
		ENUMS(LFO_SHAPE_PARAM, NUM_LFOS),
		/** Output level of each LFO, 0..100% of +-5 V (default 100%) */
		ENUMS(LFO_AMT_PARAM, NUM_LFOS),
		PARAMS_LEN
	};
	enum InputId {
		ENUMS(MOD_INPUT, wa::NUM_MOD_TARGETS),
		/** Pitch for this artifact only, 1 V/oct, added to the main V/OCT */
		VOCT_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(LFO_OUTPUT, NUM_LFOS),
		OUTPUTS_LEN
	};
	enum LightId {
		/** Green: linked and active. Red: next to waveArtifact's chain but beyond 5 expanders. */
		ENUMS(LINK_LIGHT, 2),
		/** Green/red per LFO for its positive/negative half */
		ENUMS(LFO_LIGHT, NUM_LFOS * 2),
		LIGHTS_LEN
	};

	wa::Lfo lfos[NUM_LFOS];
	/** Used while not linked (no clock): synced LFOs run at the 120 BPM fallback */
	wa::ClockTracker idleClock;
	dsp::ClockDivider lightDivider;

	// For the UI
	std::atomic<bool> linked{false};
	std::atomic<int> chainIndex{0};

	WaveArtifactExpander();
	~WaveArtifactExpander();
	void process(const ProcessArgs& args) override;
	int getMarker() {
		return clamp((int) std::round(params[MARKER_PARAM].getValue()), 0, wa::NUM_MARKERS - 1);
	}
};

/** True if `m` is waveArtifact's expander (checks the model, as Rack requires before using its messages). */
bool isWaveArtifactExpander(Module* m);
