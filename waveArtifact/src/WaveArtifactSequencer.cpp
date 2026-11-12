#include "WaveArtifactSequencer.hpp"
#include "WaveArtifact.hpp"
#include "Layout.hpp"
#include "widgets/PanelLabels.hpp"
#include "widgets/LfoWidgets.hpp"
#include "widgets/MiniSlider.hpp"

bool isWaveArtifactSequencer(Module* m) {
	return m && m->model == modelWaveArtifactSequencer;
}

static bool isMainModule(Module* m) {
	return m && m->model == modelWaveArtifact;
}

/** The row clock knob's value as text: x4, /2 ... */
struct SequencerClockQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		return WaveArtifactSequencer::multiplierText((int) std::round(getValue()));
	}
	std::string getUnit() override {
		return "";
	}
};

WaveArtifactSequencer::WaveArtifactSequencer() {
	config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
	for (int r = 0; r < ROWS; r++) {
		// Default x4: 16th-note steps. The knob's middle is x1 (a step per beat).
		configParam<SequencerClockQuantity>(CLOCK_MULT_PARAM + r, -4.f, 4.f, 3.f, "Marker " + std::to_string(r + 1) + " row clock");
		paramQuantities[CLOCK_MULT_PARAM + r]->snapEnabled = true;
		paramQuantities[CLOCK_MULT_PARAM + r]->randomizeEnabled = false;
	}
	// FILL's old slot (FILL was removed): no control on the panel
	configParam(UNUSED_FILL_PARAM, 0.f, 1.f, 0.f, "Unused");
	paramQuantities[UNUSED_FILL_PARAM]->randomizeEnabled = false;
	configSwitch(REC_PARAM, 0.f, 1.f, 0.f, "Record (live: knob moves go into the steps as they play)", {"Off", "On"});
	paramQuantities[REC_PARAM]->randomizeEnabled = false;
	configLight(REC_LIGHT, "Record");
	for (int r = 0; r < ROWS; r++)
		configOutput(GATE_OUTPUT + r, "Marker " + std::to_string(r + 1) + " row gate (half a step per trig; tied trigs: one long gate)");
	for (int r = 0; r < ROWS; r++)
		configOutput(MOD_OUTPUT + r, "Marker " + std::to_string(r + 1) + " row velocity CV (0-10 V at full level, held; glides on slide steps)");
	for (int r = 0; r < ROWS; r++) {
		// Full by default, so a patch from before it sounds the same
		configParam(MOD_LEVEL_PARAM + r, 0.f, 1.f, 1.f, "Marker " + std::to_string(r + 1) + " row MOD level", "%", 0.f, 100.f);
		paramQuantities[MOD_LEVEL_PARAM + r]->randomizeEnabled = false;
	}
	configButton(RND_PARAM, "Random locks on the lit steps of the lane's row (Shift + click: every row)");
	paramQuantities[RND_PARAM]->randomizeEnabled = false;
	configParam(RND_AMOUNT_PARAM, 0.f, 1.f, 0.5f, "Random locks amount (share of the settings locked on each lit trig)", "%", 0.f, 100.f);
	paramQuantities[RND_AMOUNT_PARAM]->randomizeEnabled = false;
	// The old COND RND button and its amount: no control on the panel
	configParam(UNUSED_COND_RND_PARAM, 0.f, 1.f, 0.f, "Unused");
	paramQuantities[UNUSED_COND_RND_PARAM]->randomizeEnabled = false;
	configParam(UNUSED_COND_AMOUNT_PARAM, 0.f, 1.f, 0.5f, "Unused");
	paramQuantities[UNUSED_COND_AMOUNT_PARAM]->randomizeEnabled = false;
	configButton(COND_CLR_PARAM, "Reset the trig conditions of the lane's row to Always (Shift + click: every row)");
	paramQuantities[COND_CLR_PARAM]->randomizeEnabled = false;
	for (int r = 0; r < ROWS; r++) {
		playing[r] = -1;
		rowOn[r] = true;
	}
	// Per-instance seed: two sequencers must not roll the same chances.
	rng = random::u32() | 1u;
	// This module sits on the left of the main one: it talks through its right side.
	rightExpander.producerMessage = new wa::ToSequencerMessage;
	rightExpander.consumerMessage = new wa::ToSequencerMessage;
	loadDefault();
}

WaveArtifactSequencer::~WaveArtifactSequencer() {
	delete (wa::ToSequencerMessage*) rightExpander.producerMessage;
	delete (wa::ToSequencerMessage*) rightExpander.consumerMessage;
}

void WaveArtifactSequencer::clearRow(int r) {
	for (int s = 0; s < STEPS; s++)
		deleteStep(r, s);
}

void WaveArtifactSequencer::clearAll() {
	for (int r = 0; r < ROWS; r++)
		clearRow(r);
}

void WaveArtifactSequencer::loadDefault() {
	clearAll();
	for (int r = 0; r < ROWS; r++)
		length[r] = STEPS;
	for (int s : {0, 4, 8, 12})
		velocity[0][s] = 0.9f;
	for (int s : {4, 12})
		velocity[1][s] = 0.8f;
	for (int s = 2; s < STEPS; s += 4)
		velocity[2][s] = 0.6f;
	for (int s = 0; s < STEPS; s += 4)
		velocity[2][s] = 0.4f;
}

void WaveArtifactSequencer::onReset(const ResetEvent& e) {
	Module::onReset(e);
	loadDefault();
}

void WaveArtifactSequencer::restart() {
	for (int r = 0; r < ROWS; r++) {
		position[r] = -1;
		pass[r] = -1;
		groupPlaying[r] = false;
		holdOpen[r] = false;
		gateHigh[r] = false;
	}
}

float WaveArtifactSequencer::random01() {
	// xorshift32
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (float) (rng >> 8) / 16777216.f;
}

void WaveArtifactSequencer::tickRow(int r) {
	int len = std::min(std::max(length[r], 1), STEPS);
	position[r] = (position[r] + 1) % len;
	if (position[r] == 0)
		pass[r]++;
	int p = position[r];
	playing[r] = p;
	// The row's marker is off: the row keeps time (so it is in step when the marker comes back) and plays nothing
	if (!rowOn[r]) {
		groupPlaying[r] = false;
		gateHigh[r] = false;
		holdOpen[r] = false;
		return;
	}
	float v = velocity[r][p];
	bool played = false;
	if (v > 0.f) {
		const wa::StepData& s = stepData[r][p];
		if (isTied(r, p)) {
			// Part of a long gate: plays only when the group has been playing (its first trig's condition decides
			// for the whole group). It does not start a new note, and keeps the group's velocity and locks.
			if (groupPlaying[r]) {
				lastStep[r] = wa::mergeTied(lastStep[r], s);
				wa::applyLockConditions(lastStep[r], s, std::max(pass[r], 0L), [this]() { return random01(); });
				lastLegato[r] = true;
				fires[r]++;
				played = true;
			}
		}
		// A step that does not play also leaves the locks of the one before in place.
		else if (wa::conditionPasses(s, std::max(pass[r], 0L), random01())) {
			lastVelocity[r] = v;
			lastStep[r] = s;
			wa::applyLockConditions(lastStep[r], s, std::max(pass[r], 0L), [this]() { return random01(); });
			lastLegato[r] = false;
			fires[r]++;
			played = true;
		}
	}
	groupPlaying[r] = played;
	gateHigh[r] = played;
	// MOD: the step's own velocity (a tied step's too, so a long note can carry a moving CV), gliding with its slide.
	if (played)
		mod[r].set(v, stepData[r][p].slide, 1.0 / stepsPerBeat((int) std::round(params[CLOCK_MULT_PARAM + r].getValue())));
	// The clock is untouched: the steps go on as always, only the note is kept on until the next tied step comes.
	holdOpen[r] = played && p + 1 < len && isTied(r, p + 1);
}

void WaveArtifactSequencer::process(const ProcessArgs& args) {
	Module* main = rightExpander.module;
	bool isLinked = isMainModule(main);
	linked = isLinked;
	lights[REC_LIGHT].setBrightness(params[REC_PARAM].getValue() > 0.5f ? 1.f : 0.f);
	if (!isLinked) {
		warmUp = WARM_UP_SAMPLES;
		for (int r = 0; r < ROWS; r++) {
			gateHigh[r] = false;
			rowOn[r] = true;
			outputs[GATE_OUTPUT + r].setVoltage(0.f);
			outputs[MOD_OUTPUT + r].setVoltage(0.f);
		}
		return;
	}
	const wa::ToSequencerMessage* in = (const wa::ToSequencerMessage*) rightExpander.consumerMessage;
	// Rows whose marker is off: silent (their GATE closes, MOD goes to 0)
	for (int r = 0; r < ROWS; r++) {
		bool on = in->markerOn[r];
		if (!on && rowOn[r]) {
			gateHigh[r] = false;
			holdOpen[r] = false;
			groupPlaying[r] = false;
		}
		rowOn[r] = on;
	}
	// MOD glides move with the clock: the beats since the last sample (none across a jump back or a big leap).
	{
		double d = in->beatPos - lastBeatPos;
		lastBeatPos = in->beatPos;
		if (warmUp > 0 || d < 0.0 || d > 1.0)
			d = 0.0;
		for (int r = 0; r < ROWS; r++)
			mod[r].advance(d);
	}
	// Right after snapping on, the first messages are stale (from before it was unlinked): only follow them.
	// Each row's step number: the clock's beat position times the row's steps per beat. The tiny offset keeps a step
	// that lands exactly on a beat from slipping to the next sample.
	long index[ROWS];
	for (int r = 0; r < ROWS; r++)
		index[r] = (long) std::floor(in->beatPos * stepsPerBeat((int) std::round(params[CLOCK_MULT_PARAM + r].getValue())) + 1e-7);
	if (warmUp > 0) {
		warmUp--;
		seenReset = in->resetCount;
		for (int r = 0; r < ROWS; r++)
			lastIndex[r] = index[r];
	}
	if (in->resetCount != seenReset) {
		// RESET: back to step 1, played at once, on every row.
		seenReset = in->resetCount;
		restart();
		for (int r = 0; r < ROWS; r++) {
			lastIndex[r] = index[r];
			tickRow(r);
		}
	}
	else {
		for (int r = 0; r < ROWS; r++) {
			// Any change counts (a clock that jumps back, e.g. an external restart, still moves on one step).
			if (index[r] != lastIndex[r]) {
				lastIndex[r] = index[r];
				tickRow(r);
			}
		}
	}
	wa::FromSequencerMessage* out = (wa::FromSequencerMessage*) main->leftExpander.producerMessage;
	out->linked = true;
	for (int r = 0; r < ROWS; r++) {
		out->fireCount[r] = fires[r];
		out->velocity[r] = lastVelocity[r];
		std::memcpy(out->lockValue[r], lastStep[r].value, sizeof(out->lockValue[r]));
		out->lockMask[r] = lastStep[r].mask;
		out->slide[r] = lastStep[r].slide;
		out->stepBeats[r] = 1.0 / stepsPerBeat((int) std::round(params[CLOCK_MULT_PARAM + r].getValue()));
		out->legato[r] = lastLegato[r];
		// Live, so untying or deleting the next step while the note waits lets it go at that step.
		int next = position[r] + 1;
		out->holdOpen[r] = holdOpen[r] && next < STEPS && isTied(r, next);
		// GATE: high from the trig for half a step, or on through a tied group. Where the row is in its step comes
		// from the same clock as the steps, so the gates keep time exactly.
		if (gateHigh[r] && !out->holdOpen[r]) {
			double x = in->beatPos * stepsPerBeat((int) std::round(params[CLOCK_MULT_PARAM + r].getValue())) + 1e-7;
			if (x - std::floor(x) >= GATE_LENGTH)
				gateHigh[r] = false;
		}
		outputs[GATE_OUTPUT + r].setVoltage(gateHigh[r] ? 10.f : 0.f);
		outputs[MOD_OUTPUT + r].setVoltage(rowOn[r] ? 10.f * params[MOD_LEVEL_PARAM + r].getValue() * mod[r].value : 0.f);
	}
	main->leftExpander.requestMessageFlip();
}

