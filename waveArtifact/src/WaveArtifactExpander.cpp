#include "WaveArtifactExpander.hpp"
#include "Layout.hpp"
#include "widgets/LfoControls.hpp"
#include "widgets/LfoWidgets.hpp"
#include "widgets/ModeButton.hpp"
#include "widgets/PanelLabels.hpp"

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
	// LFO 1 defaults to 1 Hz, LFO 2 to 0.2 Hz.
	const float defaultRate[NUM_LFOS] = {0.606f, 0.394f};
	for (int k = 0; k < NUM_LFOS; k++) {
		std::string name = "LFO " + std::to_string(k + 1);
		configLfoRate(this, LFO_RATE_PARAM + k, LFO_SYNC_PARAM + k, LFO_TRIPLET_PARAM + k, LFO_DOTTED_PARAM + k, name + " rate", defaultRate[k]);
		configSwitch(LFO_SYNC_PARAM + k, 0.f, 1.f, 1.f, name + " sync", {"Free (Hz)", "Tempo sync (BPM / CLOCK)"});
		configSwitch(LFO_TRIPLET_PARAM + k, 0.f, 1.f, 0.f, name + " triplets", {"Off", "On"});
		configSwitch(LFO_DOTTED_PARAM + k, 0.f, 1.f, 0.f, name + " dotted", {"Off", "On"});
		for (int id : {LFO_SYNC_PARAM, LFO_TRIPLET_PARAM, LFO_DOTTED_PARAM})
			getParamQuantity(id + k)->randomizeEnabled = false;
		configSwitch(LFO_SHAPE_PARAM + k, 0.f, wa::NUM_LFO_SHAPES - 1, 0.f, name + " shape", std::vector<std::string>(wa::LFO_SHAPE_NAMES, wa::LFO_SHAPE_NAMES + wa::NUM_LFO_SHAPES));
		getParamQuantity(LFO_SHAPE_PARAM + k)->randomizeEnabled = false;
		// The output level. Unlike the CV amounts (which start at 0 so nothing is modulated until you ask),
		// this starts at 100%: an LFO output that starts silent would look broken.
		configParam(LFO_AMT_PARAM + k, 0.f, 1.f, 1.f, name + " amount (output level)", "%", 0.f, 100.f);
		lfos[k].seed(random::u32());
		configOutput(LFO_OUTPUT + k, name + " (+-5 V)");
	}
	configInput(VOCT_INPUT, "V/OCT for this artifact (1 V/oct, added to the main V/OCT)");
	configInput(GATE_INPUT, "Gate for this artifact (patched: it sounds only while the gate is high, shaped by ATTACK / DECAY)");
	// Attack 5 ms..2 s, decay 5 ms..4 s (log). At the minimum a stage is instant (plain gate).
	configParam(ATTACK_PARAM, 0.f, 1.f, 0.f, "Attack", " s", 2.f / MIN_ENV_TIME, MIN_ENV_TIME);
	configParam(DECAY_PARAM, 0.f, 1.f, 0.f, "Decay", " s", 4.f / MIN_ENV_TIME, MIN_ENV_TIME);
	configSwitch(PERC_PARAM, 0.f, 1.f, 0.f, "Envelope mode", {"Gate (holds while high, DECAY after it falls)", "Percussive AD (decays even while the gate is held)"});
	getParamQuantity(PERC_PARAM)->randomizeEnabled = false;
	configOutput(ENV_OUTPUT, "Gate envelope (0..10 V; 0 V without a gate)");
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

