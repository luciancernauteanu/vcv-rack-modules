#include "WaveArtifactExpander.hpp"
#include "WaveArtifactSequencer.hpp"
#include "Layout.hpp"
#include "widgets/LfoControls.hpp"
#include "widgets/LinkedKnob.hpp"
#include "widgets/LfoWidgets.hpp"
#include "widgets/ModeButton.hpp"
#include "widgets/PanelLabels.hpp"
#include "widgets/StepValueBars.hpp"

bool isWaveArtifactExpander(Module* m) {
	return m && m->model == modelWaveArtifactExpander;
}

static bool isMain(Module* m) {
	return m && m->model == modelWaveArtifact;
}

WaveArtifactExpander::WaveArtifactExpander() {
	config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
	configSwitch(MARKER_PARAM, 0.f, 4.f, 0.f, "Marker", {"Marker 1", "Marker 2", "Marker 3", "Marker 4", "Marker 5"});
	getParamQuantity(MARKER_PARAM)->randomizeEnabled = false;
	// The old fixed inputs' amounts: no control on the panel (an older patch's are read once, see fromJson). Their
	// ranges stay as they were, so the saved values load as they are.
	for (int t = 0; t < OLD_CV_INPUTS; t++) {
		configParam(AMT_PARAM + t, -1.f, 1.f, 0.f, "Unused");
		getParamQuantity(AMT_PARAM + t)->randomizeEnabled = false;
	}
	for (int id : {GRAIN_START_AMT_PARAM, GRAIN_DENSITY_AMT_PARAM, GRAIN_SPREAD_AMT_PARAM}) {
		configParam(id, 0.f, 1.f, 0.f, "Unused");
		getParamQuantity(id)->randomizeEnabled = false;
	}
	// Four CV inputs, each with its own destination and amount per engine (the panel shows the running engine's)
	static const char* const ENGINE_NAMES[wa::NUM_CV_ENGINES] = {"WAVE", "GRAIN", "SAMPLE"};
	std::vector<std::string> destLabels;
	for (int d = 0; d < wa::NUM_CV_DESTS; d++)
		destLabels.push_back(wa::CV_DESTS[d].name == std::string(wa::CV_DESTS[d].grainName) ? wa::CV_DESTS[d].name
			: std::string(wa::CV_DESTS[d].name) + " / " + wa::CV_DESTS[d].grainName);
	for (int e = 0; e < wa::NUM_CV_ENGINES; e++) {
		for (int s = 0; s < wa::NUM_CV_SLOTS; s++) {
			std::string in = "CV " + std::to_string(s + 1) + " (" + ENGINE_NAMES[e] + ")";
			configSwitch(destParam(e, s), 0.f, wa::NUM_CV_DESTS - 1, (float) wa::CV_DEFAULTS[e][s], in + " goes to", destLabels);
			getParamQuantity(destParam(e, s))->randomizeEnabled = false;
			// Attenuverters, from 0 (double-click resets to 0). On GAIN the CV is a VCA: the amount's size is how much.
			configParam(amtParam(e, s), -1.f, 1.f, 0.f, in + " amount", "%", 0.f, 100.f);
		}
	}
	for (int s = 0; s < OLD_CV_INPUTS; s++) {
		if (s < wa::NUM_CV_SLOTS)
			configInput(MOD_INPUT + s, "CV " + std::to_string(s + 1));
		else
			configInput(MOD_INPUT + s, "Unused");
	}
	// The LFO and the AD now belong to the marker (main module settings, shown on this panel; see the widget). Their
	// old params here are placeholders, kept so later IDs and saved patches stay valid; only the LFO jack's level is
	// this expander's own. (LFO 2 and the ENV output are gone.)
	for (int k = 0; k < NUM_LFOS; k++) {
		for (int id : {LFO_RATE_PARAM, LFO_SYNC_PARAM, LFO_TRIPLET_PARAM, LFO_DOTTED_PARAM, LFO_SHAPE_PARAM}) {
			configParam(id + k, 0.f, 1.f, 0.f, "Unused");
			getParamQuantity(id + k)->randomizeEnabled = false;
		}
	}
	// The LFO jack's level. Unlike the CV amounts (which start at 0 so nothing is modulated until you ask), this
	// starts at 100%: an LFO output that starts silent would look broken.
	configParam(LFO_AMT_PARAM + 0, 0.f, 1.f, 1.f, "LFO output level", "%", 0.f, 100.f);
	configParam(LFO_AMT_PARAM + 1, 0.f, 1.f, 1.f, "Unused");
	getParamQuantity(LFO_AMT_PARAM + 1)->randomizeEnabled = false;
	configOutput(LFO_OUTPUT + 0, "Marker's LFO (+-10 V at 100%)");
	configInput(VOCT_INPUT, "V/OCT for this artifact (1 V/oct, added to the main V/OCT)");
	configInput(GATE_INPUT, "Gate for this artifact: starts its AD. MORPH: it sounds only while the gate is high, shaped by the AD. POLY: plays its note with its own ADSR while high, instead of the playhead");
	for (int id : {ATTACK_PARAM, DECAY_PARAM, PERC_PARAM}) {
		configParam(id, 0.f, 1.f, 0.f, "Unused");
		getParamQuantity(id)->randomizeEnabled = false;
	}
	configLight(LINK_LIGHT, "Connected to Wave Artifacts");

	leftExpander.producerMessage = new wa::ToExpanderMessage;
	leftExpander.consumerMessage = new wa::ToExpanderMessage;
	rightExpander.producerMessage = new wa::ToMainMessage;
	rightExpander.consumerMessage = new wa::ToMainMessage;
	lightDivider.setDivision(64);
}

