#include "WaveArtifactSequencer.hpp"
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
	for (int r = 0; r < ROWS; r++)
		playing[r] = -1;
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
		velocity[r][s] = 0.f;
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
	for (int r = 0; r < ROWS; r++)
		position[r] = -1;
}

void WaveArtifactSequencer::tickRow(int r) {
	int len = std::min(std::max(length[r], 1), STEPS);
	position[r] = (position[r] + 1) % len;
	playing[r] = position[r];
	float v = velocity[r][position[r]];
	if (v > 0.f) {
		lastVelocity[r] = v;
		fires[r]++;
	}
}

void WaveArtifactSequencer::process(const ProcessArgs& args) {
	Module* main = rightExpander.module;
	bool isLinked = isMainModule(main);
	linked = isLinked;
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
	return root;
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
inline float cellW() {
	return (GRID_X1 - GRID_X0) / WaveArtifactSequencer::STEPS;
}
inline float rowTop(int r) {
	return ROW_Y0 + r * (ROW_H + ROW_GAP);
}
} // namespace seqlayout

/** The step grid: draw steps by dragging. Press on an empty step and drag along the row: every step you pass is
switched on, at the height of the mouse (the top of a cell is 100%). Drag up and down to set a velocity. Click a step
that is on, without moving, to switch it off; Shift + drag along a row switches the steps you pass off. Ctrl + click
sets the row's length to end at that step. Right-click a row for more. */
struct StepGrid : widget::OpaqueWidget {
	WaveArtifactSequencer* module = nullptr;
	int dragRow = -1, dragCol = -1;
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
	float velocityAt(int row, float yPx) {
		float y = yPx / SVG_DPI * MM_PER_IN;
		float v = 1.f - (y - seqlayout::rowTop(row)) / seqlayout::ROW_H;
		return clamp(v, 0.05f, 1.f);
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS)
			return;
		int r, c;
		if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			float y = e.pos.y / SVG_DPI * MM_PER_IN;
			float x = e.pos.x / SVG_DPI * MM_PER_IN;
			for (r = 0; r < WaveArtifactSequencer::ROWS; r++)
				if (y >= seqlayout::rowTop(r) && y <= seqlayout::rowTop(r) + seqlayout::ROW_H && x < seqlayout::GRID_X1)
					break;
			if (r < WaveArtifactSequencer::ROWS)
				openRowMenu(r);
			e.consume(this);
			return;
		}
		if (e.button != GLFW_MOUSE_BUTTON_LEFT || !cellAt(e.pos, &r, &c))
			return;
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
		if (erasing)
			module->velocity[r][c] = 0.f;
		else if (wasOff)
			module->velocity[r][c] = velocityAt(r, e.pos.y);
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
			module->velocity[dragRow][dragCol] = erasing ? 0.f : velocityAt(dragRow, local.y);
		}
		if (!erasing && (moved || wasOff))
			module->velocity[dragRow][dragCol] = velocityAt(dragRow, local.y);
	}

	void onDragEnd(const DragEndEvent& e) override {
		// A click on a step that was on, without a drag: off.
		if (module && dragRow >= 0 && !wasOff && !moved && !erasing)
			module->velocity[dragRow][dragCol] = 0.f;
		dragRow = dragCol = -1;
		erasing = false;
	}

	void openRowMenu(int r) {
		WaveArtifactSequencer* m = module;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel("Marker " + std::to_string(r + 1) + " row"));
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
			for (int s = 0; s < WaveArtifactSequencer::STEPS; s++)
				m->velocity[r][s] = (random::uniform() < 0.35f) ? 0.4f + 0.6f * random::uniform() : 0.f;
		}));
		menu->addChild(createMenuItem("Clear row", "", [=]() { m->clearRow(r); }));
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Clear all", "", [=]() { m->clearAll(); }));
		menu->addChild(createMenuItem("Default pattern", "", [=]() { m->loadDefault(); }));
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float mm = SVG_DPI / MM_PER_IN;
		bool dark = settings::preferDarkPanels;
		float cw = seqlayout::cellW();
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++) {
			NVGcolor col = layout::MARKER_COLORS[r];
			float top = seqlayout::rowTop(r);
			int len = module ? module->length[r] : 16;
			int playing = module ? module->playing[r].load() : (r == 0 ? 3 : -1);
			for (int s = 0; s < WaveArtifactSequencer::STEPS; s++) {
				float x = seqlayout::GRID_X0 + s * cw;
				float v = module ? module->velocity[r][s] : ((s % 4 == 0 && r < 3) ? 0.8f : 0.f);
				bool inside = s < len;
				nvgBeginPath(vg);
				nvgRoundedRect(vg, (x + seqlayout::CELL_GAP / 2.f) * mm, top * mm, (cw - seqlayout::CELL_GAP) * mm, seqlayout::ROW_H * mm, 1.5f);
				// Every fourth step is a shade lighter, like the beats of a bar.
				bool beat = (s / 4) % 2 == 0;
				nvgFillColor(vg, dark ? (beat ? nvgRGB(0x3a, 0x3f, 0x4a) : nvgRGB(0x30, 0x34, 0x3d)) : (beat ? nvgRGB(0xc4, 0xc1, 0xb9) : nvgRGB(0xcf, 0xcc, 0xc4)));
				nvgFill(vg);
				if (v > 0.f) {
					float h = seqlayout::ROW_H * v;
					nvgBeginPath(vg);
					nvgRoundedRect(vg, (x + seqlayout::CELL_GAP / 2.f) * mm, (top + seqlayout::ROW_H - h) * mm, (cw - seqlayout::CELL_GAP) * mm, h * mm, 1.5f);
					nvgFillColor(vg, nvgTransRGBAf(col, inside ? 1.f : 0.3f));
					nvgFill(vg);
				}
				if (!inside) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, (x + seqlayout::CELL_GAP / 2.f) * mm, top * mm, (cw - seqlayout::CELL_GAP) * mm, seqlayout::ROW_H * mm, 1.5f);
					nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, dark ? 0.45f : 0.3f));
					nvgFill(vg);
				}
				if (s == playing) {
					nvgBeginPath(vg);
					nvgRoundedRect(vg, (x + seqlayout::CELL_GAP / 2.f) * mm, top * mm, (cw - seqlayout::CELL_GAP) * mm, seqlayout::ROW_H * mm, 1.5f);
					nvgStrokeColor(vg, nvgRGB(0xff, 0xff, 0xff));
					nvgStrokeWidth(vg, 1.6f);
					nvgStroke(vg);
				}
			}
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
		}
	}
};

