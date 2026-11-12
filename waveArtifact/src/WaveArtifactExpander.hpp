#pragma once
#include "plugin.hpp"
#include "ExpanderMessages.hpp"

/** Per-marker CV expander. Snap it to the right of waveArtifact (or of another expander, up to 5
in a row). It adds four CV inputs for one marker, picked with the MARKER button, each with an
attenuverter and sent to one of the marker's knobs (the name above the jack is a menu; each engine
keeps its own four, starting on its main controls: wa::CV_DEFAULTS), a GATE and V/OCT for it, and
the marker's modulation: an LFO and an AD envelope, each sent to one of the marker's settings with a
depth (MarkerMod.hpp). Those are the marker's own settings in the main module (so sequencer trigs
can lock them); this panel shows and sets them for its marker. The LFO also comes out of a jack. */
struct WaveArtifactExpander : Module {
	/** LFO params and lights keep room for the two LFOs this panel once had (IDs stay valid); only LFO 1 is used */
	static const int NUM_LFOS = 2;
	/** The old six fixed inputs (WAVE GAIN CUTOFF DRIVE FINE PAN): their IDs stay so later IDs stay valid */
	static const int OLD_CV_INPUTS = 6;

	enum ParamId {
		/** 0..4 = marker 1..5 */
		MARKER_PARAM,
		/** No longer used: the amounts of the old six fixed inputs (read once from an older patch, see fromJson) */
		ENUMS(AMT_PARAM, OLD_CV_INPUTS),
		ENUMS(LFO_RATE_PARAM, NUM_LFOS),
		ENUMS(LFO_SYNC_PARAM, NUM_LFOS),
		ENUMS(LFO_TRIPLET_PARAM, NUM_LFOS),
		ENUMS(LFO_DOTTED_PARAM, NUM_LFOS),
		/** wa::LfoShape of each LFO */
		ENUMS(LFO_SHAPE_PARAM, NUM_LFOS),
		/** Output level of each LFO, 0..100% of +-10 V (default 100%) */
		ENUMS(LFO_AMT_PARAM, NUM_LFOS),
		/** No longer used: the AD is the marker's (MARKER_MOD_AD_* in the main module). Kept so later IDs stay valid. */
		ATTACK_PARAM,
		DECAY_PARAM,
		PERC_PARAM,
		/** No longer used: the old granular amounts (GRAIN_START_AMT is read once from an older patch) */
		GRAIN_START_AMT_PARAM,
		GRAIN_DENSITY_AMT_PARAM,
		GRAIN_SPREAD_AMT_PARAM,
		/** Per engine (0 WAVE, 1 GRAIN, 2 SAMPLE) and input: where it goes (wa::CvDest) and how much (-1..1) */
		ENUMS(CV_DEST_PARAM, wa::NUM_CV_ENGINES * wa::NUM_CV_SLOTS),
		ENUMS(CV_AMT_PARAM, wa::NUM_CV_ENGINES * wa::NUM_CV_SLOTS),
		PARAMS_LEN
	};
	enum InputId {
		/** The four CV inputs are the first four; the last two are no longer on the panel */
		ENUMS(MOD_INPUT, OLD_CV_INPUTS),
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
	/** The main module's engine: 0 wavetable, 1 granular, 2 sample (picks the four inputs' set) */
	std::atomic<int> engine{0};
	std::atomic<int> chainIndex{0};

	WaveArtifactExpander();
	~WaveArtifactExpander();
	void process(const ProcessArgs& args) override;
	json_t* dataToJson() override;
	void fromJson(json_t* rootJ) override;
	/** An older patch (no "cvSlots" in its data): its six fixed inputs' amounts onto the four inputs */
	void migrateOldInputs();
	int getMarker() {
		return clamp((int) std::round(params[MARKER_PARAM].getValue()), 0, wa::NUM_MARKERS - 1);
	}
	static int destParam(int engine, int slot) {
		return CV_DEST_PARAM + engine * wa::NUM_CV_SLOTS + slot;
	}
	static int amtParam(int engine, int slot) {
		return CV_AMT_PARAM + engine * wa::NUM_CV_SLOTS + slot;
	}
	int destOf(int engine, int slot) {
		return clamp((int) std::round(params[destParam(engine, slot)].getValue()), 0, wa::NUM_CV_DESTS - 1);
	}
};

/** True if `m` is waveArtifact's expander (checks the model, as Rack requires before using its messages). */
bool isWaveArtifactExpander(Module* m);
