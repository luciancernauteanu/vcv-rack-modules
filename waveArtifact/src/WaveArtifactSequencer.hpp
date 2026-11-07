#pragma once
#include "plugin.hpp"
#include "ExpanderMessages.hpp"

/** Drum-machine step grid for Wave Artifacts. Snap it to the LEFT of the main module and choose SEQ with the PLAY
button. The clock is the main module's (the CLOCK input when patched, otherwise the internal tempo). Each marker has
its own row of up to 16 steps, each with a velocity, a length, and a clock multiplier / divider (CLOCK knob left of
the row): rows can run at different speeds and lengths (polyrhythms). */
struct WaveArtifactSequencer : Module {
	static const int ROWS = wa::NUM_MARKERS;
	static const int STEPS = 16;

	enum ParamId {
		/** Per row: -4..4 (whole steps). Right of the middle multiplies the clock, left divides it. */
		ENUMS(CLOCK_MULT_PARAM, ROWS),
		PARAMS_LEN
	};

	/** Steps per beat (quarter note) for a knob position: ...÷8 ÷4 ÷3 ÷2 | x1 | x2 x3 x4 x8. At x4 a step is a 16th. */
	static double stepsPerBeat(int index) {
		static const double table[9] = {1.0 / 8.0, 1.0 / 4.0, 1.0 / 3.0, 1.0 / 2.0, 1.0, 2.0, 3.0, 4.0, 8.0};
		return table[std::min(std::max(index, -4), 4) + 4];
	}
	static std::string multiplierText(int index) {
		static const char* const table[9] = {"/8", "/4", "/3", "/2", "x1", "x2", "x3", "x4", "x8"};
		return table[std::min(std::max(index, -4), 4) + 4];
	}
	enum InputId { INPUTS_LEN };
	enum OutputId { OUTPUTS_LEN };
	enum LightId { LIGHTS_LEN };

	/** 0 = off, else the velocity 0..1. Written by the UI, read by process(). Saved with the patch. */
	float velocity[ROWS][STEPS] = {};
	int length[ROWS] = {16, 16, 16, 16, 16};
	/** The step each row played last (-1 before the first), for the display */
	std::atomic<int> playing[ROWS];
	std::atomic<bool> linked{false};

	WaveArtifactSequencer();
	~WaveArtifactSequencer();
	void process(const ProcessArgs& args) override;
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
	void onReset(const ResetEvent& e) override;

	void clearRow(int r);
	void clearAll();
	/** A starting pattern: a kick on 1 5 9 13, a snare on 5 13, a hat on every other step */
	void loadDefault();

private:
	uint32_t seenReset = 0, fires[ROWS] = {};
	/** The step number each row's clock is at (steps since reset), to see when it moves on */
	long lastIndex[ROWS] = {};
	float lastVelocity[ROWS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	int position[ROWS] = {-1, -1, -1, -1, -1};
	static const int WARM_UP_SAMPLES = 4;
	int warmUp = WARM_UP_SAMPLES;
	void tickRow(int r);
	void restart();
};

bool isWaveArtifactSequencer(Module* m);