WaveArtifactExpander::~WaveArtifactExpander() {
	delete (wa::ToExpanderMessage*) leftExpander.producerMessage;
	delete (wa::ToExpanderMessage*) leftExpander.consumerMessage;
	delete (wa::ToMainMessage*) rightExpander.producerMessage;
	delete (wa::ToMainMessage*) rightExpander.consumerMessage;
}


void WaveArtifactExpander::process(const ProcessArgs& args) {
	Module* left = leftExpander.module;
	Module* right = rightExpander.module;
	bool leftOk = isMain(left) || isWaveArtifactExpander(left);
	bool rightIsExp = isWaveArtifactExpander(right);

	// From the left: clock and our place in the chain.
	wa::ToExpanderMessage fromLeft;
	if (leftOk)
		fromLeft = *(const wa::ToExpanderMessage*) leftExpander.consumerMessage;
	int index = fromLeft.linked ? fromLeft.index + 1 : 0;
	bool active = fromLeft.linked && index <= wa::MAX_EXPANDERS;

	// GATE: Schmitt trigger (high at 1 V, low below 0.1 V). Tracked even while unlinked so the state
	// is right the moment the expander snaps on.
	bool gatePatched = inputs[GATE_INPUT].isConnected();
	if (gatePatched)
		gateTrigger.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 1.f);
	else
		gateTrigger.reset();

	// Modulation: what the expanders to our right sent, plus ours.
	wa::ToMainMessage acc;
	if (rightIsExp)
		acc = *(const wa::ToMainMessage*) rightExpander.consumerMessage;
	if (active) {
		int m = getMarker();
		// The LFO jack patched: the main module runs the marker's LFO even while it is not routed anywhere
		if (outputs[LFO_OUTPUT + 0].isConnected())
			acc.lfoJack[m] = true;
		// A patched GATE goes to the main module, which runs the marker's AD (its settings are marker settings, so
		// trigs can lock them). In MORPH the artifact's level follows the AD (the main module's 5 ms gain smoothing
		// keeps even the fastest open / close click-free); in POLY the gate plays the marker's note with its ADSR,
		// and starts the AD. Two expanders gating the same artifact: either one opens it.
		if (gatePatched) {
			acc.gatePatched[m] = true;
			acc.gateHigh[m] = acc.gateHigh[m] || gateTrigger.isHigh();
			acc.modulated[m] = true;
		}
		if (inputs[VOCT_INPUT].isConnected()) {
			acc.voct[m] += inputs[VOCT_INPUT].getVoltage();
			acc.modulated[m] = true;
		}
		int e = fromLeft.granular ? 1 : (fromLeft.sample ? 2 : 0);
		for (int s = 0; s < wa::NUM_CV_SLOTS; s++) {
			if (!inputs[MOD_INPUT + s].isConnected())
				continue;
			float amt = params[amtParam(e, s)].getValue();
			if (amt == 0.f)
				continue;
			acc.modulated[m] = true;
			float v = inputs[MOD_INPUT + s].getVoltage();
			const wa::CvDestInfo& d = wa::CV_DESTS[destOf(e, s)];
			// The knobs with an audio-rate path take it (GAIN as a VCA); the others are added where the main module
			// reads its settings
			if (d.fast == wa::MOD_GAIN)
				acc.gainMul[m] *= wa::vcaLevel(std::fabs(amt), v);
			else if (d.fast >= 0)
				acc.mod[m][d.fast] += amt * v;
			else if (acc.numKnobMods < wa::MAX_CV_KNOB_MODS)
				acc.knobMods[acc.numKnobMods++] = {(int8_t) m, (int8_t) d.setting, amt * v / 10.f};
		}
	}
	if (leftOk) {
		*(wa::ToMainMessage*) left->rightExpander.producerMessage = acc;
		left->rightExpander.requestMessageFlip();
	}
	if (rightIsExp) {
		wa::ToExpanderMessage* out = (wa::ToExpanderMessage*) right->leftExpander.producerMessage;
		out->clock = fromLeft.clock;
		out->index = index;
		out->linked = fromLeft.linked;
		out->reset = fromLeft.reset;
		out->granular = fromLeft.granular;
		out->sample = fromLeft.sample;
		out->poly = fromLeft.poly;
		std::copy(fromLeft.modLfo, fromLeft.modLfo + wa::NUM_MARKERS, out->modLfo);
		right->leftExpander.requestMessageFlip();
	}

	// LFO jack: the marker's LFO, which the main module runs (its settings are marker settings, so trigs can lock
	// them), at this expander's AMT. Silent while not linked.
	float lfoOut = active ? fromLeft.modLfo[getMarker()] * params[LFO_AMT_PARAM + 0].getValue() : 0.f;
	outputs[LFO_OUTPUT + 0].setVoltage(10.f * lfoOut);

	if (lightDivider.process()) {
		float dt = args.sampleTime * lightDivider.getDivision();
		lights[LINK_LIGHT + 0].setBrightnessSmooth(active ? 1.f : 0.f, dt);
		lights[LINK_LIGHT + 1].setBrightnessSmooth(fromLeft.linked && !active ? 1.f : 0.f, dt);
		lights[LFO_LIGHT + 0].setBrightnessSmooth(std::max(lfoOut, 0.f), dt);
		lights[LFO_LIGHT + 1].setBrightnessSmooth(std::max(-lfoOut, 0.f), dt);
		linked = active;
		chainIndex = index;
		engine = !fromLeft.linked ? 0 : (fromLeft.granular ? 1 : (fromLeft.sample ? 2 : 0));
		marker = getMarker();
	}
}