struct WaveArtifactSequencerWidget : ModuleWidget {
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
		labels->add(Vec(W / 2.f, 106.f), "draw: drag along a row, height = velocity      click a step: off", 6.f);
		labels->add(Vec(W / 2.f, 110.5f), "Shift + drag: erase      Ctrl + click: row ends there      right-click: more", 6.f);
		labels->add(Vec(W / 2.f, 120.f), "left of Wave Artifacts   |   PLAY: SEQ   |   clock: CLOCK input or internal tempo", 6.f);
		labels->add(Vec(CLOCK_KNOB_X, 14.5f), "CLOCK", 5.f);
		labels->add(Vec(9.f, 14.5f), "LEN", 5.f);
		addChild(labels);

		StepRowLabels* rows = createWidget<StepRowLabels>(Vec(0, 0));
		rows->box.size = box.size;
		rows->module = module;
		addChild(rows);

		StepGrid* grid = createWidget<StepGrid>(Vec(0, 0));
		grid->box.size = box.size;
		grid->module = module;
		addChild(grid);

		// The row clock knobs go on top of the grid, which covers the whole panel.
		for (int r = 0; r < WaveArtifactSequencer::ROWS; r++)
			addParam(createParamCentered<Trimpot>(mm2px(Vec(CLOCK_KNOB_X, rowTop(r) + 4.8f)), module, WaveArtifactSequencer::CLOCK_MULT_PARAM + r));
	}

	void appendContextMenu(Menu* menu) override {
		WaveArtifactSequencer* m = getModule<WaveArtifactSequencer>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Clear all steps", "", [=]() { m->clearAll(); }));
		menu->addChild(createMenuItem("Default pattern", "", [=]() { m->loadDefault(); }));
	}
};

Model* modelWaveArtifactSequencer = createModel<WaveArtifactSequencer, WaveArtifactSequencerWidget>("waveArtifactSequencer");
