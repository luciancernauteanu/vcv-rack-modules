#pragma once
#include "plugin.hpp"
#include "ExpanderMessages.hpp"

/** Per-marker CV expander. Snap it to the right of waveArtifact (or of another expander, up to 5
in a row). It adds CV + attenuverter for one marker's WAVE, GAIN, CUTOFF, DRIVE, FINE and PAN,
picked with the MARKER button, a GATE and V/OCT for it, and the marker's modulation: an LFO and an
AD envelope, each sent to one of the marker's settings with a depth (MarkerMod.hpp). Those are the
marker's own settings in the main module (so sequencer trigs can lock them); this panel shows and
sets them for its marker. The LFO also comes out of a jack. */
struct WaveArtifactExpander : Module {
	/** LFO params and lights keep room for the two LFOs this panel once had (IDs stay valid); only LFO 1 is used */
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
		/** No longer used: the AD is the marker's (MARKER_MOD_AD_* in the main module). Kept so later IDs stay valid. */
		ATTACK_PARAM,
		DECAY_PARAM,
		PERC_PARAM,
		/** Granular engine: 0..100% amounts of the START and DENSITY CVs. They take the place of the attenuverters
		of WAVE and FINE, which keep their own values for the wavetable engine. (GRAIN_SPREAD_AMT_PARAM is no longer
		used: PAN is PAN on both engines, with one attenuverter. It stays so later IDs and patches stay valid.) */
		GRAIN_START_AMT_PARAM,
		GRAIN_DENSITY_AMT_PARAM,
		GRAIN_SPREAD_AMT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		ENUMS(MOD_INPUT, wa::NUM_MOD_TARGETS),
		/** Pitch for this artifact only, 1 V/oct, added to the main V/OCT */
		VOCT_INPUT,
		/** While patched, this artifact sounds only while the gate is high (>= 1 V; off below 0.1 V) */
		GATE_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(LFO_OUTPUT, NUM_LFOS),
		/** No longer used (the gate envelope's output): kept so the ID stays valid */
		ENV_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		/** Green: linked and active. Red: next to waveArtifact's chain but beyond 5 expanders. */
		ENUMS(LINK_LIGHT, 2),
		/** Green/red per LFO for its positive/negative half */
		ENUMS(LFO_LIGHT, NUM_LFOS * 2),
		LIGHTS_LEN
	};

	dsp::ClockDivider lightDivider;
	dsp::SchmittTrigger gateTrigger;

	// For the UI
	std::atomic<bool> linked{false};
	/** The marker this expander works on (MARKER), for the panel's controls of the marker's modulation */
	std::atomic<int> marker{0};
	/** The main module runs the granular engine (relabels the CV inputs) */
	std::atomic<bool> granular{false};
	std::atomic<int> chainIndex{0};

	WaveArtifactExpander();
	~WaveArtifactExpander();
	void process(const ProcessArgs& args) override;
	int getMarker() {
		return clamp((int) std::round(params[MARKER_PARAM].getValue()), 0, wa::NUM_MARKERS - 1);
	}
	/** The amount knob of a CV input for an engine */
	static int amtParam(int target, bool grain) {
		if (grain && target == wa::MOD_WAVE)
			return GRAIN_START_AMT_PARAM;
		if (grain && target == wa::MOD_FINE)
			return GRAIN_DENSITY_AMT_PARAM;
		return AMT_PARAM + target;
	}
};

/** True if `m` is waveArtifact's expander (checks the model, as Rack requires before using its messages). */
bool isWaveArtifactExpander(Module* m);
