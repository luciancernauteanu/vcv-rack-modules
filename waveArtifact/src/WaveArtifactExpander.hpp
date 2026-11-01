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
		/** Gate envelope (applies while GATE is patched): attack and decay times, 0..1 on a log scale */
		ATTACK_PARAM,
		DECAY_PARAM,
		/** 0 = gate (holds while high, DECAY after it falls), 1 = percussive AD (decays even while held) */
		PERC_PARAM,
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
		/** The gate envelope, 0..10 V (0 V without a gate) */
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

	wa::Lfo lfos[NUM_LFOS];
	/** Used while not linked (no clock): synced LFOs run at the 120 BPM fallback */
	wa::ClockTracker idleClock;
	dsp::ClockDivider lightDivider;
	dsp::SchmittTrigger gateTrigger;

	/** Gate envelope state (audio thread) */
	enum EnvStage {ENV_IDLE, ENV_ATTACK, ENV_HOLD, ENV_DECAY};
	EnvStage envStage = ENV_IDLE;
	float env = 0.f;

	/** Attack / decay time in seconds for a knob value; MIN_ENV_TIME or below means instant. */
	static constexpr float MIN_ENV_TIME = 0.005f;
	static float envTime(float knob, float maxTime) {
		return MIN_ENV_TIME * std::pow(maxTime / MIN_ENV_TIME, knob);
	}
	float stepEnvelope(bool gateHigh, bool risingEdge, float sampleTime);

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
