#include <osdialog.h>

#include "Layout.hpp"
#include "WaveArtifact.hpp"
#include "widgets/DotStrip.hpp"
#include "widgets/MarkerBadge.hpp"
#include "widgets/PanelLabels.hpp"
#include "widgets/StatusDisplay.hpp"

namespace {

const float DEFAULT_MARKER_POS[WaveArtifact::NUM_MARKERS] = {0.f, 1.f, 0.25f, 0.5f, 0.75f};
const size_t MAX_SHOWN_NAME = 22;
const float LN_100 = 4.605170186f;

/** Band-pass frequency 20 Hz to 20 kHz on a log scale. Fully counter-clockwise bypasses the filter. */
struct BpQuantity : ParamQuantity {
	bool isOff() {
		return getValue() <= getMinValue();
	}
	std::string getDisplayValueString() override {
		return isOff() ? "Off" : ParamQuantity::getDisplayValueString();
	}
	std::string getUnit() override {
		return isOff() ? "" : ParamQuantity::getUnit();
	}
};

/** Shows the wavetable position as a 1-based table number of the loaded bank, e.g. "12.4 / 50". */
struct WaveQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(module);
		const wa::WavetableBank* bank = m ? m->activeBank.load() : nullptr;
		if (!bank)
			return string::f("%.0f%%", getValue() * 100.f);
		return string::f("%.1f / %d", 1.f + getValue() * (bank->numTables - 1), bank->numTables);
	}
	std::string getUnit() override {
		return "";
	}
};

/** LFO rate: Hz in free mode, note value (e.g. "1/8T") in sync mode. */
struct LfoRateQuantity : ParamQuantity {
	int marker = 0;
	bool isSync() {
		return module && module->params[WaveArtifact::MARKER_LFO_SYNC_PARAM + marker].getValue() > 0.5f;
	}
	std::string getDisplayValueString() override {
		if (isSync())
			return wa::SYNC_DIVISIONS[wa::lfoDivision(getValue())].name;
		float hz = wa::lfoFreeHz(getValue());
		return string::f(hz < 1.f ? "%.3f" : "%.2f", hz);
	}
	std::string getUnit() override {
		return isSync() ? "" : " Hz";
	}
};

std::string shortName(const std::string& name) {
	if (name.size() <= MAX_SHOWN_NAME)
		return name;
	return name.substr(0, MAX_SHOWN_NAME - 3) + "...";
}

} // namespace