json_t* WaveArtifactExpander::dataToJson() {
	json_t* rootJ = json_object();
	// Marks a patch with the four assignable inputs (an older one gets its fixed inputs' meanings, see fromJson)
	json_object_set_new(rootJ, "cvSlots", json_integer(1));
	return rootJ;
}

void WaveArtifactExpander::fromJson(json_t* rootJ) {
	Module::fromJson(rootJ);
	json_t* dataJ = json_object_get(rootJ, "data");
	if (!(dataJ && json_object_get(dataJ, "cvSlots")))
		migrateOldInputs();
}

void WaveArtifactExpander::migrateOldInputs() {
	// A patch from before the four assignable inputs had six fixed ones: WAVE GAIN CUTOFF DRIVE FINE PAN (GRAIN:
	// START GAIN CUTOFF P.SPRD DENSITY PAN). The first four inputs keep what they did, with their amounts; the
	// last two are gone. Only when one of them was in use: otherwise each engine starts on its own defaults.
	float old[4];
	bool used = params[GRAIN_START_AMT_PARAM].getValue() != 0.f;
	for (int s = 0; s < 4; s++) {
		old[s] = params[AMT_PARAM + s].getValue();
		used = used || old[s] != 0.f;
	}
	if (!used)
		return;
	static const int WAVE_DESTS[4] = {wa::CV_WAVE, wa::CV_GAIN, wa::CV_CUTOFF, wa::CV_DRIVE};
	static const int GRAIN_DESTS[4] = {wa::CV_WAVE, wa::CV_GAIN, wa::CV_CUTOFF, wa::CV_PSCATTER};
	for (int e = 0; e < wa::NUM_CV_ENGINES; e++) {
		for (int s = 0; s < 4; s++) {
			params[destParam(e, s)].setValue((float) (e == 1 ? GRAIN_DESTS[s] : WAVE_DESTS[s]));
			params[amtParam(e, s)].setValue(e == 1 && s == 0 ? params[GRAIN_START_AMT_PARAM].getValue() : old[s]);
		}
	}
}

