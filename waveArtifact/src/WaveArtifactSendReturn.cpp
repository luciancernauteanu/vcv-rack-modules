// wA SEND / RETURN: each artifact's send and return on separate L and R jacks, for stereo effects that take two mono
// cables (most reverbs and delays). It goes at the end of the expander chain (right of the last expander, or right of
// Wave Artifacts when there is none). What it does is the main panel's SEND / RETURN, on four jacks per artifact:
// - SEND L / R: the artifact's sound after its DRIVE and filter (type, CUTOFF, RES). SEND R unpatched: SEND L carries the whole voice (L + R).
// - RET L / R: come back in after the artifact's filter (the same 20 ms crossfade and RET tag). RET R unpatched: RET L feeds both
//   sides. Patched here, the return wins over the main panel's RETURN for that artifact.
// The audio goes through the expanders' messages: a few samples late (one per module on the way, each way).
// Each row also has the artifact's SEND level knob and its INSERT / BUS button: they are the main module's params
// (MARKER_SEND_LEVEL_PARAM, lockable per trig, and MARKER_RETURN_MODE_PARAM), bound once this module finds it.
#include "plugin.hpp"
#include "ExpanderMessages.hpp"
#include "Layout.hpp"
#include "WaveArtifact.hpp"
#include "WaveArtifactExpander.hpp"
#include "widgets/LinkedKnob.hpp"
#include "widgets/ModeButton.hpp"
#include "widgets/PanelLabels.hpp"

struct WaveArtifactSendReturn : Module {
	static const int ROWS = wa::NUM_MARKERS;
	enum ParamId {
		PARAMS_LEN
	};
	enum InputId {
		ENUMS(RET_L_INPUT, ROWS),
		ENUMS(RET_R_INPUT, ROWS),
		INPUTS_LEN
	};
	enum OutputId {
		ENUMS(SEND_L_OUTPUT, ROWS),
		ENUMS(SEND_R_OUTPUT, ROWS),
		OUTPUTS_LEN
	};
	enum LightId {
		LINK_LIGHT,
		LIGHTS_LEN
	};

	/** Linked to Wave Artifacts (directly or through expanders), for the panel */
	bool linked = false;
	dsp::ClockDivider lightDivider;

	WaveArtifactSendReturn() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < ROWS; i++) {
			std::string name = "Artifact " + std::to_string(i + 1);
			configOutput(SEND_L_OUTPUT + i, name + " send L (what its filter gets; L + R while SEND R is unpatched)");
			configOutput(SEND_R_OUTPUT + i, name + " send R");
			configInput(RET_L_INPUT + i, name + " return L (comes back after its filter; feeds both sides while RET R is unpatched)");
			configInput(RET_R_INPUT + i, name + " return R");
		}
		configLight(LINK_LIGHT, "Connected to Wave Artifacts");
		leftExpander.producerMessage = new wa::ToExpanderMessage;
		leftExpander.consumerMessage = new wa::ToExpanderMessage;
		lightDivider.setDivision(256);
	}
	~WaveArtifactSendReturn() {
		delete (wa::ToExpanderMessage*) leftExpander.producerMessage;
		delete (wa::ToExpanderMessage*) leftExpander.consumerMessage;
	}

	static bool isMain(Module* m) {
		return m && m->model == modelWaveArtifact;
	}

	void process(const ProcessArgs& args) override {
		Module* left = leftExpander.module;
		bool leftOk = isMain(left) || isWaveArtifactExpander(left);
		const wa::ToExpanderMessage* in = leftOk ? (const wa::ToExpanderMessage*) leftExpander.consumerMessage : nullptr;
		bool on = in && in->linked;
		wa::ToMainMessage out;
		for (int i = 0; i < ROWS; i++) {
			float sl = on ? in->sendL[i] : 0.f, sr = on ? in->sendR[i] : 0.f;
			Output& sendL = outputs[SEND_L_OUTPUT + i];
			Output& sendR = outputs[SEND_R_OUTPUT + i];
			if (sendR.isConnected()) {
				sendL.setVoltage(5.f * sl);
				sendR.setVoltage(5.f * sr);
			}
			else {
				// A mono effect on SEND L gets the whole voice
				sendL.setVoltage(2.5f * (sl + sr));
				sendR.setVoltage(0.f);
			}
			out.stereoSendWanted = out.stereoSendWanted || sendL.isConnected() || sendR.isConnected();
			Input& retL = inputs[RET_L_INPUT + i];
			Input& retR = inputs[RET_R_INPUT + i];
			if (retL.isConnected()) {
				out.stereoReturn[i] = true;
				out.stereoRetL[i] = retL.getVoltage();
				out.stereoRetR[i] = retR.isConnected() ? retR.getVoltage() : out.stereoRetL[i];
			}
		}
		if (leftOk) {
			*(wa::ToMainMessage*) left->rightExpander.producerMessage = out;
			left->rightExpander.requestMessageFlip();
		}
		if (lightDivider.process()) {
			linked = on;
			lights[LINK_LIGHT].setBrightnessSmooth(on ? 1.f : 0.f, args.sampleTime * lightDivider.getDivision());
		}
	}
};