WaveArtifact::WaveArtifact() {
	config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

	configParam(POSITION_PARAM, 0.f, 1.f, 0.f, "Position", "%", 0.f, 100.f);
	configParam(POS_ATTEN_PARAM, -1.f, 1.f, 1.f, "Position CV amount", "%", 0.f, 100.f);
	configParam(FREQ_PARAM, -4.f, 4.f, 0.f, "Frequency", " Hz", 2.f, dsp::FREQ_C4);
	configButton(LOAD_PARAM, "Load WAV");
	configButton(ARRANGE_PARAM, "Auto arrange markers");

	for (int i = 0; i < NUM_MARKERS; i++) {
		std::string name = "Marker " + std::to_string(i + 1);
		configSwitch(MARKER_ACTIVE_PARAM + i, 0.f, 1.f, (i < 2) ? 1.f : 0.f, name + " active", {"Off", "On"});
		configParam(MARKER_POS_PARAM + i, 0.f, 1.f, DEFAULT_MARKER_POS[i], name + " position", "%", 0.f, 100.f);
		// Default wavetable follows the bar position: left = start of the sample.
		configParam<WaveQuantity>(MARKER_WAVE_PARAM + i, 0.f, 1.f, DEFAULT_MARKER_POS[i], name + " wavetable");
		// Minimum is the default knob-step smoothing; turning it up makes WAVE glide slowly.
		configParam(MARKER_SMOOTH_PARAM + i, 0.f, 1.f, 0.f, name + " WAVE smoothing", " ms", 100.f, 1000.f * wa::MIN_WAVE_SMOOTH);
		// Default rate is ~0.2 Hz, a slow scan (knob = ln(20) / ln(2000)).
		configParam<LfoRateQuantity>(MARKER_LFO_RATE_PARAM + i, 0.f, 1.f, 0.394f, name + " WAVE LFO rate")->marker = i;
		configParam(MARKER_LFO_AMT_PARAM + i, 0.f, 1.f, 0.f, name + " WAVE LFO amount", "%", 0.f, 100.f);
		configSwitch(MARKER_LFO_SYNC_PARAM + i, 0.f, 1.f, 0.f, name + " WAVE LFO sync", {"Free (Hz)", "Clock sync"});
		configParam(MARKER_GAIN_PARAM + i, 0.f, 4.f, 1.f, name + " gain", "%", 0.f, 100.f);
		configParam<BpQuantity>(MARKER_BP_PARAM + i, 0.f, 1.f, 0.f, name + " band-pass frequency", " Hz", 1000.f, 20.f);
		configParam(MARKER_RES_PARAM + i, 0.f, 1.f, 0.f, name + " band-pass resonance", "%", 0.f, 100.f);
		configParam(MARKER_PITCH_PARAM + i, -24.f, 24.f, 0.f, name + " pitch", " st")->snapEnabled = true;
		configParam(MARKER_PAN_PARAM + i, -1.f, 1.f, 0.f, name + " pan", "%", 0.f, 100.f);

		// Randomizing these would shuffle the bar layout, not the sound.
		getParamQuantity(MARKER_ACTIVE_PARAM + i)->randomizeEnabled = false;
		getParamQuantity(MARKER_POS_PARAM + i)->randomizeEnabled = false;
	}

	configInput(VOCT_INPUT, "1V/octave pitch");
	configInput(POS_INPUT, "Position CV");
	configInput(CLOCK_INPUT, "Clock (1 pulse per quarter note) for LFO sync");
	configOutput(LEFT_OUTPUT, "Left");
	configOutput(RIGHT_OUTPUT, "Right");

	setStatus("Load a WAV (max 2 MB)");
}

WaveArtifact::~WaveArtifact() {
	if (worker.joinable())
		worker.join();
	delete pendingBank.exchange(nullptr);
	delete activeBank.exchange(nullptr);
	delete retiredBank.exchange(nullptr);
}

void WaveArtifact::process(const ProcessArgs& args) {
	if (args.sampleRate != engine.getSampleRate())
		engine.setSampleRate(args.sampleRate);

	// A new bank waits until the engine has faded to silence. Swap only once the UI has freed
	// the previous retired bank, so nothing is ever deleted on the audio thread.
	bool swapWanted = pendingBank.load(std::memory_order_relaxed) && !retiredBank.load();

	clock.process(inputs[CLOCK_INPUT].getVoltage(), inputs[CLOCK_INPUT].isConnected(), args.sampleRate);

	wa::EngineInput in;
	for (int i = 0; i < NUM_MARKERS; i++) {
		wa::MarkerInput& m = in.markers[i];
		m.active = params[MARKER_ACTIVE_PARAM + i].getValue() > 0.5f;
		m.pos = params[MARKER_POS_PARAM + i].getValue();
		m.wave = params[MARKER_WAVE_PARAM + i].getValue();
		// The engine adds the LFO after WAVE's glide (so SMOOTH does not dull it) and folds it back
		// at the ends of the sample.
		bool sync = params[MARKER_LFO_SYNC_PARAM + i].getValue() > 0.5f;
		m.lfo = lfos[i].process(sync, params[MARKER_LFO_RATE_PARAM + i].getValue(), clock, args.sampleTime);
		m.lfoAmount = params[MARKER_LFO_AMT_PARAM + i].getValue();
		m.waveSmooth = wa::MIN_WAVE_SMOOTH * std::exp(params[MARKER_SMOOTH_PARAM + i].getValue() * LN_100);
		m.gain = params[MARKER_GAIN_PARAM + i].getValue();
		m.bp = params[MARKER_BP_PARAM + i].getValue();
		m.res = params[MARKER_RES_PARAM + i].getValue();
		m.pitch = params[MARKER_PITCH_PARAM + i].getValue();
		m.pan = params[MARKER_PAN_PARAM + i].getValue();
	}
	// 0..10 V on POS CV sweeps the whole bar.
	in.position = params[POSITION_PARAM].getValue() + params[POS_ATTEN_PARAM].getValue() * inputs[POS_INPUT].getVoltage() / 10.f;
	in.voct = params[FREQ_PARAM].getValue() + inputs[VOCT_INPUT].getVoltage();
	in.bank = activeBank.load(std::memory_order_relaxed);
	in.holdSilent = swapWanted;

	float l, r;
	engine.process(in, &l, &r);
	outputs[LEFT_OUTPUT].setVoltage(5.f * l);
	outputs[RIGHT_OUTPUT].setVoltage(5.f * r);

	if (swapWanted && engine.isSilent()) {
		wa::WavetableBank* fresh = pendingBank.exchange(nullptr);
		if (fresh)
			retiredBank.store(activeBank.exchange(fresh));
	}
}