// Panel: 10 HP. Geometry matches res/waveArtifactExpander.svg.
namespace xlayout {
static const float W = 50.8f;
static const Vec MARKER_BUTTON = Vec(13.f, 16.5f);
static const Vec MARKER_BUTTON_SIZE = Vec(16.f, 6.5f);
// V/OCT and GATE jacks in the header; the LINK light sits next to the title.
static const Vec VOCT_JACK = Vec(29.5f, 17.4f);
static const Vec GATE_JACK = Vec(40.5f, 17.4f);
static const Vec LINK_LIGHT_POS = Vec(44.5f, 6.f);
static const float TOP_LABEL_Y = 22.4f;
// Order top to bottom: header (MARKER, V/OCT, GATE), the marker's AD, CV grid, the marker's LFO.
// CV section: two columns of two cells, each its destination (a menu) above a CV jack with its AMT
// trimpot beside it.
static const float CV_CELL_X[2] = {4.f, 25.4f};
static const float CV_JACK_DX = 6.2f;
static const float CV_AMT_DX = 16.4f;
static const float CV_NAME_DX = 10.7f;
static const float CV_JACK_Y[2] = {52.f, 70.f};
static const float CV_NAME_DY = -6.4f;
static const Vec CV_DEST_SIZE = Vec(19.f, 4.4f);
// The marker's AD: ATTACK, DECAY, PERC, then where it goes (DEST) and how much (DEPTH); names above.
static const float AD_NAME_Y = 28.4f;
static const float AD_Y = 34.2f;
static const float ATTACK_X = 8.2f;
static const float DECAY_X = 15.6f;
static const Vec PERC_BUTTON = Vec(23.7f, 34.2f);
static const Vec PERC_BUTTON_SIZE = Vec(7.4f, 4.4f);
static const Vec AD_DEST_BUTTON = Vec(33.4f, 34.2f);
static const float AD_DEPTH_X = 42.6f;
static const Vec DEST_BUTTON_SIZE = Vec(10.f, 4.6f);
// The marker's LFO, left column, top to bottom: title (+ light), shape screen, SYNC / T / D centered under the
// screen, RATE + AMT (the jack's level) with their labels above, the output. Right column: where it goes (DEST)
// and how much (DEPTH).
static const float LFO_X = 14.8f;
static const float LFO_LABEL_Y = 83.2f;
static const float LFO_SCREEN_Y = 88.8f;
static const Vec LFO_SCREEN_SIZE = Vec(16.f, 6.4f);
static const float LFO_BUTTON_Y = 95.2f;
static const float LFO_KNOB_LABEL_Y = 100.f;
static const float LFO_KNOB_Y = 104.4f;
static const float LFO_KNOB_DX = 5.1f;
static const float LFO_OUT_Y = 114.6f;
static const float LFO_DEST_X = 36.2f;
static const float LFO_DEST_LABEL_Y = 87.4f;
static const float LFO_DEST_Y = 92.6f;
} // namespace xlayout

