#pragma once
#include "plugin.hpp"
#include "ExpanderMessages.hpp"

/** Drum-machine step grid for Wave Artifacts. Snap it to the LEFT of the main module and choose SEQ with the PLAY
button. The clock is the main module's (the CLOCK input when patched, otherwise the internal tempo). Each marker has
its own row of up to 16 steps, each with a velocity, a length, and a clock multiplier / divider (CLOCK knob left of
the row): rows can run at different speeds and lengths (polyrhythms).

Elektron style, each step can also carry parameter locks (its own value for any of the marker's settings), a trig
condition (chance, A:B) and slide (see StepLocks.hpp). */
struct WaveArtifactSequencer : Module {
	static const int ROWS = wa::NUM_MARKERS;
	static const int STEPS = 16;

	enum ParamId {
		/** Per row: -4..4 (whole steps). Right of the middle multiplies the clock, left divides it. */
		ENUMS(CLOCK_MULT_PARAM, ROWS),
		/** Unused: was FILL (removed). Kept so the params after it keep their IDs in saved patches. */
		UNUSED_FILL_PARAM,
		/** Latching REC: live record. While the pattern plays, the selected row's knobs are written into each of its lit
		steps as the playhead passes (see WaveArtifact::recordStep) */
		REC_PARAM,
		/** Momentary RND: random locks on the lit steps of the lane's row (Shift: every row), see randomizeLocks() */
		RND_PARAM,
		/** 0..1: how many of the lockable settings each lit step gets locked by RND (0 = none: RND clears the locks,
		1 = every one) */
		RND_AMOUNT_PARAM,
		/** Unused: were COND RND (removed) and its own amount. Kept so the params after them keep their IDs in saved
		patches. */
		UNUSED_COND_RND_PARAM,
		UNUSED_COND_AMOUNT_PARAM,
		/** Momentary COND CLR: every condition of the lane's row back to Always (Shift: every row) */
		COND_CLR_PARAM,
		/** Per row: the MOD output's level, 0..1 of 10 V (default 1, full). Last, so older patches keep their IDs. */
		ENUMS(MOD_LEVEL_PARAM, ROWS),
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
	enum InputId {
		INPUTS_LEN
	};
	enum OutputId {
		/** Per row: 10 V from each trig that plays for half a step; through a group of tied trigs (a long gate) it
		stays high until half way through the group's last step. The clock is unchanged. */
		ENUMS(GATE_OUTPUT, ROWS),
		/** Per row: the velocity of the step that played last, 0..10 V times the row's MOD level, held until the next one
		plays; a step with slide glides there over its slide x one step (as the locks do) */
		ENUMS(MOD_OUTPUT, ROWS),
		OUTPUTS_LEN
	};
	/** Share of a step a trig's GATE stays high */
	static constexpr double GATE_LENGTH = 0.5;
	enum LightId {
		REC_LIGHT,
		LIGHTS_LEN
	};

	/** 0 = off, else the velocity 0..1. Written by the UI, read by process(). Saved with the patch. */
	float velocity[ROWS][STEPS] = {};
	/** Each step's locks, condition and slide. Written by the UI, read by process(). Saved with the patch. */
	wa::StepData stepData[ROWS][STEPS];
	int length[ROWS] = {16, 16, 16, 16, 16};
	/** The step each row played last (-1 before the first), for the display */
	std::atomic<int> playing[ROWS];
	/** The row's marker is on (from the main module): a row whose marker is off keeps time, plays nothing, and is
	drawn dimmed */
	std::atomic<bool> rowOn[ROWS];
	std::atomic<bool> linked{false};

	// UI thread only
	/** The selected step (a click on it): the main module shows its values under its knobs, and turning one of that
	marker's knobs locks the value on this step. -1 = none. Not saved. */
	int selRow = -1, selStep = -1;
	/** The lane under the grid: which row, and which setting (an index into WaveArtifact::MARKER_SETTING_PARAMS) */
	int laneRow = 0, laneSetting = 9;
	/** The lane shows the row's trig conditions instead of a setting's locks */
	bool laneCondition = false;
	/** The lane shows setting k's locks (what is being locked) */
	void showLock(int k) {
		laneSetting = k;
		laneCondition = false;
	}

	WaveArtifactSequencer();
	~WaveArtifactSequencer();
	void process(const ProcessArgs& args) override;
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
	void onReset(const ResetEvent& e) override;

	bool hasSelection() const {
		return selRow >= 0 && selRow < ROWS && selStep >= 0 && selStep < STEPS;
	}
	void select(int r, int s) {
		selRow = r;
		selStep = s;
		rowSel = -1;
		if (r >= 0)
			laneRow = r;
	}
	/** A whole row selected with Ctrl + A (for Ctrl + C), or -1. UI thread, not saved. */
	int rowSel = -1;

