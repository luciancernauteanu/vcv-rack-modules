#include "WaveArtifactSequencer.hpp"
#include "WaveArtifact.hpp"
#include "Layout.hpp"
#include "widgets/PanelLabels.hpp"

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
	configSwitch(FILL_PARAM, 0.f, 1.f, 0.f, "Fill", {"Off", "On"});
	paramQuantities[FILL_PARAM]->randomizeEnabled = false;
	configInput(FILL_INPUT, "Fill gate (FILL while high)");
	configLight(FILL_LIGHT, "Fill");
	configSwitch(REC_PARAM, 0.f, 1.f, 0.f, "Record (live: knob moves go into the steps as they play)", {"Off", "On"});
	paramQuantities[REC_PARAM]->randomizeEnabled = false;
	configLight(REC_LIGHT, "Record");
	for (int r = 0; r < ROWS; r++)
		playing[r] = -1;
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
	playing[r] = position[r];
	float v = velocity[r][position[r]];
	if (v > 0.f) {
		const wa::StepData& s = stepData[r][position[r]];
		// A step that does not play also leaves the locks of the one before in place.
		if (!wa::conditionPasses(s, std::max(pass[r], 0L), fill, random01()))
			return;
		lastVelocity[r] = v;
		lastStep[r] = s;
		fires[r]++;
	}
}

void WaveArtifactSequencer::process(const ProcessArgs& args) {
	Module* main = rightExpander.module;
	bool isLinked = isMainModule(main);
	linked = isLinked;
	fill = params[FILL_PARAM].getValue() > 0.5f || inputs[FILL_INPUT].getVoltage() >= 1.f;
	lights[FILL_LIGHT].setBrightness(fill ? 1.f : 0.f);
	lights[REC_LIGHT].setBrightness(params[REC_PARAM].getValue() > 0.5f ? 1.f : 0.f);
	if (!isLinked) {
		warmUp = WARM_UP_SAMPLES;
		return;
	}
	const wa::ToSequencerMessage* in = (const wa::ToSequencerMessage*) rightExpander.consumerMessage;
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
			}
			if (d.cond != wa::COND_ALWAYS) {
				json_object_set_new(step, "cond", json_integer(d.cond));
				json_object_set_new(step, "chance", json_integer(d.chance));
				json_object_set_new(step, "a", json_integer(d.a));
				json_object_set_new(step, "b", json_integer(d.b));
			}
			if (d.slide)
				json_object_set_new(step, "slide", json_true());
			json_array_append_new(row, step);
		}
		json_array_append_new(data, row);
	}
	json_object_set_new(root, "stepData", data);
	json_object_set_new(root, "laneRow", json_integer(laneRow));
	json_object_set_new(root, "laneSetting", json_string(wa::LOCK_NAMES[clamp(laneSetting, 0, wa::NUM_LOCKABLE - 1)]));
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
			json_t* cond = json_object_get(step, "cond");
			if (cond) {
				d.cond = (uint8_t) clamp((int) json_integer_value(cond), 0, wa::NUM_CONDITIONS - 1);
				json_t* j;
				if ((j = json_object_get(step, "chance")))
					d.chance = (uint8_t) clamp((int) json_integer_value(j), 0, 100);
				if ((j = json_object_get(step, "b")))
					d.b = (uint8_t) clamp((int) json_integer_value(j), 1, 8);
				if ((j = json_object_get(step, "a")))
					d.a = (uint8_t) clamp((int) json_integer_value(j), 1, (int) d.b);
			}
			d.slide = json_is_true(json_object_get(step, "slide"));
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
}