/** The main module this expander is chained to, or null */
static WaveArtifact* mainOf(Module* expander) {
	Module* p = expander ? expander->leftExpander.module : nullptr;
	for (int n = 0; p && n <= wa::MAX_EXPANDERS; n++) {
		if (isMain(p))
			return (WaveArtifact*) p;
		if (!isWaveArtifactExpander(p))
			return nullptr;
		p = p->leftExpander.module;
	}
	return nullptr;
}

/** A CV input's destination: shows where it goes; a click opens the list of the knobs this engine has */
struct CvDestButton : ModeButton {
	int engine = 0;

	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && (e.mods & RACK_MOD_MASK) == 0) {
			engine::ParamQuantity* pq = getParamQuantity();
			if (pq && module) {
				ui::Menu* menu = createMenu();
				menu->addChild(createMenuLabel(pq->getLabel()));
				int64_t moduleId = module->id;
				int id = paramId;
				int eng = engine;
				int now = (int) std::round(pq->getValue());
				for (int d = 0; d < wa::NUM_CV_DESTS; d++) {
					if (!wa::cvDestOffered(d, eng))
						continue;
					menu->addChild(createCheckMenuItem(wa::cvDestName(d, eng), "",
						[=]() { return now == d; },
						[=]() {
							Module* m = APP->engine->getModule(moduleId);
							if (!m)
								return;
							float oldValue = m->params[id].getValue();
							m->params[id].setValue((float) d);
							history::ParamChange* h = new history::ParamChange;
							h->name = "change CV destination";
							h->moduleId = moduleId;
							h->paramId = id;
							h->oldValue = oldValue;
							h->newValue = (float) d;
							APP->history->push(h);
						}));
				}
			}
			e.consume(this);
			return;
		}
		ParamWidget::onButton(e);
	}
};

struct WaveArtifactExpanderWidget : ModuleWidget {
	/** Per CV input and engine: its destination and its amount, at the same spots; the running engine's are shown */
	ParamWidget* cvDests[wa::NUM_CV_ENGINES][wa::NUM_CV_SLOTS] = {};
	ParamWidget* cvAmts[wa::NUM_CV_ENGINES][wa::NUM_CV_SLOTS] = {};
	int shownEngine = -1;

	/** The controls of the marker's LFO and AD: main module params (a block of NUM_MARKERS IDs from `base`), shown
	for this expander's marker. Each frame they point at the main module and the marker as they are now, so a step
	selected on the sequencer locks them like the main module's own knobs. Not linked: no param (inactive). */
	struct Bound {
		ParamWidget* widget;
		int base;
	};
	std::vector<Bound> bound;
	LinkedKnob<LfoRateKnobT<RoundSmallBlackKnob>>* rateKnob = nullptr;
	ModeButton* destButtons[2] = {};
	WaveArtifact* boundMain = nullptr;
	/** Trig-lock dots and value bars over the bound controls (StepValueBars, as on the main module) */
	StepValueBars* stepBars = nullptr;
	int boundMarker = -1;

	template <class T>
	T* bind(T* w, int base) {
		bound.push_back({w, base});
		return w;
	}