void WaveArtifact::onSampleRateChange(const SampleRateChangeEvent& e) {
	Module::onSampleRateChange(e);
	engine.setSampleRate(e.sampleRate);
}

void WaveArtifact::loadFile(const std::string& path) {
	if (analyzing) {
		setStatus("Busy, still analyzing");
		return;
	}
	uint64_t size = system::getFileSize(path);
	if (size == 0) {
		setStatus("Cannot read file");
		return;
	}
	if (size > wa::MAX_FILE_BYTES) {
		setStatus("File larger than 2 MB");
		return;
	}
	std::vector<uint8_t> bytes = system::readFile(path);
	if (bytes.empty()) {
		setStatus("Cannot read file");
		return;
	}
	wavBytes = std::move(bytes);
	fileName = system::getFilename(path);
	startAnalysis();
}

void WaveArtifact::startAnalysis() {
	if (wavBytes.empty())
		return;
	if (analyzing) {
		setStatus("Busy, still analyzing");
		return;
	}
	if (worker.joinable())
		worker.join();

	analyzing = true;
	std::string name = shortName(fileName);
	setStatus("Analyzing " + name);

	// The worker gets its own copy, so the UI may replace wavBytes at any time.
	std::vector<uint8_t> bytes = wavBytes;
	wa::TableCountMode mode = tableCountMode;
	worker = std::thread([this, bytes, mode, name]() {
		std::string error;
		wa::WavetableBank* bank = wa::analyzeWav(bytes.data(), bytes.size(), mode, &error);
		if (bank) {
			// If process() has not taken the previous result yet, it never saw it, so it is
			// safe to delete here.
			delete pendingBank.exchange(bank);
			setStatus(name + "  " + std::to_string(bank->numTables) + " tables");
		}
		else {
			setStatus(error);
		}
		analyzing = false;
	});
}

void WaveArtifact::uiStep() {
	delete retiredBank.exchange(nullptr);
}

void WaveArtifact::autoArrange() {
	// The engine treats newly activated markers as a structural change (5 ms fade out/in) and
	// crossfades the wavetable jumps, so arranging while playing is click-free.
	for (int i = 0; i < NUM_MARKERS; i++) {
		float pos = (float) i / (NUM_MARKERS - 1);
		params[MARKER_ACTIVE_PARAM + i].setValue(1.f);
		params[MARKER_POS_PARAM + i].setValue(pos);
		params[MARKER_WAVE_PARAM + i].setValue(pos);
		for (int id : {MARKER_SMOOTH_PARAM, MARKER_LFO_RATE_PARAM, MARKER_LFO_AMT_PARAM, MARKER_LFO_SYNC_PARAM, MARKER_GAIN_PARAM, MARKER_BP_PARAM, MARKER_RES_PARAM, MARKER_PITCH_PARAM, MARKER_PAN_PARAM})
			params[id + i].setValue(getParamQuantity(id + i)->getDefaultValue());
	}
}