// Panel: 30 HP. Geometry matches res/waveArtifactSequencer.svg.
namespace seqlayout {
static const float W = 152.4f;
static const float GRID_X0 = 31.f;
/** Row clock knob, left of the grid */
static const float CLOCK_KNOB_X = 21.f;
static const float GRID_X1 = 147.f;
static const float ROW_Y0 = 19.f;
static const float ROW_H = 14.f;
static const float ROW_GAP = 2.6f;
static const float CELL_GAP = 0.8f;
/** The lock lane under the grid (below the step numbers), and its name on the left */
static const float LANE_Y0 = 104.6f;
static const float LANE_H = 9.4f;
static const float LANE_LABEL_X = 18.6f;
/** CLR, left of the lane's name: clears the lane's setting in its row */
static const float CLR_X0 = 2.4f, CLR_W = 5.2f;
static const float CLR_Y0 = LANE_Y0 + 2.2f, CLR_H = 5.0f;
/** FILL button and gate input, under the lane's name */
static const Vec FILL_BUTTON = Vec(13.5f, 119.6f);
static const Vec FILL_INPUT = Vec(24.5f, 119.6f);
static const float FILL_LABEL_Y = 115.4f;
/** REC (live record) in the top-left corner, under the screw, with its label to the right */
static const Vec REC_BUTTON = Vec(14.f, 9.6f);
static const Vec REC_LABEL = Vec(22.f, 10.6f);
/** Shift arrows of each row, stacked between the CLOCK knob and the first step: ◀ on top, ▶ below */
static const float SHIFT_X0 = 25.4f, SHIFT_W = 4.6f, SHIFT_H = 5.2f;
static const float SHIFT_LEFT_DY = 1.2f, SHIFT_RIGHT_DY = 7.6f;
inline float cellW() {
	return (GRID_X1 - GRID_X0) / WaveArtifactSequencer::STEPS;
}
inline float rowTop(int r) {
	return ROW_Y0 + r * (ROW_H + ROW_GAP);
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
			m->stepData[r][s].mask = 0;
	}));
	menu->addChild(createMenuItem("Clear row", "", [=]() { m->clearRow(r); }));
	menu->addChild(new MenuSeparator);
	menu->addChild(createMenuItem("Clear all", "", [=]() { m->clearAll(); }));
	menu->addChild(createMenuItem("Default pattern", "", [=]() { m->loadDefault(); }));
}

/** The step's own menu: its condition, slide, locks; the row's items in a submenu */
static void openStepMenu(WaveArtifactSequencer* m, int r, int s) {
	ui::Menu* menu = createMenu();
	menu->addChild(createMenuLabel(string::f("Marker %d, step %d", r + 1, s + 1)));
	wa::StepData* d = &m->stepData[r][s];
	char text[16];
	wa::conditionText(*d, text, sizeof(text));
	menu->addChild(createSubmenuItem("Condition", d->cond == wa::COND_ALWAYS ? "always" : text, [=](ui::Menu* sub) {
		sub->addChild(createCheckMenuItem("Always", "", [=]() { return d->cond == wa::COND_ALWAYS; }, [=]() { d->cond = wa::COND_ALWAYS; }));
		sub->addChild(new MenuSeparator);
		for (int c : wa::CHANCES)
			sub->addChild(createCheckMenuItem(string::f("%d%% chance", c), "", [=]() { return d->cond == wa::COND_CHANCE && d->chance == c; },
				[=]() { d->cond = wa::COND_CHANCE; d->chance = (uint8_t) c; }));
		sub->addChild(new MenuSeparator);
		sub->addChild(createMenuLabel("A:B - pass A of every B passes of the row"));
		for (int b = 2; b <= 8; b++) {
			sub->addChild(createSubmenuItem(string::f("every %d passes", b), "", [=](ui::Menu* sub2) {
				for (int a = 1; a <= b; a++)
					sub2->addChild(createCheckMenuItem(string::f("%d:%d", a, b), "", [=]() { return d->cond == wa::COND_RATIO && d->a == a && d->b == b; },
						[=]() { d->cond = wa::COND_RATIO; d->a = (uint8_t) a; d->b = (uint8_t) b; }));
			}));
		}
		sub->addChild(new MenuSeparator);
		sub->addChild(createCheckMenuItem("FILL (only while FILL is on)", "", [=]() { return d->cond == wa::COND_FILL; }, [=]() { d->cond = wa::COND_FILL; }));
		sub->addChild(createCheckMenuItem("NOT FILL (only while FILL is off)", "", [=]() { return d->cond == wa::COND_NOT_FILL; }, [=]() { d->cond = wa::COND_NOT_FILL; }));
	}));
	menu->addChild(createBoolPtrMenuItem("Slide (glide to this step's values over one step)", "", &d->slide));
	int locks = 0;
	for (int k = 0; k < wa::NUM_LOCKABLE; k++)
		locks += d->locked(k) ? 1 : 0;
	menu->addChild(createMenuItem("Clear locks on this step", string::f("%d", locks), [=]() { d->mask = 0; }));
	menu->addChild(createMenuItem("Delete step", "", [=]() { m->deleteStep(r, s); }));
	menu->addChild(new MenuSeparator);
	menu->addChild(createSubmenuItem(string::f("Row %d", r + 1), "", [=](ui::Menu* sub) { appendRowMenu(sub, m, r); }));
}