/** Gate envelope: 0..1. ATTACK rises linearly; DECAY falls exponentially (60 dB in the DECAY time).
At the minimum (5 ms or less) a stage is instant and the main module's 5 ms gain smoothing
declicks it, so the defaults behave exactly like a plain gate. In gate mode the level holds while
the gate is high and DECAY starts when it falls; in PERC mode a gate edge runs attack then decay
whatever the gate does. A gate patched while already high starts the envelope (gate mode). */
float WaveArtifactExpander::stepEnvelope(bool gateHigh, bool risingEdge, float sampleTime) {
	bool perc = params[PERC_PARAM].getValue() > 0.5f;
	float attack = envTime(params[ATTACK_PARAM].getValue(), 2.f);
	float decay = envTime(params[DECAY_PARAM].getValue(), 4.f);
	const float instant = MIN_ENV_TIME * 1.05f;
	if (risingEdge || (gateHigh && !perc && envStage == ENV_IDLE))
		envStage = ENV_ATTACK;

	if (envStage == ENV_ATTACK) {
		if (!perc && !gateHigh) {
			envStage = ENV_DECAY;
		}
		else {
			env = (attack <= instant) ? 1.f : std::min(1.f, env + sampleTime / attack);
			if (env >= 1.f) {
				env = 1.f;
				// PERC starts falling next sample, so an instant attack still reaches the output.
				envStage = perc ? ENV_DECAY : ENV_HOLD;
				return env;
			}
		}
	}
	else if (envStage == ENV_HOLD && !gateHigh) {
		envStage = ENV_DECAY;
	}

	if (envStage == ENV_DECAY) {
		if (decay <= instant) {
			env = 0.f;
			envStage = ENV_IDLE;
		}
		else {
			env *= std::exp(-6.9078f * sampleTime / decay);
			if (env < 1e-3f) {
				env = 0.f;
				envStage = ENV_IDLE;
			}
		}
	}
	return env;
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
	bool gateRising = false;
	if (gatePatched)
		gateRising = gateTrigger.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 1.f);
	else
		gateTrigger.reset();
	float envLevel = stepEnvelope(gatePatched && gateTrigger.isHigh(), gateRising, args.sampleTime);
	outputs[ENV_OUTPUT].setVoltage(10.f * envLevel);

	// Modulation: what the expanders to our right sent, plus ours.
	wa::ToMainMessage acc;
	if (rightIsExp)
		acc = *(const wa::ToMainMessage*) rightExpander.consumerMessage;
	if (active) {
		int m = getMarker();
		// A patched GATE shapes the artifact's level with the ATTACK / DECAY envelope, through the
		// same multiplier as the GAIN VCA, so the main module's 5 ms gain smoothing keeps even the
		// fastest open/close click-free. Two expanders gating the same artifact multiply.
		if (gatePatched) {
			acc.gainMul[m] *= envLevel;
			acc.modulated[m] = true;
		}
		if (inputs[VOCT_INPUT].isConnected()) {
			acc.voct[m] += inputs[VOCT_INPUT].getVoltage();
			acc.modulated[m] = true;
		}
		for (int t = 0; t < wa::NUM_MOD_TARGETS; t++) {
			if (!inputs[MOD_INPUT + t].isConnected())
				continue;
			float amt = params[AMT_PARAM + t].getValue();
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
		right->leftExpander.requestMessageFlip();
	}

	// RESET from the main module restarts these LFOs too.
	if (fromLeft.linked && fromLeft.reset) {
		for (int k = 0; k < NUM_LFOS; k++)
			lfos[k].reset();
	}
	// LFOs run whether or not the expander is linked; unlinked, sync falls back to 120 BPM.
	const wa::ClockTracker& clock = fromLeft.linked ? fromLeft.clock : idleClock;
	float lfoOut[NUM_LFOS];
	for (int k = 0; k < NUM_LFOS; k++) {
		bool sync = params[LFO_SYNC_PARAM + k].getValue() > 0.5f;
		bool triplets = params[LFO_TRIPLET_PARAM + k].getValue() > 0.5f;
		bool dotted = params[LFO_DOTTED_PARAM + k].getValue() > 0.5f;
		int shape = (int) std::round(params[LFO_SHAPE_PARAM + k].getValue());
		lfoOut[k] = lfos[k].process(shape, sync, params[LFO_RATE_PARAM + k].getValue(), triplets, dotted, clock, args.sampleTime);
		lfoOut[k] *= params[LFO_AMT_PARAM + k].getValue();
		outputs[LFO_OUTPUT + k].setVoltage(5.f * lfoOut[k]);
	}

	if (lightDivider.process()) {
		float dt = args.sampleTime * lightDivider.getDivision();
		lights[LINK_LIGHT + 0].setBrightnessSmooth(active ? 1.f : 0.f, dt);
		lights[LINK_LIGHT + 1].setBrightnessSmooth(fromLeft.linked && !active ? 1.f : 0.f, dt);
		for (int k = 0; k < NUM_LFOS; k++) {
			lights[LFO_LIGHT + 2 * k + 0].setBrightnessSmooth(std::max(lfoOut[k], 0.f), dt);
			lights[LFO_LIGHT + 2 * k + 1].setBrightnessSmooth(std::max(-lfoOut[k], 0.f), dt);
		}
		linked = active;
		chainIndex = index;
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
// Order top to bottom: header (MARKER, V/OCT, GATE), envelope, CV grid, LFOs.
// CV section: two columns of three cells, each a name above a CV jack with its AMT trimpot
// beside it (WAVE GAIN / CUTOFF DRIVE / FINE PAN).
static const float CV_CELL_X[2] = {4.f, 25.4f};
static const float CV_JACK_DX = 6.2f;
static const float CV_AMT_DX = 16.4f;
static const float CV_NAME_DX = 11.3f;
static const float CV_JACK_Y[3] = {49.4f, 61.4f, 73.4f};
static const float CV_NAME_DY = -5.6f;
// Envelope section (gate): names above ATTACK, DECAY and the ENV output; PERC button between.
static const float ENV_NAME_Y = 28.4f;
static const float ENV_Y = 34.2f;
static const float ATTACK_X = 10.5f;
static const float DECAY_X = 21.2f;
static const Vec PERC_BUTTON = Vec(30.4f, 34.2f);
static const Vec PERC_BUTTON_SIZE = Vec(8.f, 4.4f);
static const float ENV_OUT_X = 41.4f;
// Per LFO column, top to bottom: title (+ light), shape screen, SYNC / T / D centered under the
// screen, RATE + AMT with their labels above (as on the main module), output.
static const float LFO_X[2] = {14.8f, 36.2f};
static const float LFO_LABEL_Y = 83.2f;
static const float LFO_SCREEN_Y = 88.8f;
static const Vec LFO_SCREEN_SIZE = Vec(16.f, 6.4f);
static const float LFO_BUTTON_Y = 95.2f;
static const float LFO_KNOB_LABEL_Y = 100.f;
static const float LFO_KNOB_Y = 104.4f;
static const float LFO_KNOB_DX = 5.1f;
static const float LFO_OUT_Y = 114.6f;
} // namespace xlayout

struct WaveArtifactExpanderWidget : ModuleWidget {
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
		for (int t = 0; t < wa::NUM_MOD_TARGETS; t++)
			labels->add(Vec(CV_CELL_X[t % 2] + CV_NAME_DX, CV_JACK_Y[t / 2] + CV_NAME_DY), wa::MOD_TARGET_NAMES[t], 6.f);
		labels->add(Vec(ATTACK_X, ENV_NAME_Y), "ATTACK", 6.f);
		labels->add(Vec(DECAY_X, ENV_NAME_Y), "DECAY", 6.f);
		labels->add(Vec(ENV_OUT_X, ENV_NAME_Y), "ENV", 6.f);
		for (int k = 0; k < WaveArtifactExpander::NUM_LFOS; k++)
		{
			labels->add(Vec(LFO_X[k], LFO_LABEL_Y), "LFO " + std::to_string(k + 1), 6.f);
			labels->add(Vec(LFO_X[k] - LFO_KNOB_DX, LFO_KNOB_LABEL_Y), "RATE", 6.f);
			labels->add(Vec(LFO_X[k] + LFO_KNOB_DX, LFO_KNOB_LABEL_Y), "AMT", 6.f);
		}
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
			addParam(createParamCentered<Trimpot>(mm2px(Vec(cellX + CV_AMT_DX, y)), module, WaveArtifactExpander::AMT_PARAM + t));
		}

		addParam(createParamCentered<Trimpot>(mm2px(Vec(ATTACK_X, ENV_Y)), module, WaveArtifactExpander::ATTACK_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(DECAY_X, ENV_Y)), module, WaveArtifactExpander::DECAY_PARAM));
		addParam(createLitToggle(PERC_BUTTON, module, WaveArtifactExpander::PERC_PARAM, "PERC", layout::SCREEN_TEXT, PERC_BUTTON_SIZE));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(ENV_OUT_X, ENV_Y)), module, WaveArtifactExpander::ENV_OUTPUT));

		for (int k = 0; k < WaveArtifactExpander::NUM_LFOS; k++) {
			float x = LFO_X[k];
			NVGcolor c = layout::SCREEN_TEXT;
			addParam(createLfoShapeScreen(Vec(x, LFO_SCREEN_Y), LFO_SCREEN_SIZE, module, WaveArtifactExpander::LFO_SHAPE_PARAM + k, c));
			addParam(createLfoRateKnob<LfoRateKnobT<RoundSmallBlackKnob>>(Vec(x - LFO_KNOB_DX, LFO_KNOB_Y), module,
				WaveArtifactExpander::LFO_RATE_PARAM + k, WaveArtifactExpander::LFO_SYNC_PARAM + k,
				WaveArtifactExpander::LFO_TRIPLET_PARAM + k, WaveArtifactExpander::LFO_DOTTED_PARAM + k));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(x + LFO_KNOB_DX, LFO_KNOB_Y)), module, WaveArtifactExpander::LFO_AMT_PARAM + k));
			// SYNC (wide), then the narrow T and D, centered as a group under the shape screen.
			addParam(createLitToggle(Vec(x - 4.7f, LFO_BUTTON_Y), module, WaveArtifactExpander::LFO_SYNC_PARAM + k, "SYNC", c));
			addParam(createLitToggle(Vec(x + 1.3f, LFO_BUTTON_Y), module, WaveArtifactExpander::LFO_TRIPLET_PARAM + k, "T", c, layout::LFO_TOGGLE_SMALL));
			addParam(createLitToggle(Vec(x + 6.f, LFO_BUTTON_Y), module, WaveArtifactExpander::LFO_DOTTED_PARAM + k, "D", c, layout::LFO_TOGGLE_SMALL));
			addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(x, LFO_OUT_Y)), module, WaveArtifactExpander::LFO_OUTPUT + k));
			addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(Vec(x + 8.6f, LFO_LABEL_Y)), module, WaveArtifactExpander::LFO_LIGHT + 2 * k));
		}
	}
};

Model* modelWaveArtifactExpander = createModel<WaveArtifactExpander, WaveArtifactExpanderWidget>("waveArtifactExpander");