std::string WaveArtifact::getStatus() {
	std::lock_guard<std::mutex> lock(statusMutex);
	return status;
}

void WaveArtifact::setStatus(const std::string& s) {
	std::lock_guard<std::mutex> lock(statusMutex);
	status = s;
}

static void openLoadDialog(WaveArtifact* module) {
	if (!module)
		return;
	osdialog_filters* filters = osdialog_filters_parse("WAV:wav,WAV");
	char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, filters);
	osdialog_filters_free(filters);
	if (!path)
		return;
	module->loadFile(path);
	std::free(path);
}

/** Momentary button that opens the file dialog on release. */
struct LoadButton : VCVButton {
	void onDragEnd(const DragEndEvent& e) override {
		VCVButton::onDragEnd(e);
		if (e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		openLoadDialog(dynamic_cast<WaveArtifact*>(module));
	}
};

/** Momentary button that auto-arranges the markers on release. */
struct ArrangeButton : VCVButton {
	void onDragEnd(const DragEndEvent& e) override {
		VCVButton::onDragEnd(e);
		if (e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(module);
		if (m)
			m->autoArrange();
	}
};

struct WaveArtifactWidget : ModuleWidget {
	static const int EDIT_KNOBS = 10;
	ParamWidget* editKnobs[WaveArtifact::NUM_MARKERS][EDIT_KNOBS] = {};