bool isWaveArtifactSendReturn(Module* m) {
	return m && m->model == modelWaveArtifactSendReturn;
}

namespace srlayout {
static const float W = 50.8f;
/** Columns: the artifact's number, SEND L, SEND R (on the dark output box), RET L, RET R */
static const float NUM_X = 5.2f;
static const float SEND_L_X = 14.2f, SEND_R_X = 24.0f, RET_L_X = 35.0f, RET_R_X = 44.8f;
static const float HEAD_Y = 16.f;
static const float ROW_Y0 = 26.f, ROW_DY = 17.f;
inline float rowY(int r) {
	return ROW_Y0 + r * ROW_DY;
}
/** Each row's second line: its SEND knob (under the SEND jacks) and its INSERT / BUS button (under the RETURN jacks) */
static const float CTRL_DY = 8.3f;
static const float SEND_KNOB_X = (SEND_L_X + SEND_R_X) / 2.f, MODE_X = (RET_L_X + RET_R_X) / 2.f;
} // namespace srlayout

/** Each row's number in its artifact's color, and a dim hint while not linked */
struct SendReturnRowNumbers : widget::TransparentWidget {
	WaveArtifactSendReturn* module = nullptr;
	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		NVGcontext* vg = args.vg;
		float mm = SVG_DPI / MM_PER_IN;
		nvgFontFaceId(vg, font->handle);
		for (int r = 0; r < WaveArtifactSendReturn::ROWS; r++) {
			float x = srlayout::NUM_X * mm, y = srlayout::rowY(r) * mm;
			nvgBeginPath(vg);
			nvgCircle(vg, x, y, 2.4f * mm);
			nvgFillColor(vg, layout::MARKER_COLORS[r]);
			nvgFill(vg);
			nvgFontSize(vg, 9.f);
			nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgFillColor(vg, nvgRGB(0x16, 0x18, 0x22));
			nvgText(vg, x, y + 0.5f, std::to_string(r + 1).c_str(), NULL);
		}
		if (module && !module->linked) {
			nvgFontSize(vg, 6.5f);
			nvgFillColor(vg, nvgRGB(0xd0, 0x60, 0x50));
			nvgText(vg, srlayout::W / 2.f * mm, 119.6f * mm, "snap it right of Wave Artifacts", NULL);
			nvgText(vg, srlayout::W / 2.f * mm, 122.4f * mm, "or of its last expander", NULL);
		}
	}
};