json_t* WaveArtifactSequencer::dataToJson() {
	json_t* root = json_object();
	json_t* rows = json_array();
	for (int r = 0; r < ROWS; r++) {
		json_t* row = json_array();
		for (int s = 0; s < STEPS; s++)
			json_array_append_new(row, json_real(velocity[r][s]));
		json_array_append_new(rows, row);
	}
	json_object_set_new(root, "steps", rows);
	json_t* lens = json_array();
	for (int r = 0; r < ROWS; r++)
		json_array_append_new(lens, json_integer(length[r]));
	json_object_set_new(root, "length", lens);
	// Locks by setting name, so a patch keeps them whatever the order of the settings becomes. Plain steps are null.
	json_t* data = json_array();
	for (int r = 0; r < ROWS; r++) {
		json_t* row = json_array();
		for (int s = 0; s < STEPS; s++) {
			const wa::StepData& d = stepData[r][s];
			if (d.isPlain()) {
				json_array_append_new(row, json_null());
				continue;
			}
			json_t* step = json_object();
			if (d.mask) {
				json_t* locks = json_object();
				for (int k = 0; k < wa::NUM_LOCKABLE; k++)
					if (d.locked(k))
						json_object_set_new(locks, wa::LOCK_NAMES[k], json_real(d.value[k]));
				json_object_set_new(step, "locks", locks);
				if (d.mask & d.condMask) {
					json_t* conds = json_object();
					for (int k = 0; k < wa::NUM_LOCKABLE; k++) {
						if (!d.locked(k) || !((d.condMask >> k) & 1u))
							continue;
						const wa::Condition& c = d.lockCond[k];
						json_t* cj = json_object();
						json_object_set_new(cj, "cond", json_integer(c.cond));
						json_object_set_new(cj, "chance", json_integer(c.chance));
						json_object_set_new(cj, "a", json_integer(c.a));
						json_object_set_new(cj, "b", json_integer(c.b));
						json_object_set_new(conds, wa::LOCK_NAMES[k], cj);
					}
					json_object_set_new(step, "lockConds", conds);
				}
			}
			if (d.cond != wa::COND_ALWAYS) {
				json_object_set_new(step, "cond", json_integer(d.cond));
				json_object_set_new(step, "chance", json_integer(d.chance));
				json_object_set_new(step, "a", json_integer(d.a));
				json_object_set_new(step, "b", json_integer(d.b));
			}
			if (d.slide > 0.f)
				json_object_set_new(step, "slide", json_real(d.slide));
			if (d.tie)
				json_object_set_new(step, "tie", json_true());
			json_array_append_new(row, step);
		}
		json_array_append_new(data, row);
	}
	json_object_set_new(root, "stepData", data);
	json_object_set_new(root, "laneRow", json_integer(laneRow));
	json_object_set_new(root, "laneSetting", json_string(wa::LOCK_NAMES[clamp(laneSetting, 0, wa::NUM_LOCKABLE - 1)]));
	json_object_set_new(root, "laneCondition", json_boolean(laneCondition));
	return root;
}

static int lockIndexByName(const char* name) {
	for (int k = 0; k < wa::NUM_LOCKABLE; k++)
		if (name && std::strcmp(name, wa::LOCK_NAMES[k]) == 0)
			return k;
	return -1;
}

void WaveArtifactSequencer::dataFromJson(json_t* root) {
	json_t* rows = json_object_get(root, "steps");
	if (rows) {
		for (int r = 0; r < ROWS; r++) {
			json_t* row = json_array_get(rows, r);
			for (int s = 0; s < STEPS; s++) {
				json_t* v = row ? json_array_get(row, s) : NULL;
				velocity[r][s] = v ? clamp((float) json_number_value(v), 0.f, 1.f) : 0.f;
			}
		}
	}
	json_t* lens = json_object_get(root, "length");
	if (lens) {
		for (int r = 0; r < ROWS; r++) {
			json_t* l = json_array_get(lens, r);
			length[r] = l ? clamp((int) json_integer_value(l), 1, STEPS) : STEPS;
		}
	}
	// Older patches have no stepData: every step plain.
	json_t* data = json_object_get(root, "stepData");
	for (int r = 0; r < ROWS; r++) {
		json_t* row = data ? json_array_get(data, r) : NULL;
		for (int s = 0; s < STEPS; s++) {
			wa::StepData& d = stepData[r][s];
			d.clear();
			json_t* step = row ? json_array_get(row, s) : NULL;
			if (!step || !json_is_object(step))
				continue;
			json_t* locks = json_object_get(step, "locks");
			const char* key;
			json_t* value;
			if (locks) {
				json_object_foreach(locks, key, value) {
					int k = lockIndexByName(key);
					if (k >= 0 && json_is_number(value))
						d.setLock(k, (float) json_number_value(value));
				}
			}
			json_t* conds = json_object_get(step, "lockConds");
			if (conds) {
				json_object_foreach(conds, key, value) {
					int k = lockIndexByName(key);
					if (k < 0 || !json_is_object(value))
						continue;
					wa::Condition c;
					int cv = (int) json_integer_value(json_object_get(value, "cond"));
					c.cond = (cv >= 0 && cv < wa::NUM_CONDITIONS) ? (uint8_t) cv : (uint8_t) wa::COND_ALWAYS;
					c.chance = (uint8_t) clamp((int) json_integer_value(json_object_get(value, "chance")), 0, 100);
					c.b = (uint8_t) clamp((int) json_integer_value(json_object_get(value, "b")), 1, 8);
					c.a = (uint8_t) clamp((int) json_integer_value(json_object_get(value, "a")), 1, (int) c.b);
					d.setLockCondition(k, c);
				}
			}
			json_t* cond = json_object_get(step, "cond");
			if (cond) {
				// FILL and NOT FILL (3 and 4, removed) and anything unknown play Always.
				int c = (int) json_integer_value(cond);
				d.cond = (c >= 0 && c < wa::NUM_CONDITIONS) ? (uint8_t) c : (uint8_t) wa::COND_ALWAYS;
				json_t* j;
				if ((j = json_object_get(step, "chance")))
					d.chance = (uint8_t) clamp((int) json_integer_value(j), 0, 100);
				if ((j = json_object_get(step, "b")))
					d.b = (uint8_t) clamp((int) json_integer_value(j), 1, 8);
				if ((j = json_object_get(step, "a")))
					d.a = (uint8_t) clamp((int) json_integer_value(j), 1, (int) d.b);
			}
			// Older patches saved slide as on / off: on is a full slide.
			json_t* slide = json_object_get(step, "slide");
			d.slide = json_is_true(slide) ? 1.f : (json_is_number(slide) ? clamp((float) json_number_value(slide), 0.f, 1.f) : 0.f);
			d.tie = json_is_true(json_object_get(step, "tie"));
		}
	}
	json_t* lr = json_object_get(root, "laneRow");
	if (lr)
		laneRow = clamp((int) json_integer_value(lr), 0, ROWS - 1);
	json_t* ls = json_object_get(root, "laneSetting");
	if (ls) {
		int k = lockIndexByName(json_string_value(ls));
		if (wa::isLockable(k))
			laneSetting = k;
	}
	laneCondition = json_is_true(json_object_get(root, "laneCondition"));
}

// Panel: 32 HP. Geometry matches res/waveArtifactSequencer.svg.
namespace seqlayout {
static const float W = 162.56f;
/** Left of each row, packed tight: the marker badge with LEN under it, the row clock knob, the shift arrows */
static const float BADGE_X = 8.5f;
static const float CLOCK_KNOB_X = 15.6f;
static const float GRID_X0 = 23.8f;
static const float GRID_X1 = 139.8f;
/** GATE output of each row, right of the grid, in an output box (both SVGs) */
static const float GATE_X = 144.4f;
/** MOD output of each row (its steps' velocities as CV), right of GATE in the same box */
static const float MOD_X = 154.2f;
/** Each row's MOD level trimpot, under its MOD jack (in the gap before the next row's jack) */
static const float MOD_LEVEL_DY = 8.4f;
static const float GATE_LABEL_Y = 14.5f;
static const float ROW_Y0 = 19.f;
/** A row's cells; under them, in the gap, the SLIDE strips of the lit trigs (same row pitch as before: 16.6 mm) */
static const float ROW_H = 12.6f;
static const float ROW_GAP = 4.0f;
static const float CELL_GAP = 0.8f;
/** The lock lane under the grid (below the step numbers), and its name on the left */
static const float LANE_Y0 = 104.6f;
static const float LANE_H = 9.4f;
static const float LANE_LABEL_X = 15.7f;
/** CLR, left of the lane's name: clears the lane's setting in its row */
static const float CLR_X0 = 2.4f, CLR_W = 5.2f;
static const float CLR_Y0 = LANE_Y0 + 2.2f, CLR_H = 5.0f;
/** REC (live record) in the top-left corner, under the screw, with its label to the right */
static const Vec REC_BUTTON = Vec(14.f, 9.6f);
static const Vec REC_LABEL = Vec(22.f, 10.6f);
/** RND (random locks) and its amount slider in the top-right corner, level with REC, left of the screw */
static const Vec RND_LABEL = Vec(112.f, 10.6f);
static const Vec RND_BUTTON = Vec(121.5f, 9.6f);
static const Vec RND_BUTTON_SIZE = Vec(8.f, 4.f);
static const Vec RND_SLIDER = Vec(138.f, 9.6f);
static const Vec RND_SLIDER_SIZE = Vec(21.f, 3.6f);
/** COND CLR (every trig condition of the lane's row back to Always), in the top bar right of REC */
static const Vec COND_LABEL = Vec(30.5f, 10.6f);
static const Vec COND_CLR_BUTTON = Vec(38.f, 9.6f);
static const Vec COND_CLR_SIZE = Vec(7.f, 4.f);
static const float TITLE_X = 84.f;
/** Accent of the COND controls (warm, apart from the locks' blue) */
static const NVGcolor COND_COLOR = nvgRGB(0xff, 0xc4, 0x6b);
/** Shift arrows of each row, stacked between the CLOCK knob and the first step: ◀ on top, ▶ below */
static const float SHIFT_X0 = 19.2f, SHIFT_W = 4.0f, SHIFT_H = 5.0f;
static const float SHIFT_LEFT_DY = 1.0f, SHIFT_RIGHT_DY = 6.8f;
/** SLIDE strip under each lit trig, apart from its cell (SLIDE_DY below it): drag right for more slide (left end =
off). The cell above stays all velocity. */
static const float SLIDE_DY = 0.5f, SLIDE_H = 2.0f;
inline float slideTop(int r);
inline float cellW() {
	return (GRID_X1 - GRID_X0) / WaveArtifactSequencer::STEPS;
}
inline float rowTop(int r) {
	return ROW_Y0 + r * (ROW_H + ROW_GAP);
}
inline float slideTop(int r) {
	return rowTop(r) + ROW_H + SLIDE_DY;
}
inline float cellX(int s) {
	return GRID_X0 + s * cellW();
}
} // namespace seqlayout