	WaveArtifactWidget(WaveArtifact* module) {
		using namespace layout;
		setModule(module);
		setPanel(createPanel(
			asset::plugin(pluginInstance, "res/waveArtifact.svg"),
			asset::plugin(pluginInstance, "res/waveArtifact-dark.svg")
		));

		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		PanelLabels* labels = createWidget<PanelLabels>(Vec(0, 0));
		labels->box.size = box.size;
		labels->add(Vec(PANEL_W / 2.f, 6.f), "waveArtifact", 13.f);
		labels->add(Vec(MARKER_BADGE.x, EDIT_LABEL_Y), "MARKER");
		labels->add(Vec(WAVE_KNOB.x, EDIT_LABEL_Y), "WAVE");
		labels->add(Vec(SMOOTH_KNOB.x + 4.f, SMOOTH_KNOB.y), "SMOOTH", 6.f, false, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		labels->add(Vec(LFO_RATE_KNOB.x + 4.f, LFO_RATE_KNOB.y), "LFO RATE", 6.f, false, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		labels->add(Vec(LFO_AMT_KNOB.x + 4.f, LFO_AMT_KNOB.y), "AMT", 6.f, false, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		labels->add(Vec(LFO_SYNC_BUTTON.x + 4.f, LFO_SYNC_BUTTON.y), "SYNC", 6.f, false, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		labels->add(Vec(CLOCK_INPUT.x, CONTROL_LABEL_Y), "CLOCK");
		labels->add(Vec(GAIN_KNOB.x, EDIT_LABEL_Y), "GAIN");
		labels->add(Vec(BP_KNOB.x, EDIT_LABEL_Y), "BP");
		labels->add(Vec(RES_KNOB.x, EDIT_LABEL_Y), "RES");
		labels->add(Vec(PITCH_KNOB.x, EDIT_LABEL_Y), "PITCH");
		labels->add(Vec(PAN_KNOB.x, EDIT_LABEL_Y), "PAN");
		labels->add(Vec(LOAD_BUTTON.x + 5.f, LOAD_BUTTON.y), "LOAD WAV", 7.f, false, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		labels->add(Vec(STATUS_POS.x + STATUS_SIZE.x, STATUS_POS.y + STATUS_SIZE.y + 2.f), "MAX 2 MB", 6.f, false, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
		labels->add(Vec(ARRANGE_BUTTON.x + 5.f, ARRANGE_BUTTON.y), "ARRANGE", 7.f, false, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		labels->add(Vec(POSITION_KNOB.x, CONTROL_LABEL_Y), "POSITION");
		labels->add(Vec(POS_ATTEN.x, CONTROL_LABEL_Y), "CV AMT");
		labels->add(Vec(POS_INPUT.x, CONTROL_LABEL_Y), "POS CV");
		labels->add(Vec(FREQ_KNOB.x, CONTROL_LABEL_Y), "FREQ");
		labels->add(Vec(VOCT_INPUT.x, CONTROL_LABEL_Y), "V/OCT");
		labels->add(Vec(LEFT_OUTPUT.x, CONTROL_LABEL_Y - 1.f), "L", 7.f, true);
		labels->add(Vec(RIGHT_OUTPUT.x, CONTROL_LABEL_Y - 1.f), "R", 7.f, true);
		addChild(labels);

		StatusDisplay* status = createWidget<StatusDisplay>(mm2px(STATUS_POS));
		status->box.size = mm2px(STATUS_SIZE);
		status->module = module;
		addChild(status);

		DotStrip* dots = createWidget<DotStrip>(mm2px(DOTS_POS));
		dots->box.size = mm2px(DOTS_SIZE);
		dots->module = module;
		addChild(dots);

		MarkerBadge* badge = createWidget<MarkerBadge>(mm2px(MARKER_BADGE.minus(Vec(4.f, 4.f))));
		badge->box.size = mm2px(Vec(8.f, 8.f));
		badge->module = module;
		addChild(badge);

		// One knob set per marker at the same spot; step() shows the selected marker's set.
		for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
			editKnobs[i][0] = createParamCentered<RoundBlackKnob>(mm2px(WAVE_KNOB), module, WaveArtifact::MARKER_WAVE_PARAM + i);
			editKnobs[i][1] = createParamCentered<RoundBlackKnob>(mm2px(GAIN_KNOB), module, WaveArtifact::MARKER_GAIN_PARAM + i);
			editKnobs[i][2] = createParamCentered<RoundBlackKnob>(mm2px(BP_KNOB), module, WaveArtifact::MARKER_BP_PARAM + i);
			editKnobs[i][3] = createParamCentered<RoundBlackKnob>(mm2px(RES_KNOB), module, WaveArtifact::MARKER_RES_PARAM + i);
			editKnobs[i][4] = createParamCentered<RoundBlackKnob>(mm2px(PITCH_KNOB), module, WaveArtifact::MARKER_PITCH_PARAM + i);
			editKnobs[i][5] = createParamCentered<RoundBlackKnob>(mm2px(PAN_KNOB), module, WaveArtifact::MARKER_PAN_PARAM + i);
			editKnobs[i][6] = createParamCentered<Trimpot>(mm2px(SMOOTH_KNOB), module, WaveArtifact::MARKER_SMOOTH_PARAM + i);
			editKnobs[i][7] = createParamCentered<Trimpot>(mm2px(LFO_RATE_KNOB), module, WaveArtifact::MARKER_LFO_RATE_PARAM + i);
			editKnobs[i][8] = createParamCentered<Trimpot>(mm2px(LFO_AMT_KNOB), module, WaveArtifact::MARKER_LFO_AMT_PARAM + i);
			editKnobs[i][9] = createParamCentered<VCVLatch>(mm2px(LFO_SYNC_BUTTON), module, WaveArtifact::MARKER_LFO_SYNC_PARAM + i);
			for (int k = 0; k < EDIT_KNOBS; k++) {
				addParam(editKnobs[i][k]);
				editKnobs[i][k]->setVisible(i == 0);
			}
		}

		addParam(createParamCentered<LoadButton>(mm2px(LOAD_BUTTON), module, WaveArtifact::LOAD_PARAM));
		addParam(createParamCentered<ArrangeButton>(mm2px(ARRANGE_BUTTON), module, WaveArtifact::ARRANGE_PARAM));

		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(POSITION_KNOB), module, WaveArtifact::POSITION_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(POS_ATTEN), module, WaveArtifact::POS_ATTEN_PARAM));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(POS_INPUT), module, WaveArtifact::POS_INPUT));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(FREQ_KNOB), module, WaveArtifact::FREQ_PARAM));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(VOCT_INPUT), module, WaveArtifact::VOCT_INPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(CLOCK_INPUT), module, WaveArtifact::CLOCK_INPUT));

		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(LEFT_OUTPUT), module, WaveArtifact::LEFT_OUTPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(RIGHT_OUTPUT), module, WaveArtifact::RIGHT_OUTPUT));
	}

	void step() override {
		WaveArtifact* m = getModule<WaveArtifact>();
		if (m) {
			m->uiStep();
			for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
				for (int k = 0; k < EDIT_KNOBS; k++)
					editKnobs[i][k]->setVisible(i == m->selectedMarker);
			}
		}
		ModuleWidget::step();
	}

	void appendContextMenu(Menu* menu) override {
		WaveArtifact* m = getModule<WaveArtifact>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Load WAV...", "", [=]() {
			openLoadDialog(m);
		}));
		menu->addChild(createIndexSubmenuItem("Wavetable count",
			{"Auto (50-100)", "50", "64", "100"},
			[=]() {
				return (size_t) m->tableCountMode;
			},
			[=](size_t mode) {
				m->tableCountMode = (wa::TableCountMode) mode;
				m->startAnalysis();
			}
		));
		menu->addChild(createMenuItem("Auto arrange markers", "", [=]() {
			m->autoArrange();
		}));

		// Interim marker controls until the marker bar (M5) handles this on the panel.
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Markers (temporary, until the marker bar)"));
		menu->addChild(createIndexSubmenuItem("Edit knobs for",
			{"Marker 1", "Marker 2", "Marker 3", "Marker 4", "Marker 5"},
			[=]() {
				return (size_t) m->selectedMarker;
			},
			[=](size_t i) {
				m->selectedMarker = (int) i;
			}
		));
		for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
			menu->addChild(createSubmenuItem("Marker " + std::to_string(i + 1), "", [=](Menu* sub) {
				sub->addChild(createCheckMenuItem("Active", "",
					[=]() {
						return m->params[WaveArtifact::MARKER_ACTIVE_PARAM + i].getValue() > 0.5f;
					},
					[=]() {
						Param& p = m->params[WaveArtifact::MARKER_ACTIVE_PARAM + i];
						p.setValue(p.getValue() > 0.5f ? 0.f : 1.f);
					}
				));
				sub->addChild(new MenuSeparator);
				for (float pos : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
					sub->addChild(createMenuItem(string::f("Position %d%%", (int) (pos * 100)), "", [=]() {
						m->params[WaveArtifact::MARKER_POS_PARAM + i].setValue(pos);
					}));
				}
				const wa::WavetableBank* bank = m->activeBank.load();
				int n = bank ? bank->numTables : 0;
				if (n > 1) {
					sub->addChild(new MenuSeparator);
					for (int step = 0; step <= 4; step++) {
						int t = (int) std::round(step / 4.f * (n - 1));
						sub->addChild(createMenuItem(string::f("Wavetable %d", t + 1), "", [=]() {
							m->params[WaveArtifact::MARKER_WAVE_PARAM + i].setValue((float) t / (n - 1));
						}));
					}
				}
			}));
		}
	}
};

Model* modelWaveArtifact = createModel<WaveArtifact, WaveArtifactWidget>("waveArtifact");