	/** A copied row: every step (velocity, locks, condition, slide), its length and its clock */
	struct RowCopy {
		float velocity[STEPS] = {};
		wa::StepData data[STEPS];
		int length = STEPS;
		float clockMult = 3.f;
	};
	RowCopy copyRow(int r) {
		RowCopy c;
		for (int s = 0; s < STEPS; s++) {
			c.velocity[s] = velocity[r][s];
			c.data[s] = stepData[r][s];
		}
		c.length = length[r];
		c.clockMult = params[CLOCK_MULT_PARAM + r].getValue();
		return c;
	}
	void pasteRow(int r, const RowCopy& c) {
		for (int s = 0; s < STEPS; s++) {
			velocity[r][s] = c.velocity[s];
			stepData[r][s] = c.data[s];
		}
		length[r] = c.length;
		params[CLOCK_MULT_PARAM + r].setValue(c.clockMult);
	}
	/** Switches a step off, with its locks, condition and slide. The step after it is no longer tied to it. */
	void deleteStep(int r, int s) {
		velocity[r][s] = 0.f;
		stepData[r][s].clear();
		if (s + 1 < STEPS)
			stepData[r][s + 1].tie = false;
	}
	/** The step is lit and tied to the lit step before it (a long gate). Groups do not wrap round the row's end. */
	bool isTied(int r, int s) const {
		return s > 0 && s < STEPS && velocity[r][s] > 0.f && velocity[r][s - 1] > 0.f && stepData[r][s].tie;
	}
	/** Alt + click: ties a lit step to the lit step before it, or unties it. False when it cannot be tied. */
	bool toggleTie(int r, int s) {
		if (s <= 0 || s >= STEPS || velocity[r][s] <= 0.f || velocity[r][s - 1] <= 0.f)
			return false;
		stepData[r][s].tie = !stepData[r][s].tie;
		return true;
	}
	/** Moves a row's trigs one step right (dir 1) or left (-1), with their velocity, locks, condition and slide,
	wrapping around inside the row's length. A selected step of that row moves with its trig. */
	void shiftRow(int r, int dir) {
		int len = std::min(std::max(length[r], 1), STEPS);
		if (len < 2 || r < 0 || r >= ROWS)
			return;
		dir = (dir > 0) ? 1 : -1;
		float v[STEPS];
		wa::StepData d[STEPS];
		for (int s = 0; s < len; s++) {
			int to = (s + dir + len) % len;
			v[to] = velocity[r][s];
			d[to] = stepData[r][s];
		}
		for (int s = 0; s < len; s++) {
			velocity[r][s] = v[s];
			stepData[r][s] = d[s];
		}
		if (selRow == r && selStep >= 0 && selStep < len)
			selStep = (selStep + dir + len) % len;
	}
	/** RND: random locks on the lit steps of the lane's row (allRows: every row), RND AMOUNT deciding how many settings
	each step locks. False when the main module is not linked. UI thread. */
	bool randomizeLocks(bool allRows);
	/** COND CLR: every condition of the lane's row (allRows: every row) back to Always. UI thread. */
	void clearConditions(bool allRows);
	void clearRow(int r);
	void clearAll();
	/** A starting pattern: a kick on 1 5 9 13, a snare on 5 13, a hat on every other step */
	void loadDefault();

private:
	uint32_t seenReset = 0, fires[ROWS] = {};
	/** The step number each row's clock is at (steps since reset), to see when it moves on */
	long lastIndex[ROWS] = {};
	float lastVelocity[ROWS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	/** What the step that played last carries, sent with its fire (for a tied step: the group's locks with its own) */
	wa::StepData lastStep[ROWS];
	/** The row's last step played and was tied (its note goes on); the row's next step is tied to it (keep the
	note on); this tick played a step (a tied step after it continues the group) */
	bool lastLegato[ROWS] = {}, holdOpen[ROWS] = {}, groupPlaying[ROWS] = {};
	/** Each row's GATE output is high */
	bool gateHigh[ROWS] = {};
	/** Each row's MOD output (0..1, its steps' velocities) and the beat position seen last, to move its glide on */
	wa::GlideValue mod[ROWS];
	double lastBeatPos = 0.0;
	int position[ROWS] = {-1, -1, -1, -1, -1};
	/** Passes of each row since RESET (0 = the first), for A:B conditions */
	long pass[ROWS] = {-1, -1, -1, -1, -1};
	uint32_t rng = 1;
	static const int WARM_UP_SAMPLES = 4;
	int warmUp = WARM_UP_SAMPLES;
	float random01();
	void tickRow(int r);
	void restart();
};

bool isWaveArtifactSequencer(Module* m);

/** The condition choices of lock k on step s of row r (Always, a chance, A:B): when the trig plays and the condition
fails, that setting plays the knob. For the lane's and the main module's right-click menus. UI thread. */
void appendLockConditionItems(ui::Menu* menu, WaveArtifactSequencer* m, int r, int s, int k);