/** The step grid: draw steps by dragging. Press on an empty step and drag along the row: every step you pass is
switched on, at the height of the mouse (the top of a cell is 100%). Drag up and down to set a velocity. A click on a
step selects it (its values show under the main module's knobs, and turning that marker's knobs locks them on it); a
double-click on a step switches it off. Shift + drag along a row switches the steps you pass off. Ctrl + click sets
the row's length to end at that step. Right-click a step for its condition, slide and locks; click outside the
steps to deselect. */
struct StepGrid : widget::OpaqueWidget {
	WaveArtifactSequencer* module = nullptr;
	int dragRow = -1, dragCol = -1;
	/** The last pressed step, for a double-click, and the step a press switched on (with when), so the double-click
	that follows a creating click does not switch it off again */
	int pressRow = -1, pressCol = -1, createdRow = -1, createdCol = -1;
	double createdAt = -10.0;
	float pressY = 0.f;
	bool wasOff = false, moved = false, erasing = false;
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

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS)
			return;
		// Any click ends a whole-row selection (Ctrl + A)
		module->rowSel = -1;
		int r, c;
		bool onCell = cellAt(e.pos, &r, &c);
		// A row's shift arrows: its trigs (and everything on them) one step left or right
		int arrowRow, arrowDir;
		if (e.button == GLFW_MOUSE_BUTTON_LEFT && arrowAt(e.pos, &arrowRow, &arrowDir)) {
			module->shiftRow(arrowRow, arrowDir);
			pressRow = pressCol = -1;
			e.consume(this);
			return;
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
		if (!module || dragRow < 0 || e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		Vec local = mouseOverride ? mouseAt : APP->scene->getMousePos().minus(getAbsoluteOffset(Vec(0, 0))).div(getAbsoluteZoom());
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
					module->stepData[dragRow][s].clear();
		}
		dragRow = dragCol = -1;
		erasing = false;
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float mm = SVG_DPI / MM_PER_IN;
		bool dark = settings::preferDarkPanels;
		float cw = seqlayout::cellW();
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++) {
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
					if (d.slide) {
						nvgBeginPath(vg);
						nvgMoveTo(vg, cx + 2.f, cy + 6.f);
						nvgLineTo(vg, cx + 6.f, cy + 2.f);
						nvgStrokeColor(vg, nvgRGB(0xff, 0xff, 0xff));
						nvgStrokeWidth(vg, 1.3f);
						nvgStroke(vg);
					}
					char text[16];
					wa::conditionText(d, text, sizeof(text));
					if (text[0] && font) {
						nvgFontFaceId(vg, font->handle);
						nvgFontSize(vg, 6.5f);
						nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM);
						nvgFillColor(vg, nvgRGB(0x16, 0x18, 0x22));
						nvgText(vg, cx + cwPx / 2.f, cy + chPx - 1.f, text, NULL);
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
		}
	}
};

