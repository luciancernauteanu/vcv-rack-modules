#pragma once
#include <atomic>

#include "PrismaEngine.hpp"
#include "plugin.hpp"

/** Nur Prisma: four filter cores that move together (CUTOFF) and fan apart (SPREAD).
Param, input, output and light IDs are append only: saved patches refer to them by number. The MOTION IDs are
reserved here (no knobs, no jacks): the MOTION column comes with the branch feat/nur-prisma-motion. */
struct NurPrisma : engine::Module {
	enum ParamId {
		CUTOFF_PARAM,
		SPREAD_PARAM,
		RES_PARAM,
		TYPE_PARAM,
		SHAPE_PARAM,
		STACK_PARAM,
		DRIVE_PARAM,
		WIDTH_PARAM,
		MIX_PARAM,
		NOISE_PARAM,
		COLOR_PARAM,
		ANALOG_PARAM,
		DIGITAL_PARAM,
		// Reserved for MOTION (feat/nur-prisma-motion)
		DRIFT_PARAM,
		DRIFT_RATE_PARAM,
		ORBIT_PARAM,
		ORBIT_RATE_PARAM,
		SWEEP_RATE_PARAM,
		SWEEP_SHAPE_PARAM,
		SWEEP_CUT_PARAM,
		SWEEP_FAN_PARAM,
		STRUM_PARAM,
		STRIKE_PARAM,
		BAND_PARAM,
		/** Each core's own share of SPREAD (the dots on the prism ring) */
		ENUMS(CORE_SPREAD_PARAM, 4),
		/** Per-core TYPE / RES / BAND (used once the core owns them), LEVEL and PAN: edited in focus (core bullets) */
		ENUMS(CORE_TYPE_PARAM, 4),
		ENUMS(CORE_RES_PARAM, 4),
		ENUMS(CORE_BAND_PARAM, 4),
		ENUMS(CORE_LEVEL_PARAM, 4),
		ENUMS(CORE_PAN_PARAM, 4),
		/** Per-core CUTOFF (the centre knob while the core is in focus) */
		ENUMS(CORE_CUTOFF_PARAM, 4),
		PARAMS_LEN
	};
	enum InputId {
		IN_L_INPUT,
		IN_R_INPUT,
		VOCT_INPUT,
		CUTOFF_INPUT,
		SPREAD_INPUT,
		RES_INPUT,
		STACK_INPUT,
		// Reserved for MOTION
		CLOCK_INPUT,
		RESET_INPUT,
		PING_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_L_OUTPUT,
		OUT_R_OUTPUT,
		CORES_OUTPUT,
		// Reserved for MOTION
		SWEEP_OUTPUT,
		ORBIT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(SHAPE_LIGHT, 5),
		// Reserved for MOTION
		ENUMS(SWEEP_SHAPE_LIGHT, 3),
		LIGHTS_LEN
	};

	static const int NUM_CORES = 4;
	static const int NUM_SHAPES = 5;

	/** Live core frequencies for the prism ring, 0..1 = 20 Hz..20 kHz on a log scale (UI reads, audio writes) */
	std::atomic<float> corePos[NUM_CORES];

	np::PrismaEngine engine;
	np::EngineOutput engineOut;

	// Menu settings (saved in the patch)
	int scale = 0;
	int chord = 0;
	int pingMode = np::PING_ALL;
	/** Poly input: one voice per core (true) or summed to one signal for all cores */
	bool polySplit = true;
	bool quantizeCutoff = false;
	/** np::LevelComp: Off, Gentle, Full */
	int levelComp = np::LEVEL_FULL;

	/** Which core the knobs edit: -1 = ALL (UI state, saved in the patch) */
	int focus = -1;
	/** Per core: which of TYPE / RES / BAND it owns (bits OWN_*); written by the UI, read by the audio thread */
	enum { OWN_TYPE = 1, OWN_RES = 2, OWN_BAND = 4, OWN_CUTOFF = 8, OWN_PAN = 16, NUM_OWN = 5 };
	std::atomic<int> own[NUM_CORES];
	/** UI thread: last seen per-core TYPE / RES / BAND / CUTOFF / PAN values, to notice an edit (NAN = not seen yet) */
	float lastSeen[NUM_CORES][NUM_OWN];
	/** UI thread: what core i's TYPE / RES / BAND / CUTOFF / PAN twin shows while it follows (the global knob, or
	where SPREAD / WIDTH put the core) */
	float followValue(int core, int k);

	dsp::SchmittTrigger pingTrigger[16];
	/** Knobs and CV are read every BLOCK samples */
	int paramCounter = 0;

	NurPrisma();
	void process(const ProcessArgs& args) override;
	void onSampleRateChange(const SampleRateChangeEvent& e) override;
	void onReset(const ResetEvent& e) override;
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
	/** Reads knobs and CV into the engine's settings (control rate) */
	void readParams();
	void rerollCharacter();
	/** UI thread: focus a core (or -1 = ALL). A core's TYPE / RES / BAND it does not own yet start from the global knobs. */
	void setFocus(int core);
	/** UI thread: the core follows the global TYPE / RES / BAND / CUTOFF and WIDTH again (level and spread are kept). */
	void followGlobal(int core);
	/** UI thread: back to defaults: follows, level 100%, pan centre, spread 100%. */
	void resetCore(int core);
	/** UI thread, every frame: a per-core TYPE / RES / BAND that changed (knob, MIDI map, automation) becomes owned. */
	void checkOwnership();
	/** True if the core differs from plain "follow everything" (for the bullet's ring) */
	bool coreIsCustom(int core);
	void rerollRandomShape();
};