static WaveArtifact* mainOf(WaveArtifactSequencer* m) {
	Module* right = m ? m->rightExpander.module : nullptr;
	return isMainModule(right) ? (WaveArtifact*) right : nullptr;
}

/** Selects a step and shows its marker on the main module (its knobs then show the step's values) */
static void selectStep(WaveArtifactSequencer* m, int r, int s) {
	m->select(r, s);
	WaveArtifact* main = mainOf(m);
	if (main && r >= 0 && main->isMarkerActive(r))
		main->selectedMarker = r;
}

/** RND: new random locks on every lit step of the lane's row (allRows: of every row). Each lockable setting that the
main module's own Randomize would change gets locked with probability RND AMOUNT, to a random value in that knob's
range (whole values for switches, lists and stepped knobs). The step's other locks go, so 0 % leaves no locks and
100 % locks every one. Conditions and slide stay. UI thread (the RND button adds the undo entry). */
bool WaveArtifactSequencer::randomizeLocks(bool allRows) {
	WaveArtifactSequencer* m = this;
	WaveArtifact* main = mainOf(m);
	if (!main)
		return false;
	float amount = clamp(m->params[WaveArtifactSequencer::RND_AMOUNT_PARAM].getValue(), 0.f, 1.f);
	for (int r = 0; r < WaveArtifactSequencer::ROWS; r++) {
		if (!allRows && r != m->laneRow)
			continue;
		for (int s = 0; s < WaveArtifactSequencer::STEPS; s++) {
			if (m->velocity[r][s] <= 0.f)
				continue;
			wa::StepData& d = m->stepData[r][s];
			d.clearLocks();
			for (int k = 0; k < wa::NUM_LOCKABLE; k++) {
				ParamQuantity* q = main->getParamQuantity(WaveArtifact::MARKER_SETTING_PARAMS[k] + r);
				if (!wa::isLockable(k) || !q || !q->randomizeEnabled)
					continue;
				// amount 1: uniform() < 1 always locks; amount 0: never.
				if (random::uniform() >= amount)
					continue;
				float lo = q->getMinValue(), hi = q->getMaxValue();
				float v = lo + (hi - lo) * random::uniform();
				if (q->snapEnabled)
					v = std::round(v);
				d.setLock(k, clamp(v, lo, hi));
			}
		}
	}
	return true;
}

void WaveArtifactSequencer::clearConditions(bool allRows) {
	laneCondition = true;
	for (int r = 0; r < ROWS; r++)
		for (int s = 0; s < STEPS; s++)
			if (allRows || r == laneRow)
				stepData[r][s].cond = wa::COND_ALWAYS;
}

/** A panel action with an undo entry (the whole module before and after). `f` returns false when it did nothing. */
static void undoable(WaveArtifactSequencer* m, const std::string& name, std::function<bool()> f) {
	json_t* before = m->toJson();
	if (!f()) {
		json_decref(before);
		return;
	}
	history::ModuleChange* h = new history::ModuleChange;
	h->name = name;
	h->moduleId = m->id;
	h->oldModuleJ = before;
	h->newModuleJ = m->toJson();
	APP->history->push(h);
}

/** What Ctrl + C copied last (a trig or a row), shared by every sequencer so it can go from one to another */
struct SeqClipboard {
	enum Kind { NONE, TRIG, ROW } kind = NONE;
	float velocity = 0.f;
	wa::StepData data;
	WaveArtifactSequencer::RowCopy row;
};
static SeqClipboard& seqClipboard() {
	static SeqClipboard clip;
	return clip;
}

static void copyRowToClipboard(WaveArtifactSequencer* m, int r) {
	SeqClipboard& clip = seqClipboard();
	clip.kind = SeqClipboard::ROW;
	clip.row = m->copyRow(r);
}

static bool pasteRowFromClipboard(WaveArtifactSequencer* m, int r) {
	SeqClipboard& clip = seqClipboard();
	if (clip.kind != SeqClipboard::ROW)
		return false;
	m->pasteRow(r, clip.row);
	m->select(-1, -1);
	m->rowSel = r;
	m->laneRow = r;
	return true;
}

static void appendRowMenu(ui::Menu* menu, WaveArtifactSequencer* m, int r) {
	menu->addChild(createMenuItem("Copy row", RACK_MOD_CTRL_NAME "+A, " RACK_MOD_CTRL_NAME "+C", [=]() { copyRowToClipboard(m, r); }));
	menu->addChild(createMenuItem("Paste row here", RACK_MOD_CTRL_NAME "+V", [=]() { pasteRowFromClipboard(m, r); },
		seqClipboard().kind != SeqClipboard::ROW));
	menu->addChild(new MenuSeparator);
	menu->addChild(createSubmenuItem("Length", string::f("%d steps", m->length[r]), [=](ui::Menu* sub) {
		for (int n = 1; n <= WaveArtifactSequencer::STEPS; n++)
			sub->addChild(createCheckMenuItem(string::f("%d", n), "", [=]() { return m->length[r] == n; }, [=]() { m->length[r] = n; }));
	}));
	menu->addChild(createMenuItem("Every 4th step", "", [=]() {
		m->clearRow(r);
		for (int s = 0; s < WaveArtifactSequencer::STEPS; s += 4)
			m->velocity[r][s] = 0.8f;
	}));
	menu->addChild(createMenuItem("Every 2nd step", "", [=]() {
		m->clearRow(r);
		for (int s = 0; s < WaveArtifactSequencer::STEPS; s += 2)
			m->velocity[r][s] = 0.8f;
	}));
	menu->addChild(createMenuItem("Random", "", [=]() {
		for (int s = 0; s < WaveArtifactSequencer::STEPS; s++) {
			bool on = random::uniform() < 0.35f;
			m->velocity[r][s] = on ? 0.4f + 0.6f * random::uniform() : 0.f;
			if (!on)
				m->stepData[r][s].clear();
		}
	}));
	menu->addChild(createMenuItem("Clear locks in this row", "", [=]() {
		for (int s = 0; s < WaveArtifactSequencer::STEPS; s++)
			m->stepData[r][s].clearLocks();
	}));
	menu->addChild(createMenuItem("Clear row", "", [=]() { m->clearRow(r); }));
	menu->addChild(new MenuSeparator);
	menu->addChild(createMenuItem("Clear all", "", [=]() { m->clearAll(); }));
	menu->addChild(createMenuItem("Default pattern", "", [=]() { m->loadDefault(); }));
}

/** Condition choices: Always, a chance, or A:B, read and written through get / set (a trig's condition, or a lock's) */
static void appendConditionItems(ui::Menu* sub, std::function<wa::Condition()> get, std::function<void(wa::Condition)> set) {
	sub->addChild(createCheckMenuItem("Always", "", [=]() { return get().cond == wa::COND_ALWAYS; }, [=]() { set(wa::Condition()); }));
	sub->addChild(new MenuSeparator);
	for (int pct : wa::CHANCES)
		sub->addChild(createCheckMenuItem(string::f("%d%% chance", pct), "",
			[=]() { wa::Condition c = get(); return c.cond == wa::COND_CHANCE && c.chance == pct; },
			[=]() { wa::Condition c; c.cond = wa::COND_CHANCE; c.chance = (uint8_t) pct; set(c); }));
	sub->addChild(new MenuSeparator);
	sub->addChild(createMenuLabel("A:B - pass A of every B passes of the row"));
	for (int b = 2; b <= 8; b++) {
		sub->addChild(createSubmenuItem(string::f("every %d passes", b), "", [=](ui::Menu* sub2) {
			for (int a = 1; a <= b; a++)
				sub2->addChild(createCheckMenuItem(string::f("%d:%d", a, b), "",
					[=]() { wa::Condition c = get(); return c.cond == wa::COND_RATIO && c.a == a && c.b == b; },
					[=]() { wa::Condition c; c.cond = wa::COND_RATIO; c.a = (uint8_t) a; c.b = (uint8_t) b; set(c); }));
		}));
	}
}

/** A trig's condition: whether it plays (the trig's menu and the lane's CONDITION view) */
static void appendTrigConditionItems(ui::Menu* sub, WaveArtifactSequencer* m, int r, int s) {
	wa::StepData* d = &m->stepData[r][s];
	appendConditionItems(sub, [=]() { return d->trigCondition(); }, [=](wa::Condition c) { d->setTrigCondition(c); });
}

void appendLockConditionItems(ui::Menu* sub, WaveArtifactSequencer* m, int r, int s, int k) {
	wa::StepData* d = &m->stepData[r][s];
	sub->addChild(createMenuLabel("When it fails, the trig plays the knob for this setting"));
	appendConditionItems(sub, [=]() { return d->lockCondition(k); }, [=](wa::Condition c) { d->setLockCondition(k, c); });
}

/** The step's own menu: its condition, slide, locks; the row's items in a submenu */
static void openStepMenu(WaveArtifactSequencer* m, int r, int s) {
	ui::Menu* menu = createMenu();
	menu->addChild(createMenuLabel(string::f("Marker %d, step %d", r + 1, s + 1)));
	wa::StepData* d = &m->stepData[r][s];
	char text[16];
	wa::conditionText(*d, text, sizeof(text));
	menu->addChild(createSubmenuItem("Condition (also in the lane: CONDITION)", d->cond == wa::COND_ALWAYS ? "always" : text, [=](ui::Menu* sub) {
		appendTrigConditionItems(sub, m, r, s);
	}));
	menu->addChild(createSubmenuItem("Slide (glide to this step's values)", d->slide > 0.f ? string::f("%d%%", (int) std::round(d->slide * 100.f)) : "off",
		[=](ui::Menu* sub) {
			sub->addChild(createMenuLabel("Share of one step the glide takes (also: drag the strip under the trig)"));
			for (int pct : {0, 10, 25, 50, 75, 100})
				sub->addChild(createCheckMenuItem(pct == 0 ? "Off" : string::f("%d%%", pct), "",
					[=]() { return (int) std::round(d->slide * 100.f) == pct; }, [=]() { d->slide = pct / 100.f; }));
		}));
	menu->addChild(createCheckMenuItem("Tie to the trig before (long gate)", "", [=]() { return m->isTied(r, s); },
		[=]() { m->toggleTie(r, s); }, s == 0 || m->velocity[r][s] <= 0.f || m->velocity[r][s - 1] <= 0.f));
	int locks = 0;
	for (int k = 0; k < wa::NUM_LOCKABLE; k++)
		locks += d->locked(k) ? 1 : 0;
	menu->addChild(createMenuItem("Clear locks on this step", string::f("%d", locks), [=]() { d->clearLocks(); }));
	menu->addChild(createMenuItem("Delete step", "", [=]() { m->deleteStep(r, s); }));
	menu->addChild(new MenuSeparator);
	menu->addChild(createSubmenuItem(string::f("Row %d", r + 1), "", [=](ui::Menu* sub) { appendRowMenu(sub, m, r); }));
}

/** The step grid: draw steps by dragging. Press on an empty step and drag along the row: every step you pass is
switched on, at the height of the mouse (the top of a cell is 100%). Drag up and down to set a velocity. A click on a
step selects it (its values show under the main module's knobs, and turning that marker's knobs locks them on it); a
double-click on a step switches it off. Shift + drag along a row switches the steps you pass off. Ctrl + click sets
the row's length to end at that step. Right-click a step for its condition, slide and locks; click outside the
steps to deselect. The strip under a lit trig (apart from its cell) is its SLIDE: drag right for more (left end = off,
double-click = off).
Between two lit trigs, a + appears in the middle of the row on hover: click it to tie them (one note through both, a
long gate, the clock unchanged); on a tied pair it shows − to untie. */
struct StepGrid : widget::OpaqueWidget {
	WaveArtifactSequencer* module = nullptr;
	int dragRow = -1, dragCol = -1;
	/** The last pressed step, for a double-click, and the step a press switched on (with when), so the double-click
	that follows a creating click does not switch it off again */
	int pressRow = -1, pressCol = -1, createdRow = -1, createdCol = -1;
	double createdAt = -10.0;
	float pressY = 0.f;
	bool wasOff = false, moved = false, erasing = false;
	/** A drag on a lit trig's SLIDE strip (that step), and whether the last press was on a strip (a double-click
	there switches the slide off instead of deleting the step) */
	int slideRow = -1, slideCol = -1;
	bool pressOnSlide = false;
	/** Tests: use this pointer position (in panel pixels) instead of the real mouse */
	bool mouseOverride = false;
	Vec mouseAt;

