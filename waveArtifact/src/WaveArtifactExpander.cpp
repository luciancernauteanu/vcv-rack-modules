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
	for (int t = 0; t < wa::NUM_MOD_TARGETS; t++) {
		std::string name = wa::MOD_TARGET_NAMES[t];
		// GAIN (a VCA), CUTOFF and DRIVE are unipolar: 0..100%. WAVE, FINE and PAN are
		// attenuverters. All amounts start at 0 (double-click resets to 0).
		bool unipolar = (t == wa::MOD_GAIN || t == wa::MOD_CUTOFF || t == wa::MOD_DRIVE);
		configParam(AMT_PARAM + t, unipolar ? 0.f : -1.f, 1.f, 0.f, name + ((t == wa::MOD_GAIN) ? " VCA amount (100% = silent at 0 V)" : " CV amount"), "%", 0.f, 100.f);
		configInput(MOD_INPUT + t, name + " CV");
	}
	// Granular engine: START and DENSITY are unipolar amounts, shown in place of the attenuverters.
	configParam(GRAIN_START_AMT_PARAM, 0.f, 1.f, 0.f, "START CV amount", "%", 0.f, 100.f);
	configParam(GRAIN_DENSITY_AMT_PARAM, 0.f, 1.f, 0.f, "DENSITY CV amount", "%", 0.f, 100.f);
	configParam(GRAIN_SPREAD_AMT_PARAM, 0.f, 1.f, 0.f, "Unused", "%", 0.f, 100.f);
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
		for (int t = 0; t < wa::NUM_MOD_TARGETS; t++) {
			if (!inputs[MOD_INPUT + t].isConnected())
				continue;
			float amt = params[amtParam(t, fromLeft.granular)].getValue();
			if (amt != 0.f)
				acc.modulated[m] = true;
			float v = inputs[MOD_INPUT + t].getVoltage();
			if (t == wa::MOD_GAIN)
				acc.gainMul[m] *= wa::vcaLevel(amt, v);
			else
				acc.mod[m][t] += amt * v;
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
		granular = fromLeft.linked && fromLeft.granular;
		marker = getMarker();
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
// CV section: two columns of three cells, each a name above a CV jack with its AMT trimpot
// beside it (WAVE GAIN / CUTOFF DRIVE / FINE PAN).
static const float CV_CELL_X[2] = {4.f, 25.4f};
static const float CV_JACK_DX = 6.2f;
static const float CV_AMT_DX = 16.4f;
static const float CV_NAME_DX = 11.3f;
static const float CV_JACK_Y[3] = {49.4f, 61.4f, 73.4f};
static const float CV_NAME_DY = -5.6f;
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

struct WaveArtifactExpanderWidget : ModuleWidget {
	/** Per CV input with a separate granular amount: the wavetable knob and the granular knob at the same spot */
	ParamWidget* waveAmtKnobs[wa::NUM_MOD_TARGETS] = {};
	ParamWidget* grainAmtKnobs[wa::NUM_MOD_TARGETS] = {};

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
		bool grain = m && m->granular.load(std::memory_order_relaxed);
		for (int t = 0; t < wa::NUM_MOD_TARGETS; t++) {
			if (!grainAmtKnobs[t])
				continue;
			waveAmtKnobs[t]->setVisible(!grain);
			grainAmtKnobs[t]->setVisible(grain);
		}
		WaveArtifact* main = (m && m->linked.load(std::memory_order_relaxed)) ? mainOf(m) : nullptr;
		int marker = m ? clamp(m->marker.load(std::memory_order_relaxed), 0, wa::NUM_MARKERS - 1) : 0;
		if (main != boundMain || marker != boundMarker)
			rebind(main, marker);
		for (ModeButton* d : destButtons) {
			if (d)
				d->labels.assign(grain ? wa::MOD_DEST_SHORT_GRAIN : wa::MOD_DEST_SHORT, (grain ? wa::MOD_DEST_SHORT_GRAIN : wa::MOD_DEST_SHORT) + wa::NUM_MOD_DESTS);
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
		// The same six inputs under their granular names while the main module runs the granular engine
		static const char* const GRAIN_TARGET_NAMES[wa::NUM_MOD_TARGETS] = {"START", "GAIN", "CUTOFF", "P.SPRD", "DENSITY", "PAN"};
		for (int t = 0; t < wa::NUM_MOD_TARGETS; t++)
			labels->addModal(Vec(CV_CELL_X[t % 2] + CV_NAME_DX, CV_JACK_Y[t / 2] + CV_NAME_DY), wa::MOD_TARGET_NAMES[t], GRAIN_TARGET_NAMES[t], 6.f);
		labels->grainFlag = module ? &module->granular : nullptr;
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

		for (int t = 0; t < wa::NUM_MOD_TARGETS; t++) {
			float cellX = CV_CELL_X[t % 2];
			float y = CV_JACK_Y[t / 2];
			addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(cellX + CV_JACK_DX, y)), module, WaveArtifactExpander::MOD_INPUT + t));
			ParamWidget* amt = createParamCentered<Trimpot>(mm2px(Vec(cellX + CV_AMT_DX, y)), module, WaveArtifactExpander::AMT_PARAM + t);
			addParam(amt);
			int grainId = WaveArtifactExpander::amtParam(t, true);
			if (grainId != WaveArtifactExpander::AMT_PARAM + t) {
				waveAmtKnobs[t] = amt;
				grainAmtKnobs[t] = createParamCentered<Trimpot>(mm2px(Vec(cellX + CV_AMT_DX, y)), module, grainId);
				grainAmtKnobs[t]->setVisible(false);
				addParam(grainAmtKnobs[t]);
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