struct WaveArtifactSendReturnWidget : ModuleWidget {
	WaveArtifactSendReturnWidget(WaveArtifactSendReturn* module) {
		using namespace srlayout;
		setModule(module);
		setPanel(createPanel(
			asset::plugin(pluginInstance, "res/waveArtifactSendReturn.svg"),
			asset::plugin(pluginInstance, "res/waveArtifactSendReturn-dark.svg")
		));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		PanelLabels* labels = createWidget<PanelLabels>(Vec(0, 0));
		labels->box.size = box.size;
		labels->add(Vec(W / 2.f, 6.f), "wA SEND / RETURN", 8.f);
		labels->add(Vec((SEND_L_X + SEND_R_X) / 2.f, HEAD_Y - 3.6f), "SEND", 6.f, true);
		labels->add(Vec(SEND_L_X, HEAD_Y), "L", 6.f, true);
		labels->add(Vec(SEND_R_X, HEAD_Y), "R", 6.f, true);
		labels->add(Vec((RET_L_X + RET_R_X) / 2.f, HEAD_Y - 3.6f), "RETURN", 6.f);
		labels->add(Vec(RET_L_X, HEAD_Y), "L", 6.f);
		labels->add(Vec(RET_R_X, HEAD_Y), "R", 6.f);
		labels->add(Vec(W / 2.f, 110.5f), "knob: SEND level in BUS (lock it per trig)", 5.f);
		labels->add(Vec(W / 2.f, 113.3f), "INS: in its notes  BUS: rings out", 5.f);
		labels->add(Vec(W / 2.f, 116.1f), "only L patched: mono", 5.f);
		addChild(labels);

		SendReturnRowNumbers* numbers = createWidget<SendReturnRowNumbers>(Vec(0, 0));
		numbers->box.size = box.size;
		numbers->module = module;
		addChild(numbers);

		for (int r = 0; r < WaveArtifactSendReturn::ROWS; r++) {
			float y = rowY(r);
			addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(SEND_L_X, y)), module, WaveArtifactSendReturn::SEND_L_OUTPUT + r));
			addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(SEND_R_X, y)), module, WaveArtifactSendReturn::SEND_R_OUTPUT + r));
			addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(RET_L_X, y)), module, WaveArtifactSendReturn::RET_L_INPUT + r));
			addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(RET_R_X, y)), module, WaveArtifactSendReturn::RET_R_INPUT + r));
			// The main module's params for this artifact, bound in step() (hidden until then)
			// A LinkedKnob, as on the main panel: with a trig selected its drag moves that trig's lock directly
			LinkedKnob<Trimpot>* k = createParamCentered<LinkedKnob<Trimpot>>(mm2px(Vec(SEND_KNOB_X, y + CTRL_DY)), nullptr, 0);
			k->setVisible(false);
			addChild(k);
			sendKnobs[r] = k;
			ModeButton* b = new ModeButton;
			b->box.size = mm2px(Vec(10.f, 4.4f));
			b->box.pos = mm2px(Vec(MODE_X, y + CTRL_DY)).minus(b->box.size.div(2.f));
			b->labels = {"INS", "BUS"};
			b->colors = {layout::SCREEN_TEXT, nvgRGB(0x8f, 0xe3, 0xb0)};
			b->setVisible(false);
			addChild(b);
			modeButtons[r] = b;
		}
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(W - 6.3f, 6.f)), module, WaveArtifactSendReturn::LINK_LIGHT));
	}

	ParamWidget* sendKnobs[WaveArtifactSendReturn::ROWS] = {};
	ParamWidget* modeButtons[WaveArtifactSendReturn::ROWS] = {};

	/** Wave Artifacts on the left, directly or through expanders, or null */
	Module* mainOnLeft() {
		Module* m = module ? module->leftExpander.module : nullptr;
		for (int n = 0; m && n <= wa::MAX_EXPANDERS; n++) {
			if (m->model == modelWaveArtifact)
				return m;
			if (!isWaveArtifactExpander(m))
				return nullptr;
			m = m->leftExpander.module;
		}
		return nullptr;
	}

	void step() override {
		Module* main = mainOnLeft();
		for (int r = 0; r < WaveArtifactSendReturn::ROWS; r++) {
			ParamWidget* ws[2] = {sendKnobs[r], modeButtons[r]};
			int ids[2] = {WaveArtifact::MARKER_SEND_LEVEL_PARAM + r, WaveArtifact::MARKER_RETURN_MODE_PARAM + r};
			for (int k = 0; k < 2; k++) {
				if (!ws[k])
					continue;
				if (ws[k]->module != main || ws[k]->paramId != ids[k]) {
					ws[k]->module = main;
					ws[k]->paramId = ids[k];
				}
				ws[k]->setVisible(main != nullptr);
			}
		}
		ModuleWidget::step();
	}
};

Model* modelWaveArtifactSendReturn = createModel<WaveArtifactSendReturn, WaveArtifactSendReturnWidget>("waveArtifactSendReturn");