	bool cellAt(Vec p, int* row, int* col) {
		float x = p.x / SVG_DPI * MM_PER_IN - 0.f;
		float y = p.y / SVG_DPI * MM_PER_IN;
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++) {
			float top = seqlayout::rowTop(r);
			if (y < top || y > top + seqlayout::ROW_H)
				continue;
			int c = (int) std::floor((x - seqlayout::GRID_X0) / seqlayout::cellW());
			if (c < 0 || c >= WaveArtifactSequencer::STEPS)
				return false;
			*row = r;
			*col = c;
			return true;
		}
		return false;
	}

	/** Velocity for a mouse height inside a row: the top of the cell is 100% */
	/** Velocity for a mouse height inside a row: the top of the cell is 100%. The bottom of the cell (below 6%) is 0:
	the step is off, so a velocity dragged all the way down is silent, not a quiet 5%. */
	static constexpr float OFF_BELOW = 0.06f, MIN_ON = 0.05f;
	float velocityAt(int row, float yPx) {
		float y = yPx / SVG_DPI * MM_PER_IN;
		float v = 1.f - (y - seqlayout::rowTop(row)) / seqlayout::ROW_H;
		return (v < OFF_BELOW) ? 0.f : clamp(v, MIN_ON, 1.f);
	}

	/** TIE handle: a small round button on the line between two steps, in the middle of the row's height. It shows
	while the mouse is near that line (HOVER_MM either side) and both steps are lit. */
	static constexpr float TIE_DY = 6.3f, TIE_R = 1.8f, TIE_HOVER_MM = 2.6f; // TIE_DY: ROW_H / 2
	/** The row and the gap (s = between steps s - 1 and s) the mouse is near, and whether it is on the handle */
	int hoverRow = -1, hoverGap = -1;
	bool hoverOnHandle = false;

	bool tieHandleUsable(int r, int s) {
		return module && r >= 0 && r < WaveArtifactSequencer::ROWS && s >= 1 && s < WaveArtifactSequencer::STEPS &&
			module->velocity[r][s] > 0.f && module->velocity[r][s - 1] > 0.f;
	}
	/** The gap nearest to a point (panel pixels) in a row, within `reach` mm of it, and whether the point is on its
	handle */
	bool gapNear(Vec p, float reach, int* row, int* gap, bool* onHandle) {
		float x = p.x / SVG_DPI * MM_PER_IN, y = p.y / SVG_DPI * MM_PER_IN;
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++) {
			float top = seqlayout::rowTop(r);
			if (y < top || y > top + seqlayout::ROW_H)
				continue;
			int s = (int) std::round((x - seqlayout::GRID_X0) / seqlayout::cellW());
			float gx = seqlayout::GRID_X0 + s * seqlayout::cellW();
			if (std::fabs(x - gx) > reach || !tieHandleUsable(r, s))
				return false;
			*row = r;
			*gap = s;
			float dx = x - gx, dy = y - (top + TIE_DY);
			*onHandle = dx * dx + dy * dy <= (TIE_R + 0.4f) * (TIE_R + 0.4f);
			return true;
		}
		return false;
	}
	/** The press (panel pixels) is on a TIE handle: the row and the step it ties (to the one before) */
	bool tieHandleAt(Vec p, int* row, int* s) {
		bool on = false;
		return gapNear(p, TIE_R + 0.4f, row, s, &on) && on;
	}
	void onHover(const HoverEvent& e) override {
		if (!gapNear(e.pos, TIE_HOVER_MM, &hoverRow, &hoverGap, &hoverOnHandle)) {
			hoverRow = hoverGap = -1;
			hoverOnHandle = false;
		}
		OpaqueWidget::onHover(e);
	}
	void onLeave(const LeaveEvent& e) override {
		hoverRow = hoverGap = -1;
		hoverOnHandle = false;
		OpaqueWidget::onLeave(e);
	}

	/** The press (panel pixels) is on the SLIDE strip under a lit trig: which one. The whole gap under the row counts
	(the strip plus a little margin), so the thin strip is easy to catch. */
	bool slideStripAt(Vec p, int* row, int* col) {
		if (!module)
			return false;
		float x = p.x / SVG_DPI * MM_PER_IN, y = p.y / SVG_DPI * MM_PER_IN;
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++) {
			float top = seqlayout::rowTop(r) + seqlayout::ROW_H;
			if (y <= top || y > seqlayout::slideTop(r) + seqlayout::SLIDE_H + 0.7f)
				continue;
			int c = (int) std::floor((x - seqlayout::GRID_X0) / seqlayout::cellW());
			if (c < 0 || c >= WaveArtifactSequencer::STEPS || module->velocity[r][c] <= 0.f)
				return false;
			*row = r;
			*col = c;
			return true;
		}
		return false;
	}
	/** SLIDE for a mouse x (panel pixels) over step c: the strip's left end is off, its right end 100%. A few
	percent of dead zone at the left, so off is easy to reach. */
	float slideAt(int c, float xPx) {
		float x = xPx / SVG_DPI * MM_PER_IN;
		float x0 = seqlayout::cellX(c) + seqlayout::CELL_GAP / 2.f + 0.4f, w = seqlayout::cellW() - seqlayout::CELL_GAP - 0.8f;
		float f = (x - x0) / w;
		const float DEAD = 0.04f;
		if (f <= DEAD)
			return 0.f;
		return clamp((f - DEAD) / (1.f - DEAD) * 0.99f + 0.01f, 0.01f, 1.f);
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS)
			return;
		// Any click ends a whole-row selection (Ctrl + A)
		module->rowSel = -1;
		pressOnSlide = false;
		int r = -1, c = -1;
		bool onCell = cellAt(e.pos, &r, &c);
		// A row's shift arrows: its trigs (and everything on them) one step left or right
		int arrowRow, arrowDir;
		if (e.button == GLFW_MOUSE_BUTTON_LEFT && arrowAt(e.pos, &arrowRow, &arrowDir)) {
			module->shiftRow(arrowRow, arrowDir);
			pressRow = pressCol = -1;
			e.consume(this);
			return;
		}
		// The TIE handle between two lit trigs (shown on hover): ties the right one to the left one, or unties it.
		int tr, ts;
		if (e.button == GLFW_MOUSE_BUTTON_LEFT && tieHandleAt(e.pos, &tr, &ts)) {
			module->toggleTie(tr, ts);
			selectStep(module, tr, ts);
			pressRow = pressCol = -1;
			e.consume(this);
			return;
		}
		// The SLIDE strip under a lit trig: set its slide where the mouse is, and drag sideways for more or less.
		// Right-click there opens the step's menu.
		int sr, sc;
		if (slideStripAt(e.pos, &sr, &sc)) {
			if (e.button == GLFW_MOUSE_BUTTON_LEFT && (e.mods & RACK_MOD_MASK) == 0) {
				pressRow = sr;
				pressCol = sc;
				slideRow = sr;
				slideCol = sc;
				pressOnSlide = true;
				module->stepData[sr][sc].slide = slideAt(sc, e.pos.x);
				selectStep(module, sr, sc);
				e.consume(this);
				return;
			}
			if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
				selectStep(module, sr, sc);
				openStepMenu(module, sr, sc);
				e.consume(this);
				return;
			}
		}
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			if (onCell) {
				selectStep(module, r, c);
				openStepMenu(module, r, c);
				e.consume(this);
				return;
			}
			float y = e.pos.y / SVG_DPI * MM_PER_IN;
			float x = e.pos.x / SVG_DPI * MM_PER_IN;
			for (r = 0; r < WaveArtifactSequencer::ROWS; r++)
				if (y >= seqlayout::rowTop(r) && y <= seqlayout::rowTop(r) + seqlayout::ROW_H && x < seqlayout::GRID_X1)
					break;
			if (r < WaveArtifactSequencer::ROWS) {
				ui::Menu* menu = createMenu();
				menu->addChild(createMenuLabel("Marker " + std::to_string(r + 1) + " row"));
				appendRowMenu(menu, module, r);
				e.consume(this);
			}
			return;
		}
		if (e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		if (!onCell) {
			// A click outside the steps: no step selected (the main module's knobs are its own again).
			module->select(-1, -1);
			pressRow = pressCol = -1;
			e.consume(this);
			return;
		}
		pressRow = r;
		pressCol = c;
		if (e.mods & RACK_MOD_CTRL) {
			module->length[r] = c + 1;
			e.consume(this);
			return;
		}
		dragRow = r;
		dragCol = c;
		pressY = e.pos.y;
		moved = false;
		erasing = (e.mods & RACK_MOD_MASK) == GLFW_MOD_SHIFT;
		wasOff = module->velocity[r][c] <= 0.f;
		if (erasing) {
			module->deleteStep(r, c);
		}
		else {
			if (wasOff) {
				// A click always makes a step, even at the very bottom of the cell (dragging down switches it off).
				module->velocity[r][c] = std::max(velocityAt(r, e.pos.y), MIN_ON);
				createdRow = r;
				createdCol = c;
				createdAt = system::getTime();
			}
			selectStep(module, r, c);
		}
		e.consume(this);
	}

	/** The press (panel pixels) is on a row's ◀ (dir -1) or ▶ (dir 1) */
	bool arrowAt(Vec p, int* row, int* dir) {
		float x = p.x / SVG_DPI * MM_PER_IN, y = p.y / SVG_DPI * MM_PER_IN;
		if (x < seqlayout::SHIFT_X0 || x > seqlayout::SHIFT_X0 + seqlayout::SHIFT_W)
			return false;
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++) {
			float top = seqlayout::rowTop(r);
			if (y >= top + seqlayout::SHIFT_LEFT_DY && y <= top + seqlayout::SHIFT_LEFT_DY + seqlayout::SHIFT_H) {
				*row = r;
				*dir = -1;
				return true;
			}
			if (y >= top + seqlayout::SHIFT_RIGHT_DY && y <= top + seqlayout::SHIFT_RIGHT_DY + seqlayout::SHIFT_H) {
				*row = r;
				*dir = 1;
				return true;
			}
		}
		return false;
	}

	/** The row under the mouse (panel pixels), or -1 */
	int rowAt(Vec p) {
		float y = p.y / SVG_DPI * MM_PER_IN;
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++)
			if (y >= seqlayout::rowTop(r) && y <= seqlayout::rowTop(r) + seqlayout::ROW_H)
				return r;
		return -1;
	}

	/** Left / right arrow keys over a row: shift its trigs */
	void onHoverKey(const HoverKeyEvent& e) override {
		if (module && (e.action == GLFW_PRESS || e.action == GLFW_REPEAT) && (e.mods & RACK_MOD_MASK) == 0 &&
			(e.key == GLFW_KEY_LEFT || e.key == GLFW_KEY_RIGHT)) {
			int r = rowAt(e.pos);
			if (r >= 0) {
				module->shiftRow(r, e.key == GLFW_KEY_RIGHT ? 1 : -1);
				e.consume(this);
				return;
			}
		}
		OpaqueWidget::onHoverKey(e);
	}

	void onDoubleClick(const DoubleClickEvent& e) override {
		if (!module || pressRow < 0 || pressCol < 0)
			return;
		if (pressOnSlide) {
			// On the SLIDE strip: slide off, the step stays.
			module->stepData[pressRow][pressCol].slide = 0.f;
			e.consume(this);
			return;
		}
		bool justCreated = pressRow == createdRow && pressCol == createdCol && system::getTime() - createdAt < 0.6;
		if (!justCreated && module->velocity[pressRow][pressCol] > 0.f) {
			module->deleteStep(pressRow, pressCol);
			if (module->selRow == pressRow && module->selStep == pressCol)
				module->select(-1, -1);
		}
		createdRow = createdCol = -1;
		e.consume(this);
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		Vec local = mouseOverride ? mouseAt : APP->scene->getMousePos().minus(getAbsoluteOffset(Vec(0, 0))).div(getAbsoluteZoom());
		if (slideRow >= 0) {
			module->stepData[slideRow][slideCol].slide = slideAt(slideCol, local.x);
			return;
		}
		if (dragRow < 0)
			return;
		if (std::fabs(local.y - pressY) > 2.f)
			moved = true;
		// The step under the mouse, in the row the drag began in; every step passed on the way gets the same.
		float x = local.x / SVG_DPI * MM_PER_IN;
		int col = clamp((int) std::floor((x - seqlayout::GRID_X0) / seqlayout::cellW()), 0, WaveArtifactSequencer::STEPS - 1);
		int step = (col > dragCol) ? 1 : -1;
		while (dragCol != col) {
			dragCol += step;
			moved = true;
			if (erasing)
				module->deleteStep(dragRow, dragCol);
			else
				module->velocity[dragRow][dragCol] = velocityAt(dragRow, local.y);
		}
		if (!erasing && (moved || wasOff))
			module->velocity[dragRow][dragCol] = moved ? velocityAt(dragRow, local.y) : std::max(velocityAt(dragRow, local.y), MIN_ON);
	}

	void onDragEnd(const DragEndEvent& e) override {
		// Steps dragged down to off lose their locks, condition and slide now (kept during the drag, so dragging back
		// up brings a step back whole).
		if (module && dragRow >= 0) {
			for (int s = 0; s < WaveArtifactSequencer::STEPS; s++)
				if (module->velocity[dragRow][s] <= 0.f)
					module->deleteStep(dragRow, s);
		}
		dragRow = dragCol = -1;
		slideRow = slideCol = -1;
		erasing = false;
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float mm = SVG_DPI / MM_PER_IN;
		bool dark = settings::preferDarkPanels;
		float cw = seqlayout::cellW();
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++) {
			// A row whose marker is off plays nothing: drawn dimmed (its steps can still be edited)
			nvgSave(vg);
			if (module && !module->rowOn[r].load(std::memory_order_relaxed))
				nvgGlobalAlpha(vg, 0.3f);
			NVGcolor col = layout::MARKER_COLORS[r];
			float top = seqlayout::rowTop(r);
			int len = module ? module->length[r] : 16;
			int playing = module ? module->playing[r].load() : (r == 0 ? 3 : -1);
			for (int s = 0; s < WaveArtifactSequencer::STEPS; s++) {
				float x = seqlayout::cellX(s);
				float v = module ? module->velocity[r][s] : ((s % 4 == 0 && r < 3) ? 0.8f : 0.f);
				bool inside = s < len;
				float cx = (x + seqlayout::CELL_GAP / 2.f) * mm, cy = top * mm, cwPx = (cw - seqlayout::CELL_GAP) * mm, chPx = seqlayout::ROW_H * mm;
				nvgBeginPath(vg);
				nvgRoundedRect(vg, cx, cy, cwPx, chPx, 1.5f);
				// Every fourth step is a shade lighter, like the beats of a bar.
				bool beat = (s / 4) % 2 == 0;
				nvgFillColor(vg, dark ? (beat ? nvgRGB(0x3a, 0x3f, 0x4a) : nvgRGB(0x30, 0x34, 0x3d)) : (beat ? nvgRGB(0xc4, 0xc1, 0xb9) : nvgRGB(0xcf, 0xcc, 0xc4)));
				nvgFill(vg);
				if (v > 0.f) {
					float h = seqlayout::ROW_H * v;
					nvgBeginPath(vg);
					nvgRoundedRect(vg, cx, (top + seqlayout::ROW_H - h) * mm, cwPx, h * mm, 1.5f);
					nvgFillColor(vg, nvgTransRGBAf(col, inside ? 1.f : 0.3f));
					nvgFill(vg);
				}
				if (!inside) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, cx, cy, cwPx, chPx, 1.5f);
					nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, dark ? 0.45f : 0.3f));
					nvgFill(vg);
				}
				if (module && v > 0.f) {
					const wa::StepData& d = module->stepData[r][s];
					// Locks: a dot in the top corner. Slide: a slash in the other. Condition: its text at the bottom.
					if (d.mask) {
						nvgBeginPath(vg);
						nvgCircle(vg, cx + cwPx - 3.2f, cy + 3.2f, 1.9f);
						nvgFillColor(vg, nvgRGB(0xff, 0xff, 0xff));
						nvgFill(vg);
						nvgStrokeColor(vg, nvgRGB(0x16, 0x18, 0x22));
						nvgStrokeWidth(vg, 0.7f);
						nvgStroke(vg);
					}
					// SLIDE strip under the cell, apart from it: a track in the row's color, filled from the left as far
					// as the slide goes.
					float sx = cx, sw = cwPx;
					float sh = seqlayout::SLIDE_H * mm, sy = seqlayout::slideTop(r) * mm;
					nvgBeginPath(vg);
					nvgRoundedRect(vg, sx, sy, sw, sh, 1.f);
					nvgFillColor(vg, dark ? nvgRGB(0x1c, 0x1f, 0x26) : nvgRGB(0xb3, 0xb0, 0xa8));
					nvgFill(vg);
					nvgStrokeColor(vg, nvgTransRGBAf(col, inside ? 0.7f : 0.25f));
					nvgStrokeWidth(vg, 0.6f);
					nvgStroke(vg);
					if (d.slide > 0.f) {
						nvgBeginPath(vg);
						nvgRoundedRect(vg, sx, sy, std::max(sw * d.slide, 1.6f), sh, 1.f);
						nvgFillColor(vg, nvgTransRGBAf(col, inside ? 1.f : 0.3f));
						nvgFill(vg);
					}
					// While its strip is dragged: the amount in the cell
					if (slideRow == r && slideCol == s && font) {
						nvgFontFaceId(vg, font->handle);
						nvgFontSize(vg, 7.f);
						nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
						nvgFillColor(vg, nvgRGB(0x16, 0x18, 0x22));
						std::string pct = d.slide > 0.f ? string::f("%d%%", (int) std::round(d.slide * 100.f)) : "off";
						nvgText(vg, cx + cwPx / 2.f, cy + chPx - 1.f, pct.c_str(), NULL);
					}
					char text[16];
					wa::conditionText(d, text, sizeof(text));
					if (text[0] && font && !(slideRow == r && slideCol == s)) {
						nvgFontFaceId(vg, font->handle);
						nvgFontSize(vg, 6.5f);
						nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
						nvgFillColor(vg, nvgRGB(0x16, 0x18, 0x22));
						nvgText(vg, cx + cwPx / 2.f, cy + chPx - 1.f, text, NULL);
					}
					// Tied to the trig before (a long gate): the two bars joined across the gap, with a bar on top
					if (module->isTied(r, s) && inside) {
						float hPrev = seqlayout::ROW_H * module->velocity[r][s - 1];
						float h = std::min(hPrev, seqlayout::ROW_H * v);
						// Across the gap (and the rounded corners)
						float gx0 = cx - seqlayout::CELL_GAP * mm - 1.6f, gx1 = cx + 1.6f;
						float gy0 = (top + seqlayout::ROW_H - h) * mm;
						nvgBeginPath(vg);
						nvgRect(vg, gx0, gy0, gx1 - gx0, cy + chPx - gy0 - 1.5f);
						nvgFillColor(vg, col);
						nvgFill(vg);
						float ly = (top + seqlayout::ROW_H - h) * mm + 1.6f;
						nvgBeginPath(vg);
						nvgMoveTo(vg, cx - cwPx / 2.f - seqlayout::CELL_GAP * mm, ly);
						nvgLineTo(vg, cx + cwPx / 2.f, ly);
						nvgStrokeColor(vg, nvgRGB(0xff, 0xff, 0xff));
						nvgStrokeWidth(vg, 1.4f);
						nvgLineCap(vg, NVG_ROUND);
						nvgStroke(vg);
					}
				}
				if (s == playing) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, cx, cy, cwPx, chPx, 1.5f);
					nvgStrokeColor(vg, nvgRGB(0xff, 0xff, 0xff));
					nvgStrokeWidth(vg, 1.6f);
					nvgStroke(vg);
				}
				if (module && module->rowSel == r && s == 0) {
					// Whole row selected (Ctrl + A): one frame around every step of it
					float x0 = (seqlayout::GRID_X0 + seqlayout::CELL_GAP / 2.f) * mm, x1 = (seqlayout::GRID_X1 - seqlayout::CELL_GAP / 2.f) * mm;
					nvgBeginPath(vg);
					nvgRoundedRect(vg, x0 - 1.4f, cy - 1.4f, x1 - x0 + 2.8f, chPx + 2.8f, 2.4f);
					nvgStrokeColor(vg, nvgRGB(0x8f, 0xb8, 0xff));
					nvgStrokeWidth(vg, 2.2f);
					nvgStroke(vg);
				}
				if (module && module->selRow == r && module->selStep == s) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, cx - 1.2f, cy - 1.2f, cwPx + 2.4f, chPx + 2.4f, 2.f);
					nvgStrokeColor(vg, nvgRGB(0x8f, 0xb8, 0xff));
					nvgStrokeWidth(vg, 2.2f);
					nvgStroke(vg);
				}
			}
			// TIE handle, on hover between two lit trigs of this row: + ties them, − unties them
			if (module && hoverRow == r && tieHandleUsable(r, hoverGap)) {
				bool tied = module->isTied(r, hoverGap);
				float hx = (seqlayout::GRID_X0 + hoverGap * cw) * mm, hy = (top + TIE_DY) * mm, hr = TIE_R * mm;
				nvgBeginPath(vg);
				nvgCircle(vg, hx, hy, hr);
				nvgFillColor(vg, hoverOnHandle ? nvgRGB(0xff, 0xff, 0xff) : nvgRGB(0x16, 0x18, 0x22));
				nvgFill(vg);
				nvgStrokeColor(vg, col);
				nvgStrokeWidth(vg, 1.f);
				nvgStroke(vg);
				float a = hr * 0.5f;
				nvgBeginPath(vg);
				nvgMoveTo(vg, hx - a, hy);
				nvgLineTo(vg, hx + a, hy);
				if (!tied) {
					nvgMoveTo(vg, hx, hy - a);
					nvgLineTo(vg, hx, hy + a);
				}
				nvgStrokeColor(vg, hoverOnHandle ? nvgRGB(0x16, 0x18, 0x22) : nvgRGB(0xff, 0xff, 0xff));
				nvgStrokeWidth(vg, 1.3f);
				nvgLineCap(vg, NVG_ROUND);
				nvgStroke(vg);
			}
			nvgRestore(vg);
		}
	}
};