	void rebind(WaveArtifact* main, int marker) {
		boundMain = main;
		if (stepBars)
			stepBars->module = main;
		boundMarker = marker;
		for (Bound& b : bound) {
			b.widget->module = main;
			b.widget->paramId = b.base + marker;
		}
		if (rateKnob) {
			rateKnob->syncId = WaveArtifact::MARKER_MOD_LFO_SYNC_PARAM + marker;
			rateKnob->tripletId = WaveArtifact::MARKER_MOD_LFO_TRIPLET_PARAM + marker;
			rateKnob->dottedId = WaveArtifact::MARKER_MOD_LFO_DOTTED_PARAM + marker;
		}
		// Redraw each one at its new value
		for (Bound& b : bound) {
			widget::Widget::ChangeEvent e;
			b.widget->onChange(e);
		}
	}

	void onButton(const ButtonEvent& e) override {
		ModuleWidget::onButton(e);
		// A left click on the panel itself (no control took it): leave the sequencer step being locked, as on the main
		// module. (With "Lock module positions" Rack takes the click with no target.)
		bool onPanel = e.getTarget() == this || (e.getTarget() == nullptr && e.isConsumed());
		if (boundMain && onPanel && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			WaveArtifactSequencer* seq = boundMain->sequencerOnLeft();
			if (seq)
				seq->select(-1, -1);
		}
	}

	void step() override {
		WaveArtifactExpander* m = dynamic_cast<WaveArtifactExpander*>(module);
		int engine = m ? clamp(m->engine.load(std::memory_order_relaxed), 0, wa::NUM_CV_ENGINES - 1) : 0;
		bool grain = engine == 1, sample = engine == 2;
		if (engine != shownEngine) {
			shownEngine = engine;
			for (int e = 0; e < wa::NUM_CV_ENGINES; e++) {
				for (int s = 0; s < wa::NUM_CV_SLOTS; s++) {
					cvDests[e][s]->setVisible(e == engine);
					cvAmts[e][s]->setVisible(e == engine);
				}
			}
		}
		WaveArtifact* main = (m && m->linked.load(std::memory_order_relaxed)) ? mainOf(m) : nullptr;
		int marker = m ? clamp(m->marker.load(std::memory_order_relaxed), 0, wa::NUM_MARKERS - 1) : 0;
		if (main != boundMain || marker != boundMarker)
			rebind(main, marker);
		const char* const* destNames = grain ? wa::MOD_DEST_SHORT_GRAIN : (sample ? wa::MOD_DEST_SHORT_SAMPLE : wa::MOD_DEST_SHORT);
		for (ModeButton* d : destButtons) {
			if (d)
				d->labels.assign(destNames, destNames + wa::NUM_MOD_DESTS);
		}
		ModuleWidget::step();
	}