/** The lock lane: one setting of one row, a bar per step (the steps that are on). Drag to draw the step's lock at the
mouse height, click a locked bar to remove its lock, Shift + drag to remove the locks you pass. Unlocked steps show the
knob's value as a faint line. Click the name on the left to pick the setting. The row follows the selected step. */
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
		menu->addChild(createMenuLabel("Lane: the setting to draw"));
		struct Group {
			const char* name;
			int from, to;
		};
		// Index ranges in MARKER_SETTING_PARAMS order (see StepLocks.hpp)
		static const int GROUP_KEYS[][2] = {{0, 1}, {8, 12}, {13, 16}, {28, 32}, {2, 7}, {17, 27}, {33, 36}};
		static const char* const GROUP_NAMES[] = {"Wave", "Level / filter / drive", "Pitch / pan", "Envelope (A D S H R)", "LFO", "Grain", "Grain scan"};
		for (int g = 0; g < 7; g++) {
			int from = GROUP_KEYS[g][0], to = GROUP_KEYS[g][1];
			menu->addChild(createSubmenuItem(GROUP_NAMES[g], "", [=](ui::Menu* sub) {
				for (int k = from; k <= to; k++) {
					if (wa::isLockable(k))
						sub->addChild(createCheckMenuItem(wa::LOCK_LABELS[k], "", [=]() { return m->laneSetting == k; }, [=]() { m->laneSetting = k; }));
				}
			}));
		}
	}

	void setLock(int s, float v) {
		module->stepData[module->laneRow][s].setLock(clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1), v);
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
				for (int s = 0; s < WaveArtifactSequencer::STEPS; s++) {
					if (all)
						module->stepData[row][s].mask = 0;
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
		if (module && dragCol >= 0 && wasLocked && !moved && !erasing)
			module->stepData[module->laneRow][dragCol].clearLock(clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1));
		dragCol = -1;
		erasing = false;
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float mm = SVG_DPI / MM_PER_IN;
		bool dark = settings::preferDarkPanels;
		int row = module ? clamp(module->laneRow, 0, WaveArtifactSequencer::ROWS - 1) : 1;
		int k = module ? clamp(module->laneSetting, 0, wa::NUM_LOCKABLE - 1) : 9;
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
			nvgFontSize(vg, 7.5f);
			nvgText(vg, lx, 4.6f * mm, (std::string(wa::LOCK_LABELS[k]) + " \xe2\x96\xbe").c_str(), NULL);
		}
		// CLR button: lit while the row has locks of the lane's setting (dim when there is nothing to clear)
		{
			bool any = false;
			for (int s = 0; module && s < WaveArtifactSequencer::STEPS; s++)
				any = any || module->stepData[row][s].locked(k);
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
		if (bipolar) {
			nvgBeginPath(vg);
			nvgRect(vg, (seqlayout::GRID_X0 - box.pos.x / mm) * mm, h * 0.5f - 0.3f, (seqlayout::GRID_X1 - seqlayout::GRID_X0) * mm, 0.6f);
			nvgFillColor(vg, nvgRGBAf(1.f, 1.f, 1.f, 0.25f));
			nvgFill(vg);
		}
		if (module && !q && font) {
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
			nvgCircle(vg, 9.f * mm, cy - 2.f * mm, 3.1f * mm);
			nvgFillColor(vg, col);
			nvgFill(vg);
			nvgFontSize(vg, 11.f);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
			nvgFillColor(vg, nvgRGB(0x16, 0x18, 0x22));
			nvgText(vg, 9.f * mm, cy - 2.f * mm + 3.8f, std::to_string(r + 1).c_str(), NULL);
			nvgFontSize(vg, 8.f);
			nvgFillColor(vg, col);
			std::string len = module ? string::f("%d", module->length[r]) : "16";
			nvgText(vg, 9.f * mm, cy + 5.5f * mm, len.c_str(), NULL);
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
		labels->add(Vec(W / 2.f, 6.f), "wA SEQUENCER", 9.f);
		for (int s = 0; s < WaveArtifactSequencer::STEPS; s++)
			labels->add(Vec(GRID_X0 + (s + 0.5f) * cellW(), rowTop(WaveArtifactSequencer::ROWS - 1) + ROW_H + 3.2f), string::f("%d", s + 1), 5.f);
		const float helpX = (GRID_X0 + GRID_X1) / 2.f;
		labels->add(Vec(helpX, 117.6f), "drag: draw (height = velocity)   click: select   double-click: delete   Shift + drag: erase", 5.5f);
		labels->add(Vec(helpX, 121.1f), "selected step: turn the marker's knobs to lock them   Ctrl + click: row ends   right-click: step", 5.5f);
		labels->add(Vec(W / 2.f, 125.2f), "left of Wave Artifacts   |   PLAY: SEQ   |   clock: CLOCK input or internal tempo", 5.5f);
		labels->add(Vec(CLOCK_KNOB_X, 14.5f), "CLOCK", 5.f);
		labels->add(Vec(9.f, 14.5f), "LEN", 5.f);
		labels->add(Vec((FILL_BUTTON.x + FILL_INPUT.x) / 2.f, FILL_LABEL_Y), "FILL", 5.f);
		labels->add(REC_LABEL, "REC", 6.f);
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
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(FILL_BUTTON), module, WaveArtifactSequencer::FILL_PARAM, WaveArtifactSequencer::FILL_LIGHT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(FILL_INPUT), module, WaveArtifactSequencer::FILL_INPUT));
		addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<RedLight>>>(mm2px(REC_BUTTON), module, WaveArtifactSequencer::REC_PARAM, WaveArtifactSequencer::REC_LIGHT));
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
					m->stepData[r][s].mask = 0;
		}));
		menu->addChild(createMenuItem("Default pattern", "", [=]() { m->loadDefault(); }));
	}
};

Model* modelWaveArtifactSequencer = createModel<WaveArtifactSequencer, WaveArtifactSequencerWidget>("waveArtifactSequencer");