/** The lock lane: one setting of one row, a bar per step (the steps that are on). Drag to draw the step's lock at the
mouse height, click a locked bar to remove its lock, Shift + drag to remove the locks you pass. Unlocked steps show the
knob's value as a faint line. Click the name on the left to pick the setting. The row follows the selected step.
Its CONDITION view shows the row's trig conditions instead: drag draws chances (5 % steps, the top = Always), a click on
a trig with a condition sets it back to Always, Shift + drag erases, right-click a trig for every condition (A:B too).
Tied trigs follow their group's first trig, so they are left alone. */
struct LockLane : widget::OpaqueWidget {
	WaveArtifactSequencer* module = nullptr;
	int dragCol = -1;
	bool moved = false, erasing = false, wasLocked = false;
	float pressY = 0.f;
	bool mouseOverride = false;
	Vec mouseAt;

	int settingBase() {
		return WaveArtifact::MARKER_SETTING_PARAMS[clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1)];
	}

	/** The lane's setting on the main module (for its range), or null without one */
	ParamQuantity* quantity() {
		WaveArtifact* main = mainOf(module);
		return main ? main->getParamQuantity(settingBase() + module->laneRow) : nullptr;
	}

	int colAt(float xPx) {
		float x = xPx / SVG_DPI * MM_PER_IN;
		return (int) std::floor((x - seqlayout::GRID_X0) / seqlayout::cellW());
	}

	float valueAt(ParamQuantity* q, float yPx) {
		// Local y: the lane box starts at its own top.
		float t = clamp(1.f - (yPx / SVG_DPI * MM_PER_IN) / seqlayout::LANE_H, 0.f, 1.f);
		float v = q->getMinValue() + t * (q->getMaxValue() - q->getMinValue());
		int k = clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1);
		WaveArtifact* main = mainOf(module);
		bool choice = main && (main->lockJumpMask >> k & 1u);
		return (choice || q->snapEnabled) ? std::round(v) : v;
	}

	void openSettingMenu() {
		WaveArtifactSequencer* m = module;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel("Lane: what to draw"));
		menu->addChild(createCheckMenuItem("CONDITION (trig chances, A:B)", "", [=]() { return m->laneCondition; }, [=]() { m->laneCondition = true; }));
		menu->addChild(new MenuSeparator);
		struct Group {
			const char* name;
			int from, to;
		};
		// Index ranges in MARKER_SETTING_PARAMS order (see StepLocks.hpp)
		static const int GROUP_KEYS[][2] = {{0, 1}, {8, 12}, {13, 16}, {28, 32}, {2, 7}, {17, 27}, {33, 36}, {37, 40}, {41, 52}};
		static const char* const GROUP_NAMES[] = {"Wave", "Level / filter / drive", "Pitch / pan", "Envelope (A D S H R)", "LFO", "Grain", "Grain scan",
			"Sample", "Expander LFO / AD"};
		for (int g = 0; g < 9; g++) {
			int from = GROUP_KEYS[g][0], to = GROUP_KEYS[g][1];
			menu->addChild(createSubmenuItem(GROUP_NAMES[g], "", [=](ui::Menu* sub) {
				for (int k = from; k <= to; k++) {
					if (wa::isLockable(k))
						sub->addChild(createCheckMenuItem(wa::LOCK_LABELS[k], "", [=]() { return !m->laneCondition && m->laneSetting == k; }, [=]() { m->showLock(k); }));
				}
			}));
		}
	}

	void setLock(int s, float v) {
		module->stepData[module->laneRow][s].setLock(clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1), v);
	}

	/** CONDITION view: the step can take a condition (lit, and not tied: a tied trig follows its group) */
	bool conditionStep(int s) {
		int row = module->laneRow;
		return s >= 0 && s < WaveArtifactSequencer::STEPS && module->velocity[row][s] > 0.f && !module->isTied(row, s);
	}
	/** CONDITION view: a chance from the mouse height, in 5 % steps; the top is Always */
	void setChance(int s, float yPx) {
		float t = clamp(1.f - (yPx / SVG_DPI * MM_PER_IN) / seqlayout::LANE_H, 0.f, 1.f);
		int pct = clamp((int) std::round(t * 20.f) * 5, 5, 100);
		wa::StepData& d = module->stepData[module->laneRow][s];
		if (pct >= 100) {
			d.cond = wa::COND_ALWAYS;
		}
		else {
			d.cond = wa::COND_CHANCE;
			d.chance = (uint8_t) pct;
		}
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS)
			return;
		float x = e.pos.x / SVG_DPI * MM_PER_IN + box.pos.x / SVG_DPI * MM_PER_IN;
		float y = e.pos.y / SVG_DPI * MM_PER_IN + seqlayout::LANE_Y0;
		// CLR: the lane's setting goes back to the knob on every step of the row (Shift: every setting of the row).
		if (x >= seqlayout::CLR_X0 && x <= seqlayout::CLR_X0 + seqlayout::CLR_W && y >= seqlayout::CLR_Y0 && y <= seqlayout::CLR_Y0 + seqlayout::CLR_H) {
			if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
				int row = clamp(module->laneRow, 0, WaveArtifactSequencer::ROWS - 1);
				int k = clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1);
				bool all = (e.mods & RACK_MOD_MASK) == GLFW_MOD_SHIFT;
				// CONDITION view: the row's conditions back to Always (Shift: every row's)
				if (module->laneCondition) {
					module->clearConditions(all);
					e.consume(this);
					return;
				}
				for (int s = 0; s < WaveArtifactSequencer::STEPS; s++) {
					if (all)
						module->stepData[row][s].clearLocks();
					else
						module->stepData[row][s].clearLock(k);
				}
			}
			e.consume(this);
			return;
		}
		if (x < seqlayout::GRID_X0) {
			if (e.button == GLFW_MOUSE_BUTTON_LEFT || e.button == GLFW_MOUSE_BUTTON_RIGHT)
				openSettingMenu();
			e.consume(this);
			return;
		}
		if (module->laneCondition) {
			int s = colAt(e.pos.x + box.pos.x);
			if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
				if (conditionStep(s)) {
					int row = module->laneRow;
					selectStep(module, row, s);
					ui::Menu* menu = createMenu();
					menu->addChild(createMenuLabel(string::f("Marker %d, step %d: condition", row + 1, s + 1)));
					appendTrigConditionItems(menu, module, row, s);
					e.consume(this);
				}
				return;
			}
			if (e.button != GLFW_MOUSE_BUTTON_LEFT)
				return;
			e.consume(this);
			if (!conditionStep(s)) {
				dragCol = -1;
				return;
			}
			wa::StepData& d = module->stepData[module->laneRow][s];
			dragCol = s;
			pressY = e.pos.y;
			moved = false;
			erasing = (e.mods & RACK_MOD_MASK) == GLFW_MOD_SHIFT;
			wasLocked = d.cond != wa::COND_ALWAYS;
			selectStep(module, module->laneRow, s);
			if (erasing)
				d.cond = wa::COND_ALWAYS;
			else if (!wasLocked)
				setChance(s, e.pos.y);
			return;
		}
		// Right-click a lit trig: the condition of its lock of the lane's setting
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			int s = colAt(e.pos.x + box.pos.x);
			int row = module->laneRow;
			if (s < 0 || s >= WaveArtifactSequencer::STEPS || module->velocity[row][s] <= 0.f)
				return;
			int k = clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1);
			selectStep(module, row, s);
			ui::Menu* menu = createMenu();
			menu->addChild(createMenuLabel(string::f("Marker %d, step %d: %s lock", row + 1, s + 1, wa::LOCK_LABELS[k])));
			if (module->stepData[row][s].locked(k)) {
				appendLockConditionItems(menu, module, row, s, k);
				menu->addChild(new MenuSeparator);
				WaveArtifactSequencer* m = module;
				menu->addChild(createMenuItem("Remove this lock", "", [=]() { m->stepData[row][s].clearLock(k); }));
			}
			else {
				menu->addChild(createMenuLabel("Not locked: draw a lock first, then give it a condition here"));
			}
			e.consume(this);
			return;
		}
		if (e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		ParamQuantity* q = quantity();
		int s = colAt(e.pos.x + box.pos.x);
		e.consume(this);
		if (!q || s < 0 || s >= WaveArtifactSequencer::STEPS || module->velocity[module->laneRow][s] <= 0.f) {
			dragCol = -1;
			return;
		}
		int k = clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1);
		dragCol = s;
		pressY = e.pos.y;
		moved = false;
		erasing = (e.mods & RACK_MOD_MASK) == GLFW_MOD_SHIFT;
		wasLocked = module->stepData[module->laneRow][s].locked(k);
		selectStep(module, module->laneRow, s);
		if (erasing)
			module->stepData[module->laneRow][s].clearLock(k);
		else if (!wasLocked)
			setLock(s, valueAt(q, e.pos.y));
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (!module || dragCol < 0 || e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		if (module->laneCondition) {
			Vec local = mouseOverride ? mouseAt : APP->scene->getMousePos().minus(getAbsoluteOffset(Vec(0, 0))).div(getAbsoluteZoom());
			if (std::fabs(local.y - pressY) > 2.f)
				moved = true;
			int col = clamp(colAt(local.x + box.pos.x), 0, WaveArtifactSequencer::STEPS - 1);
			int dir = (col > dragCol) ? 1 : -1;
			auto touch = [&](int s) {
				if (!conditionStep(s))
					return;
				if (erasing)
					module->stepData[module->laneRow][s].cond = wa::COND_ALWAYS;
				else
					setChance(s, local.y);
			};
			while (dragCol != col) {
				dragCol += dir;
				moved = true;
				touch(dragCol);
			}
			if (moved)
				touch(dragCol);
			return;
		}
		ParamQuantity* q = quantity();
		if (!q)
			return;
		int k = clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1);
		Vec local = mouseOverride ? mouseAt : APP->scene->getMousePos().minus(getAbsoluteOffset(Vec(0, 0))).div(getAbsoluteZoom());
		if (std::fabs(local.y - pressY) > 2.f)
			moved = true;
		int col = clamp(colAt(local.x + box.pos.x), 0, WaveArtifactSequencer::STEPS - 1);
		int step = (col > dragCol) ? 1 : -1;
		auto touch = [&](int s) {
			if (module->velocity[module->laneRow][s] <= 0.f)
				return;
			if (erasing)
				module->stepData[module->laneRow][s].clearLock(k);
			else
				setLock(s, valueAt(q, local.y));
		};
		while (dragCol != col) {
			dragCol += step;
			moved = true;
			touch(dragCol);
		}
		if (moved)
			touch(dragCol);
	}

	void onDragEnd(const DragEndEvent& e) override {
		// A click on a bar that was locked, without a drag: the step plays the knob again.
		if (module && dragCol >= 0 && wasLocked && !moved && !erasing) {
			if (module->laneCondition)
				module->stepData[module->laneRow][dragCol].cond = wa::COND_ALWAYS;
			else
				module->stepData[module->laneRow][dragCol].clearLock(clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1));
		}
		dragCol = -1;
		erasing = false;
	}

	/** A short text in the middle of a cell, on a dark backing (it reads over a bar and an empty cell alike) */
	void drawTag(NVGcontext* vg, std::shared_ptr<window::Font> font, const char* text, float x, float w, float h, NVGcolor color) {
		nvgFontFaceId(vg, font->handle);
		nvgFontSize(vg, 6.5f);
		float bounds[4];
		nvgTextBounds(vg, 0.f, 0.f, text, NULL, bounds);
		float tw = bounds[2] - bounds[0] + 2.f, th = 7.f;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, x + (w - tw) / 2.f, (h - th) / 2.f, tw, th, 1.5f);
		nvgFillColor(vg, nvgRGBA(0x16, 0x18, 0x22, 0xc8));
		nvgFill(vg);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, color);
		nvgText(vg, x + w / 2.f, h / 2.f + 0.3f, text, NULL);
	}

	/** CONDITION view, one lit trig: a chance as a bar from the bottom (its height the chance) with its %, A:B as an outline
	with its text, Always as a faint line at the top; a tied trig (it follows its group) as a dash. The selected trig is
	framed. */
	void drawCondition(NVGcontext* vg, std::shared_ptr<window::Font> font, int row, int s, float x, float w, float h) {
		const wa::StepData& d = module->stepData[row][s];
		NVGcolor cc = seqlayout::COND_COLOR;
		if (module->isTied(row, s)) {
			nvgBeginPath(vg);
			nvgRect(vg, x + w * 0.3f, h * 0.5f - 0.5f, w * 0.4f, 1.f);
			nvgFillColor(vg, nvgRGBAf(0.6f, 0.62f, 0.66f, 0.8f));
			nvgFill(vg);
			return;
		}
		if (d.cond == wa::COND_CHANCE) {
			float bh = h * clamp(d.chance / 100.f, 0.f, 1.f);
			nvgBeginPath(vg);
			nvgRoundedRect(vg, x, h - bh, w, std::max(bh, 1.f), 1.f);
			nvgFillColor(vg, cc);
			nvgFill(vg);
		}
		else if (d.cond == wa::COND_RATIO) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, x + 0.6f, 0.6f, w - 1.2f, h - 1.2f, 1.f);
			nvgStrokeColor(vg, cc);
			nvgStrokeWidth(vg, 1.2f);
			nvgStroke(vg);
		}
		else {
			nvgBeginPath(vg);
			nvgRect(vg, x + 1.f, 0.4f, w - 2.f, 1.2f);
			nvgFillColor(vg, nvgRGBAf(0.6f, 0.62f, 0.66f, 0.8f));
			nvgFill(vg);
		}
		char text[16];
		wa::conditionText(d, text, sizeof(text));
		if (text[0] && font)
			drawTag(vg, font, text, x, w, h, nvgRGB(0xff, 0xff, 0xff));
		if (module->selRow == row && module->selStep == s) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, x - 0.8f, -0.8f, w + 1.6f, h + 1.6f, 1.6f);
			nvgStrokeColor(vg, nvgRGB(0x8f, 0xb8, 0xff));
			nvgStrokeWidth(vg, 1.6f);
			nvgStroke(vg);
		}
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float mm = SVG_DPI / MM_PER_IN;
		bool dark = settings::preferDarkPanels;
		int row = module ? clamp(module->laneRow, 0, WaveArtifactSequencer::ROWS - 1) : 1;
		int k = module ? clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1) : 9;
		bool condView = module && module->laneCondition;
		NVGcolor col = layout::MARKER_COLORS[row];
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		// The lane's name: the row (in its color) and the setting, with a menu arrow
		if (font) {
			nvgFontFaceId(vg, font->handle);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
			nvgFontSize(vg, 7.f);
			nvgFillColor(vg, col);
			float lx = (seqlayout::LANE_LABEL_X - box.pos.x / mm) * mm;
			nvgText(vg, lx, 0.4f * mm, string::f("ROW %d LOCK", row + 1).c_str(), NULL);
			nvgFillColor(vg, dark ? nvgRGB(0xe6, 0xe8, 0xee) : nvgRGB(0x22, 0x24, 0x2c));
			// Sized for the longest name ("WAVE / START") in the space left of the steps
			nvgFontSize(vg, 6.4f);
			nvgText(vg, lx, 4.6f * mm, (std::string(condView ? "CONDITION" : wa::LOCK_LABELS[k]) + " \xe2\x96\xbe").c_str(), NULL);
		}
		// CLR button: lit while the row has locks of the lane's setting (dim when there is nothing to clear)
		{
			bool any = false;
			for (int s = 0; module && s < WaveArtifactSequencer::STEPS; s++)
				any = any || (condView ? module->stepData[row][s].cond != wa::COND_ALWAYS : module->stepData[row][s].locked(k));
			float bx = (seqlayout::CLR_X0 - box.pos.x / mm) * mm, by = (seqlayout::CLR_Y0 - seqlayout::LANE_Y0) * mm;
			float bw = seqlayout::CLR_W * mm, bh = seqlayout::CLR_H * mm;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, bx, by, bw, bh, 2.f);
			nvgFillColor(vg, any ? nvgRGB(0x16, 0x18, 0x22) : nvgRGBA(0x16, 0x18, 0x22, 0x60));
			nvgFill(vg);
			nvgStrokeColor(vg, any ? col : nvgTransRGBAf(col, 0.35f));
			nvgStrokeWidth(vg, 1.f);
			nvgStroke(vg);
			if (font) {
				nvgFontFaceId(vg, font->handle);
				nvgFontSize(vg, 6.5f);
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, any ? nvgRGB(0xff, 0xff, 0xff) : nvgRGBAf(1.f, 1.f, 1.f, 0.4f));
				nvgText(vg, bx + bw / 2.f, by + bh / 2.f + 0.5f, "CLR", NULL);
			}
		}
		ParamQuantity* q = module ? quantity() : nullptr;
		float lo = q ? q->getMinValue() : 0.f, hi = q ? q->getMaxValue() : 1.f;
		bool bipolar = lo < 0.f && std::fabs(lo + hi) <= 1e-4f * (hi - lo);
		float cw = seqlayout::cellW();
		float h = seqlayout::LANE_H * mm;
		for (int s = 0; s < WaveArtifactSequencer::STEPS; s++) {
			float x = (seqlayout::cellX(s) - box.pos.x / mm + seqlayout::CELL_GAP / 2.f) * mm, w = (cw - seqlayout::CELL_GAP) * mm;
			bool on = module ? module->velocity[row][s] > 0.f : (s % 4 == 0);
			nvgBeginPath(vg);
			nvgRoundedRect(vg, x, 0.f, w, h, 1.2f);
			nvgFillColor(vg, dark ? (on ? nvgRGB(0x34, 0x38, 0x42) : nvgRGB(0x24, 0x27, 0x2e)) : (on ? nvgRGB(0xc4, 0xc1, 0xb9) : nvgRGB(0xd8, 0xd5, 0xce)));
			nvgFill(vg);
			if (on && condView) {
				drawCondition(vg, font, row, s, x, w, h);
				continue;
			}
			if (!on || hi <= lo)
				continue;
			const wa::StepData* d = module ? &module->stepData[row][s] : nullptr;
			bool locked = d && d->locked(k);
			float value = locked ? d->value[k] : (q ? q->getValue() : 0.5f);
			float t = clamp((value - lo) / (hi - lo), 0.f, 1.f);
			float a = bipolar ? 0.5f : 0.f;
			float y0 = h * (1.f - std::max(a, t)), y1 = h * (1.f - std::min(a, t));
			if (locked) {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x, y0, w, std::max(y1 - y0, 1.f), 1.f);
				nvgFillColor(vg, col);
				nvgFill(vg);
				// Its own condition: the text in the COND color, on a dark backing in the middle of the cell
				char text[16];
				wa::conditionText(d->lockCondition(k), text, sizeof(text));
				if (text[0] && font)
					drawTag(vg, font, text, x, w, h, seqlayout::COND_COLOR);
			}
			else {
				// Not locked: the knob's own value, faint
				nvgBeginPath(vg);
				nvgRect(vg, x + 1.f, h * (1.f - t) - 0.6f, w - 2.f, 1.2f);
				nvgFillColor(vg, nvgRGBAf(0.6f, 0.62f, 0.66f, 0.8f));
				nvgFill(vg);
			}
			if (module && module->selRow == row && module->selStep == s) {
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x - 0.8f, -0.8f, w + 1.6f, h + 1.6f, 1.6f);
				nvgStrokeColor(vg, nvgRGB(0x8f, 0xb8, 0xff));
				nvgStrokeWidth(vg, 1.6f);
				nvgStroke(vg);
			}
		}
		// The playing step of the lane's row, framed like on the grid, so the lock that plays now is seen
		{
			int playing = module ? module->playing[row].load() : 3;
			if (playing >= 0 && playing < WaveArtifactSequencer::STEPS) {
				float x = (seqlayout::cellX(playing) - box.pos.x / mm + seqlayout::CELL_GAP / 2.f) * mm, w = (cw - seqlayout::CELL_GAP) * mm;
				nvgBeginPath(vg);
				nvgRoundedRect(vg, x, 0.f, w, h, 1.2f);
				nvgStrokeColor(vg, dark ? nvgRGB(0xff, 0xff, 0xff) : nvgRGB(0x22, 0x24, 0x2c));
				nvgStrokeWidth(vg, 1.6f);
				nvgStroke(vg);
			}
		}
		if (bipolar && !condView) {
			nvgBeginPath(vg);
			nvgRect(vg, (seqlayout::GRID_X0 - box.pos.x / mm) * mm, h * 0.5f - 0.3f, (seqlayout::GRID_X1 - seqlayout::GRID_X0) * mm, 0.6f);
			nvgFillColor(vg, nvgRGBAf(1.f, 1.f, 1.f, 0.25f));
			nvgFill(vg);
		}
		if (module && !q && font && !condView) {
			nvgFontFaceId(vg, font->handle);
			nvgFontSize(vg, 7.f);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, dark ? nvgRGB(0xe6, 0xe8, 0xee) : nvgRGB(0x22, 0x24, 0x2c));
			nvgText(vg, ((seqlayout::GRID_X0 + seqlayout::GRID_X1) / 2.f - box.pos.x / mm) * mm, h / 2.f, "snap to the left of Wave Artifacts to draw locks", NULL);
		}
	}
};