	WaveArtifactExpanderWidget(WaveArtifactExpander* module) {
		using namespace xlayout;
		setModule(module);
		setPanel(createPanel(
			asset::plugin(pluginInstance, "res/waveArtifactExpander.svg"),
			asset::plugin(pluginInstance, "res/waveArtifactExpander-dark.svg")
		));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		PanelLabels* labels = createWidget<PanelLabels>(Vec(0, 0));
		labels->box.size = box.size;
		labels->add(Vec(W / 2.f, 6.f), "wA EXPANDER", 9.f);
		labels->add(Vec(MARKER_BUTTON.x, TOP_LABEL_Y), "MARKER", 6.f);
		labels->add(Vec(VOCT_JACK.x, TOP_LABEL_Y + 1.f), "V/OCT", 6.f);
		labels->add(Vec(GATE_JACK.x, TOP_LABEL_Y + 1.f), "GATE", 6.f);
		labels->add(Vec(ATTACK_X, AD_NAME_Y), "ATTACK", 5.f);
		labels->add(Vec(DECAY_X, AD_NAME_Y), "DECAY", 5.f);
		labels->add(Vec(AD_DEST_BUTTON.x, AD_NAME_Y), "AD DEST", 5.f);
		labels->add(Vec(AD_DEPTH_X, AD_NAME_Y), "DEPTH", 5.f);
		labels->add(Vec(LFO_X, LFO_LABEL_Y), "LFO", 6.f);
		labels->add(Vec(LFO_X - LFO_KNOB_DX, LFO_KNOB_LABEL_Y), "RATE", 6.f);
		labels->add(Vec(LFO_X + LFO_KNOB_DX, LFO_KNOB_LABEL_Y), "AMT", 6.f);
		labels->add(Vec(LFO_DEST_X, LFO_DEST_LABEL_Y), "LFO DEST", 5.f);
		labels->add(Vec(LFO_DEST_X, LFO_KNOB_LABEL_Y), "DEPTH", 6.f);
		addChild(labels);

		ModeButton* marker = createParam<ModeButton>(Vec(0, 0), module, WaveArtifactExpander::MARKER_PARAM);
		marker->box.size = mm2px(MARKER_BUTTON_SIZE);
		marker->box.pos = mm2px(MARKER_BUTTON).minus(marker->box.size.div(2.f));
		marker->labels = {"1", "2", "3", "4", "5"};
		marker->colors.assign(layout::MARKER_COLORS, layout::MARKER_COLORS + wa::NUM_MARKERS);
		addParam(marker);
		addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(LINK_LIGHT_POS), module, WaveArtifactExpander::LINK_LIGHT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(VOCT_JACK), module, WaveArtifactExpander::VOCT_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(GATE_JACK), module, WaveArtifactExpander::GATE_INPUT));

		for (int s = 0; s < wa::NUM_CV_SLOTS; s++) {
			float cellX = CV_CELL_X[s % 2];
			float y = CV_JACK_Y[s / 2];
			addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(cellX + CV_JACK_DX, y)), module, WaveArtifactExpander::MOD_INPUT + s));
			for (int e = 0; e < wa::NUM_CV_ENGINES; e++) {
				CvDestButton* d = createParam<CvDestButton>(Vec(0, 0), module, WaveArtifactExpander::destParam(e, s));
				d->engine = e;
				d->box.size = mm2px(CV_DEST_SIZE);
				d->box.pos = mm2px(Vec(cellX + CV_NAME_DX, y + CV_NAME_DY)).minus(d->box.size.div(2.f));
				for (int k = 0; k < wa::NUM_CV_DESTS; k++)
					d->labels.push_back(wa::cvDestName(k, e));
				cvDests[e][s] = d;
				addParam(d);
				cvAmts[e][s] = createParamCentered<Trimpot>(mm2px(Vec(cellX + CV_AMT_DX, y)), module, WaveArtifactExpander::amtParam(e, s));
				addParam(cvAmts[e][s]);
			}
		}

		// The marker's modulation: main module params, bound to the marker in step(). Created without a module.
		NVGcolor c = layout::SCREEN_TEXT;
		auto dest = [&](Vec at) {
			ModeButton* d = createParam<ModeButton>(Vec(0, 0), nullptr, 0);
			d->box.size = mm2px(DEST_BUTTON_SIZE);
			d->box.pos = mm2px(at).minus(d->box.size.div(2.f));
			d->labels.assign(wa::MOD_DEST_SHORT, wa::MOD_DEST_SHORT + wa::NUM_MOD_DESTS);
			return d;
		};
		addParam(bind(createParamCentered<LinkedKnob<Trimpot>>(mm2px(Vec(ATTACK_X, AD_Y)), nullptr, 0), WaveArtifact::MARKER_MOD_AD_ATTACK_PARAM));
		addParam(bind(createParamCentered<LinkedKnob<Trimpot>>(mm2px(Vec(DECAY_X, AD_Y)), nullptr, 0), WaveArtifact::MARKER_MOD_AD_DECAY_PARAM));
		addParam(bind(createLitToggle(PERC_BUTTON, nullptr, 0, "PERC", c, PERC_BUTTON_SIZE), WaveArtifact::MARKER_MOD_AD_PERC_PARAM));
		destButtons[0] = bind(dest(AD_DEST_BUTTON), WaveArtifact::MARKER_MOD_AD_DEST_PARAM);
		addParam(destButtons[0]);
		addParam(bind(createParamCentered<LinkedKnob<Trimpot>>(mm2px(Vec(AD_DEPTH_X, AD_Y)), nullptr, 0), WaveArtifact::MARKER_MOD_AD_DEPTH_PARAM));

		addParam(bind(createLfoShapeScreen(Vec(LFO_X, LFO_SCREEN_Y), LFO_SCREEN_SIZE, nullptr, 0, c), WaveArtifact::MARKER_MOD_LFO_SHAPE_PARAM));
		rateKnob = createLfoRateKnob<LinkedKnob<LfoRateKnobT<RoundSmallBlackKnob>>>(Vec(LFO_X - LFO_KNOB_DX, LFO_KNOB_Y), nullptr,
			WaveArtifact::MARKER_MOD_LFO_RATE_PARAM, WaveArtifact::MARKER_MOD_LFO_SYNC_PARAM,
			WaveArtifact::MARKER_MOD_LFO_TRIPLET_PARAM, WaveArtifact::MARKER_MOD_LFO_DOTTED_PARAM);
		addParam(bind(rateKnob, WaveArtifact::MARKER_MOD_LFO_RATE_PARAM));
		// The jack's level: this expander's own
		addParam(createParamCentered<Trimpot>(mm2px(Vec(LFO_X + LFO_KNOB_DX, LFO_KNOB_Y)), module, WaveArtifactExpander::LFO_AMT_PARAM + 0));
		// SYNC (wide), then the narrow T and D, centered as a group under the shape screen.
		addParam(bind(createLitToggle(Vec(LFO_X - 4.7f, LFO_BUTTON_Y), nullptr, 0, "SYNC", c), WaveArtifact::MARKER_MOD_LFO_SYNC_PARAM));
		addParam(bind(createLitToggle(Vec(LFO_X + 1.3f, LFO_BUTTON_Y), nullptr, 0, "T", c, layout::LFO_TOGGLE_SMALL), WaveArtifact::MARKER_MOD_LFO_TRIPLET_PARAM));
		addParam(bind(createLitToggle(Vec(LFO_X + 6.f, LFO_BUTTON_Y), nullptr, 0, "D", c, layout::LFO_TOGGLE_SMALL), WaveArtifact::MARKER_MOD_LFO_DOTTED_PARAM));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(LFO_X, LFO_OUT_Y)), module, WaveArtifactExpander::LFO_OUTPUT + 0));
		addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(Vec(LFO_X + 8.6f, LFO_LABEL_Y)), module, WaveArtifactExpander::LFO_LIGHT));
		destButtons[1] = bind(dest(Vec(LFO_DEST_X, LFO_DEST_Y)), WaveArtifact::MARKER_MOD_LFO_DEST_PARAM);
		addParam(destButtons[1]);
		addParam(bind(createParamCentered<LinkedKnob<Trimpot>>(mm2px(Vec(LFO_DEST_X, LFO_KNOB_Y)), nullptr, 0), WaveArtifact::MARKER_MOD_LFO_DEPTH_PARAM));

		// Trig locks, as on the main module: a dot above each control some step of the marker's row locks (white
		// where the selected step does), and while a step is selected a bar under each with the value it plays
		stepBars = createWidget<StepValueBars>(Vec(0, 0));
		stepBars->box.size = box.size;
		for (Bound& b : bound)
			stepBars->controls.push_back(b.widget);
		addChild(stepBars);
	}
};

Model* modelWaveArtifactExpander = createModel<WaveArtifactExpander, WaveArtifactExpanderWidget>("waveArtifactExpander");