/** A row's number and its length, in the marker's color */
struct StepRowLabels : widget::TransparentWidget {
	WaveArtifactSequencer* module = nullptr;

	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		NVGcontext* vg = args.vg;
		float mm = SVG_DPI / MM_PER_IN;
		nvgFontFaceId(vg, font->handle);
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++) {
			NVGcolor col = layout::MARKER_COLORS[r];
			float cy = (seqlayout::rowTop(r) + seqlayout::ROW_H / 2.f) * mm;
			nvgBeginPath(vg);
			nvgCircle(vg, seqlayout::BADGE_X * mm, cy - 2.f * mm, 3.1f * mm);
			nvgFillColor(vg, col);
			nvgFill(vg);
			nvgFontSize(vg, 11.f);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
			nvgFillColor(vg, nvgRGB(0x16, 0x18, 0x22));
			nvgText(vg, seqlayout::BADGE_X * mm, cy - 2.f * mm + 3.8f, std::to_string(r + 1).c_str(), NULL);
			nvgFontSize(vg, 8.f);
			nvgFillColor(vg, col);
			std::string len = module ? string::f("%d", module->length[r]) : "16";
			nvgText(vg, seqlayout::BADGE_X * mm, cy + 5.5f * mm, len.c_str(), NULL);
			// The row's clock, under its knob
			std::string mult = module ? WaveArtifactSequencer::multiplierText((int) std::round(module->params[WaveArtifactSequencer::CLOCK_MULT_PARAM + r].getValue())) : "x4";
			nvgText(vg, seqlayout::CLOCK_KNOB_X * mm, cy + 5.5f * mm, mult.c_str(), NULL);
			// Shift arrows: ◀ on top, ▶ below, in the row's color
			float top = seqlayout::rowTop(r);
			for (int a = 0; a < 2; a++) {
				float bx = seqlayout::SHIFT_X0 * mm, by = (top + (a == 0 ? seqlayout::SHIFT_LEFT_DY : seqlayout::SHIFT_RIGHT_DY)) * mm;
				float bw = seqlayout::SHIFT_W * mm, bh = seqlayout::SHIFT_H * mm;
				nvgBeginPath(vg);
				nvgRoundedRect(vg, bx, by, bw, bh, 1.5f);
				nvgFillColor(vg, nvgRGBA(0x16, 0x18, 0x22, 0xb0));
				nvgFill(vg);
				float cxA = bx + bw / 2.f, cyA = by + bh / 2.f, sz = 0.26f * std::min(bw, bh);
				nvgBeginPath(vg);
				if (a == 0) {
					nvgMoveTo(vg, cxA - sz, cyA);
					nvgLineTo(vg, cxA + sz, cyA - sz);
					nvgLineTo(vg, cxA + sz, cyA + sz);
				}
				else {
					nvgMoveTo(vg, cxA + sz, cyA);
					nvgLineTo(vg, cxA - sz, cyA - sz);
					nvgLineTo(vg, cxA - sz, cyA + sz);
				}
				nvgClosePath(vg);
				nvgFillColor(vg, col);
				nvgFill(vg);
			}
		}
	}
};

/** A panel action button (RND, COND CLR): lit while held; the press runs the action, undoable (Shift:
every row). */
struct ActionButton : MomentaryLitButton {
	std::string undoName;
	std::function<bool(WaveArtifactSequencer*, bool allRows)> action;
	void onButton(const ButtonEvent& e) override {
		bool press = e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && ((e.mods & RACK_MOD_MASK) & ~GLFW_MOD_SHIFT) == 0;
		MomentaryLitButton::onButton(e);
		if (press && module && action) {
			WaveArtifactSequencer* m = (WaveArtifactSequencer*) module;
			bool all = (e.mods & GLFW_MOD_SHIFT) != 0;
			undoable(m, undoName, [=]() { return action(m, all); });
		}
	}
};

static ActionButton* createActionButton(Vec mmCenter, Module* module, int paramId, const std::string& label, NVGcolor color, Vec mmSize,
	const std::string& undoName, std::function<bool(WaveArtifactSequencer*, bool)> action) {
	ActionButton* b = createLitToggle<ActionButton>(mmCenter, module, paramId, label, color, mmSize);
	b->undoName = undoName;
	b->action = action;
	return b;
}

struct WaveArtifactSequencerWidget : ModuleWidget {
	StepGrid* grid = nullptr;
	LockLane* lane = nullptr;

	WaveArtifactSequencerWidget(WaveArtifactSequencer* module) {
		using namespace seqlayout;
		setModule(module);
		setPanel(createPanel(
			asset::plugin(pluginInstance, "res/waveArtifactSequencer.svg"),
			asset::plugin(pluginInstance, "res/waveArtifactSequencer-dark.svg")
		));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		PanelLabels* labels = createWidget<PanelLabels>(Vec(0, 0));
		labels->box.size = box.size;
		labels->add(Vec(TITLE_X, 6.f), "wA SEQUENCER", 9.f);
		for (int s = 0; s < WaveArtifactSequencer::STEPS; s++)
			labels->add(Vec(GRID_X0 + (s + 0.5f) * cellW(), slideTop(WaveArtifactSequencer::ROWS - 1) + SLIDE_H + 1.9f), string::f("%d", s + 1), 5.f);
		// Help, bottom
		const float helpX = 89.f;
		labels->add(Vec(helpX, 117.6f), "drag: draw (height = velocity)  click: select  double-click: delete  Shift+drag: erase  strip under a trig: slide", 5.5f);
		labels->add(Vec(helpX, 121.1f), "selected step: turn the marker's knobs to lock  Ctrl+click: row end  + between trigs: tie (long gate)  right-click: step", 5.5f);
		labels->add(Vec(W / 2.f, 125.2f), "left of Wave Artifacts   |   PLAY: SEQ   |   clock: CLOCK input or internal tempo", 5.5f);
		labels->add(Vec(CLOCK_KNOB_X, 14.5f), "CLOCK", 5.f);
		labels->add(Vec(BADGE_X, 14.5f), "LEN", 5.f);
		labels->add(Vec(GATE_X, GATE_LABEL_Y), "GATE", 5.f, true);
		labels->add(Vec(MOD_X, GATE_LABEL_Y), "MOD", 5.f, true);
		labels->add(REC_LABEL, "REC", 6.f);
		labels->add(RND_LABEL, "LOCKS", 6.f);
		labels->add(COND_LABEL, "COND", 6.f);
		addChild(labels);

		StepRowLabels* rows = createWidget<StepRowLabels>(Vec(0, 0));
		rows->box.size = box.size;
		rows->module = module;
		addChild(rows);

		grid = createWidget<StepGrid>(Vec(0, 0));
		grid->box.size = box.size;
		grid->module = module;
		addChild(grid);

		// The lane covers its name on the left and the step columns; on top of the grid.
		lane = createWidget<LockLane>(mm2px(Vec(2.f, LANE_Y0)));
		lane->box.size = mm2px(Vec(GRID_X1 - 2.f, LANE_H));
		lane->module = module;
		addChild(lane);

		// The row clock knobs go on top of the grid, which covers the whole panel.
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++)
			addParam(createParamCentered<Trimpot>(mm2px(Vec(CLOCK_KNOB_X, rowTop(r) + 4.8f)), module, WaveArtifactSequencer::CLOCK_MULT_PARAM + r));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(mm2px(REC_BUTTON), module, WaveArtifactSequencer::REC_PARAM, WaveArtifactSequencer::REC_LIGHT));
		addParam(createActionButton(RND_BUTTON, module, WaveArtifactSequencer::RND_PARAM, "RND", layout::SCREEN_TEXT, RND_BUTTON_SIZE, "random locks",
			[](WaveArtifactSequencer* m, bool all) { return m->randomizeLocks(all); }));
		auto addSlider = [&](Vec center, Vec size, int paramId, NVGcolor color) {
			MiniSlider* slider = createParam<MiniSlider>(Vec(0, 0), module, paramId);
			slider->box.size = mm2px(size);
			slider->box.pos = mm2px(center).minus(slider->box.size.div(2.f));
			slider->color = color;
			addParam(slider);
		};
		addSlider(RND_SLIDER, RND_SLIDER_SIZE, WaveArtifactSequencer::RND_AMOUNT_PARAM, layout::SCREEN_TEXT);
		addParam(createActionButton(COND_CLR_BUTTON, module, WaveArtifactSequencer::COND_CLR_PARAM, "CLR", COND_COLOR, COND_CLR_SIZE, "reset conditions",
			[](WaveArtifactSequencer* m, bool all) { m->clearConditions(all); return true; }));
		// Row GATE outputs, on top of the grid like the knobs
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++)
			addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(GATE_X, rowTop(r) + ROW_H / 2.f)), module, WaveArtifactSequencer::GATE_OUTPUT + r));
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++)
			addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(MOD_X, rowTop(r) + ROW_H / 2.f)), module, WaveArtifactSequencer::MOD_OUTPUT + r));
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++)
			addParam(createParamCentered<Trimpot>(mm2px(Vec(MOD_X, rowTop(r) + ROW_H / 2.f + MOD_LEVEL_DY)), module, WaveArtifactSequencer::MOD_LEVEL_PARAM + r));
	}

	void step() override {
		WaveArtifactSequencer* m = getModule<WaveArtifactSequencer>();
		// The selection is for one marker on the main module: if it goes, so does the selection.
		if (m && m->hasSelection()) {
			WaveArtifact* main = mainOf(m);
			if (!main || m->velocity[m->selRow][m->selStep] <= 0.f)
				m->select(-1, -1);
		}
		ModuleWidget::step();
	}

	/** The step for a copy / paste key: the one under the mouse, else the selected one. False when neither. */
	bool keyStep(Vec pos, int* r, int* s) {
		WaveArtifactSequencer* m = getModule<WaveArtifactSequencer>();
		if (!m)
			return false;
		if (grid && grid->cellAt(pos, r, s))
			return true;
		if (m->hasSelection()) {
			*r = m->selRow;
			*s = m->selStep;
			return true;
		}
		return false;
	}

	/** The row for a row paste: the one under the mouse, else the whole-row selection, else the selected step's row */
	int keyRow(Vec pos) {
		WaveArtifactSequencer* m = getModule<WaveArtifactSequencer>();
		int r = grid ? grid->rowAt(pos) : -1;
		float x = pos.x / SVG_DPI * MM_PER_IN;
		if (r >= 0 && x >= seqlayout::GRID_X0 - 1.f && x <= seqlayout::GRID_X1)
			return r;
		if (m->rowSel >= 0)
			return m->rowSel;
		return m->hasSelection() ? m->selRow : -1;
	}

	/** Copy / paste on the steps:
	- Ctrl + A over a row selects the whole row; Ctrl + C then copies it (every step, its length and clock).
	- Ctrl + C on a trig copies its velocity, locks, condition and slide.
	- Ctrl + V pastes what was copied last: a row onto the row under the mouse, a trig onto the trig under the mouse
	  (else the selected one). Elsewhere on the panel the keys copy / paste the module as usual in Rack. */
	void onHoverKey(const HoverKeyEvent& e) override {
		WaveArtifactSequencer* m = getModule<WaveArtifactSequencer>();
		if (m && e.action == GLFW_PRESS) {
			int r, s;
			SeqClipboard& clip = seqClipboard();
			if (e.isKeyCommand(GLFW_KEY_A, RACK_MOD_CTRL) && grid && grid->rowAt(e.pos) >= 0) {
				int row = grid->rowAt(e.pos);
				m->select(-1, -1);
				m->rowSel = row;
				m->laneRow = row;
				e.consume(this);
				return;
			}
			if (e.isKeyCommand(GLFW_KEY_C, RACK_MOD_CTRL)) {
				if (m->rowSel >= 0) {
					copyRowToClipboard(m, m->rowSel);
					e.consume(this);
					return;
				}
				if (keyStep(e.pos, &r, &s) && m->velocity[r][s] > 0.f) {
					clip.kind = SeqClipboard::TRIG;
					clip.velocity = m->velocity[r][s];
					clip.data = m->stepData[r][s];
					e.consume(this);
					return;
				}
			}
			if (e.isKeyCommand(GLFW_KEY_V, RACK_MOD_CTRL)) {
				int row = keyRow(e.pos);
				if (clip.kind == SeqClipboard::ROW && row >= 0) {
					pasteRowFromClipboard(m, row);
					e.consume(this);
					return;
				}
				if (clip.kind == SeqClipboard::TRIG && keyStep(e.pos, &r, &s)) {
					m->velocity[r][s] = clip.velocity;
					m->stepData[r][s] = clip.data;
					selectStep(m, r, s);
					e.consume(this);
					return;
				}
			}
		}
		ModuleWidget::onHoverKey(e);
	}

	void appendContextMenu(Menu* menu) override {
		WaveArtifactSequencer* m = getModule<WaveArtifactSequencer>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Clear all steps", "", [=]() { m->clearAll(); }));
		menu->addChild(createMenuItem("Clear all locks", "", [=]() {
			for (int r = 0; r < WaveArtifactSequencer::ROWS; r++)
				for (int s = 0; s < WaveArtifactSequencer::STEPS; s++)
					m->stepData[r][s].clearLocks();
		}));
		menu->addChild(createMenuItem("Default pattern", "", [=]() { m->loadDefault(); }));
	}
};

Model* modelWaveArtifactSequencer = createModel<WaveArtifactSequencer, WaveArtifactSequencerWidget>("waveArtifactSequencer");
