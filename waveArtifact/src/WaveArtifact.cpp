#include <osdialog.h>

#include "Layout.hpp"
#include "WaveArtifact.hpp"
#include "WaveArtifactExpander.hpp"
#include "widgets/ArtifactColor.hpp"
#include "widgets/WavetableMap.hpp"
#include "widgets/LfoControls.hpp"
#include "widgets/LfoWidgets.hpp"
#include "widgets/MarkerBadge.hpp"
#include "widgets/MarkerBar.hpp"
#include "widgets/ModeButton.hpp"
#include "widgets/PanelLabels.hpp"

namespace {

const float DEFAULT_MARKER_POS[WaveArtifact::NUM_MARKERS] = {0.f, 1.f, 0.25f, 0.5f, 0.75f};
const size_t MAX_SHOWN_NAME = 22;
const float LN_100 = 4.605170186f;


/** Shows the artifact (wavetable) position as a 1-based number of the loaded bank, e.g. "12.4 / 50". */
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

std::vector<std::string> lfoShapeNames() {
	return std::vector<std::string>(wa::LFO_SHAPE_NAMES, wa::LFO_SHAPE_NAMES + wa::NUM_LFO_SHAPES);
}

std::string shortName(const std::string& name) {
	if (name.size() <= MAX_SHOWN_NAME)
		return name;
	return name.substr(0, MAX_SHOWN_NAME - 3) + "...";
}

} // namespace

WaveArtifact::WaveArtifact() {
	config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

	configParam(POSITION_PARAM, 0.f, 1.f, 0.f, "Position", "%", 0.f, 100.f);
	configParam(POS_ATTEN_PARAM, 0.f, 1.f, 0.f, "Position CV amount", "%", 0.f, 100.f);
	configParam(FREQ_PARAM, -4.f, 4.f, 0.f, "Frequency", " Hz", 2.f, dsp::FREQ_C4);
	configButton(LOAD_PARAM, "Load WAV");
	// No panel button any more (right-click menu: Auto arrange); the param stays for patch compatibility.
	configButton(ARRANGE_PARAM, "Auto arrange");

	for (int i = 0; i < NUM_MARKERS; i++) {
		std::string name = "Marker " + std::to_string(i + 1);
		configSwitch(MARKER_ACTIVE_PARAM + i, 0.f, 1.f, (i < 2) ? 1.f : 0.f, name + " active", {"Off", "On"});
		configParam(MARKER_POS_PARAM + i, 0.f, 1.f, DEFAULT_MARKER_POS[i], name + " position", "%", 0.f, 100.f);
		// Default wavetable follows the bar position: left = start of the sample.
		configParam<WaveQuantity>(MARKER_WAVE_PARAM + i, 0.f, 1.f, DEFAULT_MARKER_POS[i], name + " artifact");
		// Minimum is the default knob-step smoothing; turning it up makes WAVE glide slowly.
		configParam(MARKER_SMOOTH_PARAM + i, 0.f, 1.f, 0.f, name + " WAVE smoothing", " ms", 100.f, 1000.f * wa::MIN_WAVE_SMOOTH);
		// Default rate is ~0.2 Hz, a slow scan (knob = ln(20) / ln(2000)).
		configLfoRate(this, MARKER_LFO_RATE_PARAM + i, MARKER_LFO_SYNC_PARAM + i, MARKER_LFO_TRIPLET_PARAM + i, MARKER_LFO_DOTTED_PARAM + i, name + " WAVE LFO rate");
		configParam(MARKER_LFO_AMT_PARAM + i, 0.f, 1.f, 0.f, name + " WAVE LFO amount", "%", 0.f, 100.f);
		// Synced by default: the module always has a tempo (internal BPM or CLOCK).
		configSwitch(MARKER_LFO_SYNC_PARAM + i, 0.f, 1.f, 1.f, name + " WAVE LFO sync", {"Free (Hz)", "Tempo sync (BPM / CLOCK)"});
		configSwitch(MARKER_LFO_TRIPLET_PARAM + i, 0.f, 1.f, 0.f, name + " WAVE LFO triplets", {"Off", "On"});
		configSwitch(MARKER_LFO_DOTTED_PARAM + i, 0.f, 1.f, 0.f, name + " WAVE LFO dotted", {"Off", "On"});
		for (int id : {MARKER_LFO_SYNC_PARAM, MARKER_LFO_TRIPLET_PARAM, MARKER_LFO_DOTTED_PARAM})
			getParamQuantity(id + i)->randomizeEnabled = false;
		configSwitch(MARKER_LFO_SHAPE_PARAM + i, 0.f, wa::NUM_LFO_SHAPES - 1, 0.f, name + " WAVE LFO shape", lfoShapeNames());
		getParamQuantity(MARKER_LFO_SHAPE_PARAM + i)->randomizeEnabled = false;
		lfos[i].seed(random::u32());
		// Shown in dB: 0 dB (the default) is unity; stored as a linear factor so saved patches are unchanged.
		configParam(MARKER_GAIN_PARAM + i, 0.f, 4.f, 1.f, name + " gain", " dB", -10.f, 20.f);
		configParam(MARKER_CUTOFF_PARAM + i, 0.f, 1.f, 1.f, name + " filter cutoff", " Hz", 1000.f, 20.f);
		configParam(MARKER_RES_PARAM + i, 0.f, 1.f, 0.f, name + " filter resonance", "%", 0.f, 100.f);
		configSwitch(MARKER_FILTER_MODE_PARAM + i, 0.f, 3.f, 0.f, name + " filter type", {"Low-pass", "Band-pass", "High-pass", "Notch"});
		configParam(MARKER_DRIVE_PARAM + i, 0.f, 1.f, 0.f, name + " drive", "%", 0.f, 100.f);
		// Steps in semitones; uiStep() turns snapping off while the marker's FREE button is on.
		configParam(MARKER_PITCH_PARAM + i, -36.f, 36.f, 0.f, name + " pitch", " st")->snapEnabled = true;
		configSwitch(MARKER_PITCH_FREE_PARAM + i, 0.f, 1.f, 0.f, name + " pitch mode", {"Semitones", "Free"});
		getParamQuantity(MARKER_PITCH_FREE_PARAM + i)->randomizeEnabled = false;
		configParam(MARKER_FINE_PARAM + i, -1.f, 1.f, 0.f, name + " fine tune", " cents", 0.f, 100.f);
		configParam(MARKER_PAN_PARAM + i, -1.f, 1.f, 0.f, name + " pan", "%", 0.f, 100.f);

		// Randomizing these would shuffle the bar layout, not the sound.
		getParamQuantity(MARKER_ACTIVE_PARAM + i)->randomizeEnabled = false;
		getParamQuantity(MARKER_POS_PARAM + i)->randomizeEnabled = false;
	}

	configSwitch(MASTER_MODE_PARAM, 0.f, 4.f, 1.f, "Master filter type", {"Off", "Low-pass", "Band-pass", "High-pass", "Notch"});
	configParam(MASTER_CUTOFF_PARAM, 0.f, 1.f, 1.f, "Master filter cutoff", " Hz", 1000.f, 20.f);
	configParam(MASTER_RES_PARAM, 0.f, 1.f, 0.f, "Master filter resonance", "%", 0.f, 100.f);
	configParam(MASTER_SPREAD_PARAM, -1.f, 1.f, 0.f, "Master filter spread (right above left)", " oct", 0.f, 2.f);
	configParam(MASTER_FDBK_PARAM, 0.f, 1.f, 0.f, "Master filter feedback (self-oscillates above ~75%)", "%", 0.f, 100.f);
	configParam(MASTER_DRIVE_PARAM, 0.f, 1.f, 0.f, "Master drive (warm saturation)", "%", 0.f, 100.f);
	configParam(MASTER_GAIN_PARAM, 0.f, 2.f, 1.f, "Master gain", "%", 0.f, 100.f);
	const char* masterNames[5] = {"cutoff", "resonance", "drive", "spread", "feedback"};
	for (int k = 0; k < 5; k++) {
		std::string n = std::string("Master filter ") + masterNames[k];
		if (k == 2)
			n = "Master drive";
		// Cutoff, resonance, drive and feedback amounts are unipolar (0..100%); spread is bipolar.
		configParam(MASTER_CUTOFF_AMT_PARAM + k, (k == 3) ? -1.f : 0.f, 1.f, 0.f, n + " CV amount", "%", 0.f, 100.f);
		configInput(MASTER_CUTOFF_INPUT + k, n + " CV");
	}

	// Position LFO: the same LFO as a marker's, moving POSITION (folded back at the ends).
	configSwitch(POS_LFO_SHAPE_PARAM, 0.f, wa::NUM_LFO_SHAPES - 1, 0.f, "Position LFO shape", lfoShapeNames());
	configLfoRate(this, POS_LFO_RATE_PARAM, POS_LFO_SYNC_PARAM, POS_LFO_TRIPLET_PARAM, POS_LFO_DOTTED_PARAM, "Position LFO rate");
	configParam(POS_LFO_AMT_PARAM, 0.f, 1.f, 0.f, "Position LFO amount", "%", 0.f, 100.f);
	configSwitch(POS_LFO_SYNC_PARAM, 0.f, 1.f, 1.f, "Position LFO sync", {"Free (Hz)", "Tempo sync (BPM / CLOCK)"});
	configSwitch(POS_LFO_TRIPLET_PARAM, 0.f, 1.f, 0.f, "Position LFO triplets", {"Off", "On"});
	configSwitch(POS_LFO_DOTTED_PARAM, 0.f, 1.f, 0.f, "Position LFO dotted", {"Off", "On"});
	for (int id : {POS_LFO_SHAPE_PARAM, POS_LFO_SYNC_PARAM, POS_LFO_TRIPLET_PARAM, POS_LFO_DOTTED_PARAM})
		getParamQuantity(id)->randomizeEnabled = false;
	posLfo.seed(random::u32());

	configInput(VOCT_INPUT, "1V/octave pitch");
	configInput(POS_INPUT, "Position CV");
	configInput(CLOCK_INPUT, "Clock for LFO sync (pulses per quarter note: right-click menu)");
	configInput(RESET_INPUT, "Reset: restarts the LFOs, next clock pulse is the start of a bar");
	configParam(INTERNAL_BPM_PARAM, 20.f, 300.f, 120.f, "Internal tempo (used while CLOCK is unpatched)", " BPM")->snapEnabled = true;
	configButton(RESTART_PARAM, "Restart: beat grid and every LFO start again (same as a RESET gate)");
	configSwitch(STOP_PARAM, 0.f, 1.f, 0.f, "Stop audio (all outputs fade to silence; LFOs and clock keep running)", {"Playing", "Stopped"});
	configSwitch(CLOCK_PPQN_PARAM, 0.f, wa::NUM_PPQN - 1, 0.f, "Clock resolution", {"1 PPQN (quarter notes)", "2 PPQN (eighths)", "4 PPQN (sixteenths)", "8 PPQN", "12 PPQN", "24 PPQN (MIDI clock)", "48 PPQN"});
	getParamQuantity(CLOCK_PPQN_PARAM)->randomizeEnabled = false;
	configOutput(LEFT_OUTPUT, "Left");
	configOutput(RIGHT_OUTPUT, "Right");
	configOutput(CLOCK_OUTPUT, "Clock out (one 10 V trigger per beat: internal BPM, or the CLOCK input)");
	for (int i = 0; i < NUM_MARKERS; i++) {
		std::string name = "Artifact " + std::to_string(i + 1);
		configOutput(ARTIFACT_L_OUTPUT + i, name + " left (mono when right is unpatched; patched = leaves the main L/R)");
		configOutput(ARTIFACT_R_OUTPUT + i, name + " right (patched = leaves the main L/R)");
	}

	for (int i = 0; i < NUM_MARKERS; i++) {
		shownIndex[i].store(0.f);
		shownCv[i].store(false);
		shownHeard[i].store(0.f);
	}
	// Modulation arrives from expanders on the right.
	rightExpander.producerMessage = new wa::ToMainMessage;
	rightExpander.consumerMessage = new wa::ToMainMessage;
	setStatus("Load a WAV (max 10 MB)");
}

void WaveArtifact::onReset(const ResetEvent& e) {
	Module::onReset(e);
	selectedMarker = 0;
	previewTable = -1;
	// Initialize = a fresh module: no sample. Rack calls onReset with the engine locked
	// (Engine::resetModule takes the engine mutex exclusively), so process() is not running and the
	// banks can be freed here. An analysis still running is discarded when it finishes (generation).
	{
		std::lock_guard<std::mutex> lock(publishMutex);
		analysisGeneration++;
		delete pendingBank.exchange(nullptr);
		delete activeBank.exchange(nullptr);
		delete retiredBank.exchange(nullptr);
		bankSerial++;
	}
	wavBytes.clear();
	fileName.clear();
	setStatus("Load a WAV (max 10 MB)");
}

WaveArtifact::~WaveArtifact() {
	if (worker.joinable())
		worker.join();
	delete pendingBank.exchange(nullptr);
	delete activeBank.exchange(nullptr);
	delete retiredBank.exchange(nullptr);
	delete (wa::ToMainMessage*) rightExpander.producerMessage;
	delete (wa::ToMainMessage*) rightExpander.consumerMessage;
}

void WaveArtifact::process(const ProcessArgs& args) {
	if (args.sampleRate != engine.getSampleRate())
		engine.setSampleRate(args.sampleRate);

	// A new bank waits until the engine has faded to silence. Swap only once the UI has freed
	// the previous retired bank, so nothing is ever deleted on the audio thread.
	bool swapWanted = pendingBank.load(std::memory_order_relaxed) && !retiredBank.load();

	// RESET first, so a reset and a clock pulse arriving together make that pulse beat 0.
	// Restart sources: RESET gate, the RESTART button (on press), the menu.
	bool restartPressed = params[RESTART_PARAM].getValue() > 0.5f;
	bool reset = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
	reset = (restartPressed && !restartHeld) || reset;
	restartHeld = restartPressed;
	if (restartRequest.load(std::memory_order_relaxed) && restartRequest.exchange(false))
		reset = true;
	if (reset) {
		clock.reset();
		for (int i = 0; i < NUM_MARKERS; i++)
			lfos[i].reset();
		posLfo.reset();
	}
	int ppqn = wa::PPQN_VALUES[clamp((int) std::round(params[CLOCK_PPQN_PARAM].getValue()), 0, wa::NUM_PPQN - 1)];
	// CLOCK patched: follow it. Unpatched: the internal tempo makes the same beats.
	if (inputs[CLOCK_INPUT].isConnected())
		clock.process(inputs[CLOCK_INPUT].getVoltage(), true, args.sampleRate, ppqn);
	else
		clock.processInternal(params[INTERNAL_BPM_PARAM].getValue(), args.sampleRate);
	// CLOCK OUT: one trigger per beat of whichever clock is in use (a restart gives one at once).
	if (clock.isEdge())
		clockOutPulse.trigger(5e-3f);
	outputs[CLOCK_OUTPUT].setVoltage(clockOutPulse.process(args.sampleTime) ? 10.f : 0.f);

	// Expanders: send them the clock and their chain position, take their summed modulation.
	static const wa::ToMainMessage noMod;
	const wa::ToMainMessage* mod = &noMod;
	Module* right = rightExpander.module;
	if (isWaveArtifactExpander(right)) {
		mod = (const wa::ToMainMessage*) rightExpander.consumerMessage;
		wa::ToExpanderMessage* out = (wa::ToExpanderMessage*) right->leftExpander.producerMessage;
		out->clock = clock;
		out->index = 0;
		out->linked = true;
		out->reset = reset;
		right->leftExpander.requestMessageFlip();
	}
	auto modOf = [&](int marker, int target) {
		return mod->mod[marker][target] / 10.f * wa::MOD_SCALE_PER_10V[target];
	};

	wa::EngineInput in;
	for (int i = 0; i < NUM_MARKERS; i++) {
		wa::MarkerInput& m = in.markers[i];
		m.active = params[MARKER_ACTIVE_PARAM + i].getValue() > 0.5f;
		m.pos = params[MARKER_POS_PARAM + i].getValue();
		m.wave = params[MARKER_WAVE_PARAM + i].getValue();
		// The engine adds the LFO after WAVE's glide (so SMOOTH does not dull it) and folds it back
		// at the ends of the sample.
		bool sync = params[MARKER_LFO_SYNC_PARAM + i].getValue() > 0.5f;
		bool triplets = params[MARKER_LFO_TRIPLET_PARAM + i].getValue() > 0.5f;
		bool dotted = params[MARKER_LFO_DOTTED_PARAM + i].getValue() > 0.5f;
		int shape = (int) std::round(params[MARKER_LFO_SHAPE_PARAM + i].getValue());
		m.lfo = lfos[i].process(shape, sync, params[MARKER_LFO_RATE_PARAM + i].getValue(), triplets, dotted, clock, args.sampleTime);
		m.lfoAmount = params[MARKER_LFO_AMT_PARAM + i].getValue();
		m.waveSmooth = wa::MIN_WAVE_SMOOTH * std::exp(params[MARKER_SMOOTH_PARAM + i].getValue() * LN_100);
		m.gain = params[MARKER_GAIN_PARAM + i].getValue();
		m.filterMode = (int) std::round(params[MARKER_FILTER_MODE_PARAM + i].getValue());
		m.res = params[MARKER_RES_PARAM + i].getValue();
		// Semitone mode plays whole semitones even if the knob holds a fraction (it is re-snapped in uiStep).
		float pitch = params[MARKER_PITCH_PARAM + i].getValue();
		m.pitch = (params[MARKER_PITCH_FREE_PARAM + i].getValue() > 0.5f) ? pitch : std::round(pitch);

		// Expander CV, added in each param's own units (see MOD_SCALE_PER_10V). WAVE is folded at
		// the sample's ends by the engine, like the LFO. The filter is always on; CUTOFF CV sweeps it
		// across its whole range (20 Hz .. 20 kHz).
		m.wave += modOf(i, wa::MOD_WAVE);
		m.gain = clamp(m.gain * mod->gainMul[i], 0.f, 4.f); // expander VCA
		m.cutoff = clamp(params[MARKER_CUTOFF_PARAM + i].getValue() + modOf(i, wa::MOD_CUTOFF), 0.f, 1.f);
		m.drive = clamp(params[MARKER_DRIVE_PARAM + i].getValue() + modOf(i, wa::MOD_DRIVE), 0.f, 1.f);
		m.fine = clamp(params[MARKER_FINE_PARAM + i].getValue() + modOf(i, wa::MOD_FINE), -12.f, 12.f);
		// Expander V/OCT, added to the main V/OCT for this artifact only
		m.voct = mod->voct[i];
		m.pan = clamp(params[MARKER_PAN_PARAM + i].getValue() + modOf(i, wa::MOD_PAN), -1.f, 1.f);
	}
	// 0..10 V on POS CV sweeps the whole bar.
	// Position: knob + CV (kept in range), then the position LFO sweeps +-amount/2 around it (100% =
	// the whole bar). The center is held back from the ends so the whole swing fits.
	float position = clamp(params[POSITION_PARAM].getValue() + params[POS_ATTEN_PARAM].getValue() * inputs[POS_INPUT].getVoltage() / 10.f, 0.f, 1.f);
	{
		int shape = (int) std::round(params[POS_LFO_SHAPE_PARAM].getValue());
		bool sync = params[POS_LFO_SYNC_PARAM].getValue() > 0.5f;
		bool triplets = params[POS_LFO_TRIPLET_PARAM].getValue() > 0.5f;
		bool dotted = params[POS_LFO_DOTTED_PARAM].getValue() > 0.5f;
		float lfo = posLfo.process(shape, sync, params[POS_LFO_RATE_PARAM].getValue(), triplets, dotted, clock, args.sampleTime);
		float amt = params[POS_LFO_AMT_PARAM].getValue();
		if (amt > 0.f)
			position = wa::lfoCentered(position, amt, lfo);
	}
	in.position = position;
	in.voct = params[FREQ_PARAM].getValue() + inputs[VOCT_INPUT].getVoltage();
	in.bank = activeBank.load(std::memory_order_relaxed);
	// STOP uses the same click-free fade as a bank swap (main, artifact outputs and preview).
	in.holdSilent = swapWanted || params[STOP_PARAM].getValue() > 0.5f;
	in.previewTable = previewTable.load(std::memory_order_relaxed);
	// Only artifacts with a patched output are rendered for it.
	for (int i = 0; i < NUM_MARKERS; i++)
		in.wantDirect[i] = outputs[ARTIFACT_L_OUTPUT + i].isConnected() || outputs[ARTIFACT_R_OUTPUT + i].isConnected();
	in.masterMode = (int) std::round(params[MASTER_MODE_PARAM].getValue()) - 1;
	in.masterCutoff = params[MASTER_CUTOFF_PARAM].getValue();
	in.masterRes = params[MASTER_RES_PARAM].getValue();
	in.masterSpread = params[MASTER_SPREAD_PARAM].getValue();
	in.masterFdbk = params[MASTER_FDBK_PARAM].getValue();
	in.masterDrive = params[MASTER_DRIVE_PARAM].getValue();
	in.masterGain = params[MASTER_GAIN_PARAM].getValue();
	// Master CV: knob units, 10 V at 100% = the whole knob range (cutoff: about 1 V/oct). Added in the
	// engine after smoothing, so it can run at audio rate.
	auto masterCv = [&](int input, int amt) {
		return inputs[input].isConnected() ? params[amt].getValue() * inputs[input].getVoltage() / 10.f : 0.f;
	};
	in.masterCutoffMod = masterCv(MASTER_CUTOFF_INPUT, MASTER_CUTOFF_AMT_PARAM);
	in.masterResMod = masterCv(MASTER_RES_INPUT, MASTER_RES_AMT_PARAM);
	in.masterDriveMod = masterCv(MASTER_DRIVE_INPUT, MASTER_DRIVE_AMT_PARAM);
	in.masterSpreadMod = masterCv(MASTER_SPREAD_INPUT, MASTER_SPREAD_AMT_PARAM);
	in.masterFdbkMod = masterCv(MASTER_FDBK_INPUT, MASTER_FDBK_AMT_PARAM);

	float l, r;
	engine.process(in, &l, &r);
	outputs[LEFT_OUTPUT].setVoltage(5.f * l);
	outputs[RIGHT_OUTPUT].setVoltage(5.f * r);
	for (int i = 0; i < NUM_MARKERS; i++) {
		float dl, dr;
		engine.getDirect(i, &dl, &dr);
		bool stereo = outputs[ARTIFACT_R_OUTPUT + i].isConnected();
		outputs[ARTIFACT_L_OUTPUT + i].setVoltage(5.f * (stereo ? dl : 0.5f * (dl + dr)));
		outputs[ARTIFACT_R_OUTPUT + i].setVoltage(5.f * dr);
	}

	// Display values, a few hundred times a second is plenty for a 60 Hz screen.
	if (++displayCounter >= 64) {
		displayCounter = 0;
		for (int i = 0; i < NUM_MARKERS; i++) {
			shownIndex[i].store(engine.getTableIndex(i), std::memory_order_relaxed);
			shownHeard[i].store(engine.getHeard(i), std::memory_order_relaxed);
		}
		shownPosition.store(engine.getPosition(), std::memory_order_relaxed);
		shownPreview.store(engine.getPreviewLevel(), std::memory_order_relaxed);
		for (int i = 0; i < NUM_MARKERS; i++)
			shownCv[i].store(mod->modulated[i], std::memory_order_relaxed);
	}

	if (swapWanted && engine.isSilent()) {
		wa::WavetableBank* fresh = pendingBank.exchange(nullptr);
		if (fresh) {
			retiredBank.store(activeBank.exchange(fresh));
			bankSerial++;
		}
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
		setStatus("File larger than " + std::to_string(wa::MAX_FILE_MB) + " MB");
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
	wa::TableTone tone = tableTone;
	int generation = analysisGeneration;
	worker = std::thread([this, bytes, mode, tone, name, generation]() {
		std::string error;
		wa::WavetableBank* bank = wa::analyzeWav(bytes.data(), bytes.size(), mode, &error, tone);
		{
			std::lock_guard<std::mutex> lock(publishMutex);
			if (generation != analysisGeneration) {
				// The module was initialized meanwhile: drop the result.
				delete bank;
			}
			else if (bank) {
				// If process() has not taken the previous result yet, it never saw it, so it is
				// safe to delete here.
				delete pendingBank.exchange(bank);
				setStatus(name + "  " + std::to_string(bank->numTables) + " artifacts");
			}
			else {
				setStatus(error);
			}
		}
		analyzing = false;
	});
}

void WaveArtifact::uiStep() {
	delete retiredBank.exchange(nullptr);
	// The edit area always shows an active marker.
	if (!isMarkerActive(selectedMarker)) {
		for (int i = 0; i < NUM_MARKERS; i++) {
			if (isMarkerActive(i)) {
				selectedMarker = i;
				break;
			}
		}
	}
	// PITCH steps in semitones unless the marker is in free mode (Rack knobs honour snapEnabled while
	// dragging). Switching back to semitones snaps the knob to the nearest one.
	for (int i = 0; i < NUM_MARKERS; i++) {
		Param& pitch = params[MARKER_PITCH_PARAM + i];
		bool free = params[MARKER_PITCH_FREE_PARAM + i].getValue() > 0.5f;
		getParamQuantity(MARKER_PITCH_PARAM + i)->snapEnabled = !free;
		if (!free && pitch.getValue() != std::round(pitch.getValue()))
			pitch.setValue(std::round(pitch.getValue()));
	}
	// A bank with fewer tables (re-analysis) cannot play a preview beyond its end.
	const wa::WavetableBank* bank = activeBank.load();
	int p = previewTable.load();
	if (p >= 0 && (!bank || p >= bank->numTables))
		previewTable = -1;
}

/** Everything per marker except on/off, bar position and wavetable, back to default. */
static void resetMarkerSettings(WaveArtifact* m, int i) {
	for (int id : {WaveArtifact::MARKER_SMOOTH_PARAM, WaveArtifact::MARKER_LFO_RATE_PARAM, WaveArtifact::MARKER_LFO_AMT_PARAM,
			WaveArtifact::MARKER_LFO_SYNC_PARAM, WaveArtifact::MARKER_LFO_TRIPLET_PARAM, WaveArtifact::MARKER_LFO_DOTTED_PARAM,
			WaveArtifact::MARKER_LFO_SHAPE_PARAM,
			WaveArtifact::MARKER_GAIN_PARAM, WaveArtifact::MARKER_CUTOFF_PARAM, WaveArtifact::MARKER_RES_PARAM,
			WaveArtifact::MARKER_FILTER_MODE_PARAM, WaveArtifact::MARKER_DRIVE_PARAM,
			WaveArtifact::MARKER_PITCH_PARAM, WaveArtifact::MARKER_PITCH_FREE_PARAM, WaveArtifact::MARKER_FINE_PARAM, WaveArtifact::MARKER_PAN_PARAM})
		m->params[id + i].setValue(m->getParamQuantity(id + i)->getDefaultValue());
}

void WaveArtifact::autoArrange() {
	// The engine treats newly activated markers as a structural change (5 ms fade out/in) and
	// crossfades the wavetable jumps, so arranging while playing is click-free.
	for (int i = 0; i < NUM_MARKERS; i++) {
		float pos = (float) i / (NUM_MARKERS - 1);
		params[MARKER_ACTIVE_PARAM + i].setValue(1.f);
		params[MARKER_POS_PARAM + i].setValue(pos);
		params[MARKER_WAVE_PARAM + i].setValue(pos);
		resetMarkerSettings(this, i);
	}
}

bool WaveArtifact::isMarkerActive(int i) {
	return params[MARKER_ACTIVE_PARAM + i].getValue() > 0.5f;
}

int WaveArtifact::activeMarkerCount() {
	int n = 0;
	for (int i = 0; i < NUM_MARKERS; i++)
		n += isMarkerActive(i);
	return n;
}

int WaveArtifact::addMarker(float pos) {
	for (int i = 0; i < NUM_MARKERS; i++) {
		if (isMarkerActive(i))
			continue;
		pos = clamp(pos, 0.f, 1.f);
		params[MARKER_POS_PARAM + i].setValue(pos);
		// Like ARRANGE: the bar position picks the same spot in the sample.
		params[MARKER_WAVE_PARAM + i].setValue(pos);
		resetMarkerSettings(this, i);
		params[MARKER_ACTIVE_PARAM + i].setValue(1.f);
		selectedMarker = i;
		return i;
	}
	return -1;
}

void WaveArtifact::deleteMarker(int i) {
	if (activeMarkerCount() <= 1)
		return;
	params[MARKER_ACTIVE_PARAM + i].setValue(0.f);
	// uiStep() moves the selection to an active marker.
}

void WaveArtifact::assignTable(int i, int t) {
	const wa::WavetableBank* bank = activeBank.load();
	if (!bank || t < 0 || t >= bank->numTables)
		return;
	if (!isMarkerActive(i)) {
		// Put it in the widest free stretch of the bar (the bar ends count as stretches).
		float ps[NUM_MARKERS];
		int n = 0;
		for (int k = 0; k < NUM_MARKERS; k++) {
			if (!isMarkerActive(k))
				continue;
			// Insertion sort (at most 5 values)
			float v = params[MARKER_POS_PARAM + k].getValue();
			int j = n++;
			while (j > 0 && ps[j - 1] > v) {
				ps[j] = ps[j - 1];
				j--;
			}
			ps[j] = v;
		}
		float best = 0.5f, bestGap = -1.f;
		if (n > 0) {
			if (ps[0] > bestGap) {
				best = 0.f;
				bestGap = ps[0];
			}
			if (1.f - ps[n - 1] > bestGap) {
				best = 1.f;
				bestGap = 1.f - ps[n - 1];
			}
			for (int k = 0; k + 1 < n; k++) {
				if ((ps[k + 1] - ps[k]) / 2.f > bestGap) {
					best = (ps[k] + ps[k + 1]) / 2.f;
					bestGap = (ps[k + 1] - ps[k]) / 2.f;
				}
			}
		}
		params[MARKER_POS_PARAM + i].setValue(best);
		resetMarkerSettings(this, i);
		params[MARKER_ACTIVE_PARAM + i].setValue(1.f);
	}
	params[MARKER_WAVE_PARAM + i].setValue(bank->numTables > 1 ? (float) t / (bank->numTables - 1) : 0.f);
	selectedMarker = i;
}


int WaveArtifact::markerTable(int i) {
	const wa::WavetableBank* bank = activeBank.load();
	if (!bank)
		return -1;
	return (int) std::round(params[MARKER_WAVE_PARAM + i].getValue() * (bank->numTables - 1));
}

std::string WaveArtifact::getStatus() {
	std::lock_guard<std::mutex> lock(statusMutex);
	return status;
}

void WaveArtifact::setStatus(const std::string& s) {
	std::lock_guard<std::mutex> lock(statusMutex);
	status = s;
}

void openLoadDialog(WaveArtifact* module) {
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

static ModeButton* createModeButton(Vec mmCenter, Module* module, int paramId, std::vector<std::string> labels, NVGcolor color) {
	ModeButton* b = createParam<ModeButton>(Vec(0, 0), module, paramId);
	b->box.size = mm2px(layout::MODE_BUTTON_SIZE);
	b->box.pos = mm2px(mmCenter).minus(b->box.size.div(2.f));
	b->labels = labels;
	b->color = color;
	return b;
}

/** Numbered dot in an artifact's color, labelling its output pair. */
struct ArtifactNumber : widget::Widget {
	int index = 0;

	void draw(const DrawArgs& args) override {
		Vec c = box.size.div(2.f);
		float r = std::min(box.size.x, box.size.y) / 2.f;
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, c.x, c.y, r);
		nvgFillColor(args.vg, layout::MARKER_COLORS[index]);
		nvgFill(args.vg);
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		wadraw::drawCenteredNumber(args.vg, font->handle, c.x, c.y, r * 1.4f, std::to_string(index + 1), nvgRGB(0x16, 0x18, 0x22));
	}
};

struct WaveArtifactWidget : ModuleWidget {
	static const int EDIT_KNOBS = 17;
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
		labels->add(Vec(PANEL_W / 2.f, 6.f), "Wave Artifacts", 13.f);
		// Marker sections, row 1
		labels->add(Vec(WAVE_KNOB.x, ROW1_LABEL_Y), "WAVE");
		labels->add(Vec(GAIN_KNOB.x, ROW1_LABEL_Y), "GAIN");
		labels->add(Vec(CUTOFF_KNOB.x, ROW1_LABEL_Y), "CUTOFF");
		labels->add(Vec(DRIVE_KNOB.x, ROW1_LABEL_Y), "DRIVE");
		labels->add(Vec(PITCH_KNOB.x, ROW1_LABEL_Y), "PITCH");
		labels->add(Vec(PAN_KNOB.x, ROW2_LABEL_Y), "PAN", 6.f);
		// Row 2
		labels->add(Vec(LFO_SHAPE_SCREEN.x, ROW1_LABEL_Y), "LFO SHAPE", 6.f);
		labels->add(Vec(LFO_RATE_KNOB.x, ROW1_LABEL_Y), "RATE", 6.f);
		labels->add(Vec(LFO_AMT_KNOB.x, ROW2_LABEL_Y), "AMT", 6.f);
		labels->add(Vec(SMOOTH_KNOB.x, ROW2_LABEL_Y), "SMOOTH", 6.f);
		labels->add(Vec(RES_KNOB.x, ROW2_LABEL_Y), "RES", 6.f);
		labels->add(Vec(FILTER_MODE_BUTTON.x, ROW2_LABEL_Y), "TYPE", 6.f);
		labels->add(Vec(FINE_KNOB.x, ROW2_LABEL_Y), "FINE", 6.f);
		labels->add(Vec(CLOCK_INPUT.x, ROW2_LABEL_Y + 0.5f), "CLOCK", 6.f);
		labels->add(Vec(BPM_KNOB.x, ROW1_LABEL_Y), "BPM", 6.f);
		labels->add(Vec(CLOCK_OUTPUT.x, ROW1_LABEL_Y - 0.4f), "CLK OUT", 5.f, true);
		// Controls band
		labels->add(Vec(POSITION_KNOB.x, CONTROL_LABEL_Y), "POSITION");
		labels->add(Vec(POS_ATTEN.x, POS_CV_LABEL_Y), "CV AMT", 6.f);
		labels->add(Vec(POS_LFO_SHAPE_SCREEN.x, POS_LFO_LABEL_Y), "POS LFO", 6.f);
		labels->add(Vec(POS_LFO_RATE_KNOB.x, POS_LFO_KNOB_LABEL_Y), "RATE", 6.f);
		labels->add(Vec(POS_LFO_AMT_KNOB.x, POS_LFO_KNOB_LABEL_Y), "AMT", 6.f);
		labels->add(Vec(POS_INPUT.x, CONTROL_LABEL_Y), "POS CV", 6.f);
		labels->add(Vec(FREQ_KNOB.x, CONTROL_LABEL_Y), "FREQ");
		labels->add(Vec(VOCT_INPUT.x, CONTROL_LABEL_Y), "V/OCT");
		labels->add(Vec(MASTER_MODE_BUTTON.x, MASTER_LABEL_Y), "MASTER", 6.f);
		const char* masterNames[5] = {"CUTOFF", "RES", "DRIVE", "SPREAD", "FDBK"};
		for (int k = 0; k < 5; k++)
			labels->add(Vec(MASTER_COL_X[k], MASTER_LABEL_Y), masterNames[k], 6.f);
		labels->add(Vec(MASTER_GAIN_KNOB.x, MASTER_GAIN_LABEL_Y), "GAIN", 6.f, true);
		labels->add(Vec(LEFT_OUTPUT.x, OUTPUT_LABEL_Y), "L", 6.f, true);
		labels->add(Vec(RIGHT_OUTPUT.x, OUTPUT_LABEL_Y), "R", 6.f, true);
		// Artifact outputs column
		labels->add(Vec(ARTOUT_BOX_POS.x + ARTOUT_BOX_SIZE.x / 2.f, ARTOUT_HEADER_Y), "ARTIFACT OUT", 5.f, true);
		labels->add(Vec(ARTOUT_L_X, ARTOUT_LR_LABEL_Y), "L", 6.f, true);
		labels->add(Vec(ARTOUT_R_X, ARTOUT_LR_LABEL_Y), "R", 6.f, true);
		addChild(labels);

		// Per-artifact sections outlined in the selected artifact's color (global controls are not).
		ArtifactSections* sections = createWidget<ArtifactSections>(Vec(0, 0));
		sections->box.size = box.size;
		sections->module = module;
		addChild(sections);

		MarkerBar* bar = createWidget<MarkerBar>(mm2px(SCREEN_POS));
		bar->box.size = mm2px(SCREEN_SIZE);
		bar->module = module;
		addChild(bar);

		WavetableMap* dots = createWidget<WavetableMap>(mm2px(DOTS_POS));
		dots->box.size = mm2px(DOTS_SIZE);
		dots->module = module;
		addChild(dots);

		// The selected artifact fills its whole section (color + large number).
		MarkerBadge* badge = createWidget<MarkerBadge>(mm2px(Vec(TOP_SECTIONS[0].x0, TOP_Y0)));
		badge->box.size = mm2px(Vec(TOP_SECTIONS[0].x1 - TOP_SECTIONS[0].x0, TOP_Y1 - TOP_Y0));
		badge->module = module;
		addChild(badge);

		// One knob set per marker at the same spot; step() shows the selected marker's set.
		for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
			int k = 0;
			auto add = [&](ParamWidget* w) {
				editKnobs[i][k++] = w;
				addParam(w);
				w->setVisible(i == 0);
			};
			add(withArc(createParamCentered<RoundBlackKnob>(mm2px(WAVE_KNOB), module, WaveArtifact::MARKER_WAVE_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createLfoRateKnob<LfoRateKnobT<Trimpot>>(LFO_RATE_KNOB, module, WaveArtifact::MARKER_LFO_RATE_PARAM + i,
				WaveArtifact::MARKER_LFO_SYNC_PARAM + i, WaveArtifact::MARKER_LFO_TRIPLET_PARAM + i,
				WaveArtifact::MARKER_LFO_DOTTED_PARAM + i, WaveArtifact::MARKER_LFO_AMT_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createParamCentered<Trimpot>(mm2px(LFO_AMT_KNOB), module, WaveArtifact::MARKER_LFO_AMT_PARAM + i), MARKER_COLORS[i]));
			add(createLfoShapeScreen(LFO_SHAPE_SCREEN, LFO_SHAPE_SIZE, module, WaveArtifact::MARKER_LFO_SHAPE_PARAM + i, MARKER_COLORS[i]));
			add(createLitToggle(LFO_SYNC_BUTTON, module, WaveArtifact::MARKER_LFO_SYNC_PARAM + i, "SYNC", MARKER_COLORS[i]));
			add(createLitToggle(LFO_TRIP_BUTTON, module, WaveArtifact::MARKER_LFO_TRIPLET_PARAM + i, "T", MARKER_COLORS[i], LFO_TOGGLE_SMALL));
			add(createLitToggle(LFO_DOT_BUTTON, module, WaveArtifact::MARKER_LFO_DOTTED_PARAM + i, "D", MARKER_COLORS[i], LFO_TOGGLE_SMALL));
			add(withArc(createParamCentered<Trimpot>(mm2px(SMOOTH_KNOB), module, WaveArtifact::MARKER_SMOOTH_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createParamCentered<RoundBlackKnob>(mm2px(GAIN_KNOB), module, WaveArtifact::MARKER_GAIN_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createParamCentered<RoundBlackKnob>(mm2px(CUTOFF_KNOB), module, WaveArtifact::MARKER_CUTOFF_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createParamCentered<RoundBlackKnob>(mm2px(DRIVE_KNOB), module, WaveArtifact::MARKER_DRIVE_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createParamCentered<RoundSmallBlackKnob>(mm2px(RES_KNOB), module, WaveArtifact::MARKER_RES_PARAM + i), MARKER_COLORS[i]));
			add(createModeButton(FILTER_MODE_BUTTON, module, WaveArtifact::MARKER_FILTER_MODE_PARAM + i, {"LP", "BP", "HP", "NOTCH"}, MARKER_COLORS[i]));
			add(withArc(createParamCentered<RoundBlackKnob>(mm2px(PITCH_KNOB), module, WaveArtifact::MARKER_PITCH_PARAM + i), MARKER_COLORS[i], true));
			add(withArc(createParamCentered<Trimpot>(mm2px(FINE_KNOB), module, WaveArtifact::MARKER_FINE_PARAM + i), MARKER_COLORS[i], true));
			add(createLitToggle(PITCH_FREE_BUTTON, module, WaveArtifact::MARKER_PITCH_FREE_PARAM + i, "FREE", MARKER_COLORS[i]));
			add(withArc(createParamCentered<RoundSmallBlackKnob>(mm2px(PAN_KNOB), module, WaveArtifact::MARKER_PAN_PARAM + i), MARKER_COLORS[i], true));
		}

		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(CLOCK_INPUT), module, WaveArtifact::CLOCK_INPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(CLOCK_OUTPUT), module, WaveArtifact::CLOCK_OUTPUT));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(RESET_INPUT), module, WaveArtifact::RESET_INPUT));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(BPM_KNOB), module, WaveArtifact::INTERNAL_BPM_PARAM));
		addParam(createLitToggle<MomentaryLitButton>(RESTART_BUTTON, module, WaveArtifact::RESTART_PARAM, "RESET", SCREEN_TEXT, RESTART_BUTTON_SIZE));

		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(POSITION_KNOB), module, WaveArtifact::POSITION_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(POS_ATTEN), module, WaveArtifact::POS_ATTEN_PARAM));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(POS_INPUT), module, WaveArtifact::POS_INPUT));
		// Position LFO (amount, SYNC and note kinds also in RATE's right-click menu)
		addParam(createLfoShapeScreen(POS_LFO_SHAPE_SCREEN, POS_LFO_SHAPE_SIZE, module, WaveArtifact::POS_LFO_SHAPE_PARAM, SCREEN_TEXT));
		addParam(createLitToggle(POS_LFO_SYNC_BUTTON, module, WaveArtifact::POS_LFO_SYNC_PARAM, "SYNC", SCREEN_TEXT));
		addParam(createLitToggle(POS_LFO_TRIP_BUTTON, module, WaveArtifact::POS_LFO_TRIPLET_PARAM, "T", SCREEN_TEXT, LFO_TOGGLE_SMALL));
		addParam(createLitToggle(POS_LFO_DOT_BUTTON, module, WaveArtifact::POS_LFO_DOTTED_PARAM, "D", SCREEN_TEXT, LFO_TOGGLE_SMALL));
		addParam(createLfoRateKnob<LfoRateKnobT<Trimpot>>(POS_LFO_RATE_KNOB, module, WaveArtifact::POS_LFO_RATE_PARAM,
			WaveArtifact::POS_LFO_SYNC_PARAM, WaveArtifact::POS_LFO_TRIPLET_PARAM, WaveArtifact::POS_LFO_DOTTED_PARAM,
			WaveArtifact::POS_LFO_AMT_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(POS_LFO_AMT_KNOB), module, WaveArtifact::POS_LFO_AMT_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(FREQ_KNOB), module, WaveArtifact::FREQ_PARAM));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(VOCT_INPUT), module, WaveArtifact::VOCT_INPUT));

		addParam(createModeButton(MASTER_MODE_BUTTON, module, WaveArtifact::MASTER_MODE_PARAM, {"OFF", "LP", "BP", "HP", "NOTCH"}, SCREEN_TEXT));
		// Knob order matches MASTER_COL_X: cutoff, res, drive, spread, fdbk.
		const int masterKnob[5] = {WaveArtifact::MASTER_CUTOFF_PARAM, WaveArtifact::MASTER_RES_PARAM, WaveArtifact::MASTER_DRIVE_PARAM,
			WaveArtifact::MASTER_SPREAD_PARAM, WaveArtifact::MASTER_FDBK_PARAM};
		for (int k = 0; k < 5; k++) {
			addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(MASTER_COL_X[k], MASTER_KNOB_Y)), module, masterKnob[k]));
			addParam(createParamCentered<Trimpot>(mm2px(Vec(MASTER_COL_X[k], MASTER_AMT_Y)), module, WaveArtifact::MASTER_CUTOFF_AMT_PARAM + k));
			addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(MASTER_COL_X[k], MASTER_CV_Y)), module, WaveArtifact::MASTER_CUTOFF_INPUT + k));
		}

		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(MASTER_GAIN_KNOB), module, WaveArtifact::MASTER_GAIN_PARAM));
		addParam(createLitToggle(STOP_BUTTON, module, WaveArtifact::STOP_PARAM, "STOP", STOP_COLOR, STOP_BUTTON_SIZE));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(LEFT_OUTPUT), module, WaveArtifact::LEFT_OUTPUT));
		addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(RIGHT_OUTPUT), module, WaveArtifact::RIGHT_OUTPUT));

		for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
			float y = ARTOUT_ROW0_Y + i * ARTOUT_ROW_STEP;
			ArtifactNumber* num = createWidget<ArtifactNumber>(mm2px(Vec(ARTOUT_BOX_POS.x + ARTOUT_BOX_SIZE.x / 2.f - ARTOUT_BADGE_R, y + ARTOUT_BADGE_DY - ARTOUT_BADGE_R)));
			num->box.size = mm2px(Vec(2.f * ARTOUT_BADGE_R, 2.f * ARTOUT_BADGE_R));
			num->index = i;
			addChild(num);
			addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(ARTOUT_L_X, y)), module, WaveArtifact::ARTIFACT_L_OUTPUT + i));
			addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(ARTOUT_R_X, y)), module, WaveArtifact::ARTIFACT_R_OUTPUT + i));
		}
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
		menu->addChild(createIndexSubmenuItem("Artifact count",
			{"Auto (50-100)", "50", "64", "100"},
			[=]() {
				return (size_t) m->tableCountMode;
			},
			[=](size_t mode) {
				m->tableCountMode = (wa::TableCountMode) mode;
				m->startAnalysis();
			}
		));
		menu->addChild(createMenuItem("Restart LFOs", "beat grid + all LFOs", [=]() {
			m->restartRequest = true;
		}));
		menu->addChild(createIndexSubmenuItem("Clock resolution (CLOCK input)",
			{"1 PPQN (quarter notes)", "2 PPQN (eighths)", "4 PPQN (sixteenths)", "8 PPQN", "12 PPQN", "24 PPQN (MIDI clock)", "48 PPQN"},
			[=]() {
				return (size_t) clamp((int) std::round(m->params[WaveArtifact::CLOCK_PPQN_PARAM].getValue()), 0, wa::NUM_PPQN - 1);
			},
			[=](size_t i) {
				m->params[WaveArtifact::CLOCK_PPQN_PARAM].setValue((float) i);
			}
		));
		menu->addChild(createIndexSubmenuItem("Artifact tone",
			{"Natural", "Smooth (default)", "Soft"},
			[=]() {
				return (size_t) m->tableTone;
			},
			[=](size_t tone) {
				m->tableTone = (wa::TableTone) tone;
				m->startAnalysis();
			}
		));
		menu->addChild(createMenuItem("Auto arrange artifacts", "resets their settings", [=]() {
			m->autoArrange();
		}));
		if (m->previewTable.load() >= 0) {
			menu->addChild(createMenuItem("Stop artifact preview", "", [=]() {
				m->previewTable = -1;
			}));
		}

		// Expanders already snapped on in a row to the right.
		int count = 0;
		Module* last = m;
		while (count < wa::MAX_EXPANDERS && isWaveArtifactExpander(last->rightExpander.module)) {
			last = last->rightExpander.module;
			count++;
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Add expander", string::f("%d / %d", count, wa::MAX_EXPANDERS), [=]() {
			addExpander();
		}, count >= wa::MAX_EXPANDERS));
	}

	/** Creates an expander at the right end of the chain (pushing anything there aside), set to the
	first marker no expander in the chain uses yet. Undoable. */
	void addExpander() {
		WaveArtifact* m = getModule<WaveArtifact>();
		if (!m)
			return;
		bool used[WaveArtifact::NUM_MARKERS] = {};
		int count = 0;
		Module* last = m;
		while (isWaveArtifactExpander(last->rightExpander.module)) {
			last = last->rightExpander.module;
			used[((WaveArtifactExpander*) last)->getMarker()] = true;
			if (++count >= wa::MAX_EXPANDERS)
				return;
		}
		int marker = count % WaveArtifact::NUM_MARKERS;
		for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
			if (!used[i]) {
				marker = i;
				break;
			}
		}
		ModuleWidget* lastWidget = APP->scene->rack->getModule(last->id);
		if (!lastWidget)
			return;

		Model* model = modelWaveArtifactExpander;
		Module* em = model->createModule();
		em->params[WaveArtifactExpander::MARKER_PARAM].setValue((float) marker);
		APP->engine->addModule(em);
		ModuleWidget* emw = model->createModuleWidget(em);
		APP->scene->rack->addModule(emw);
		APP->scene->rack->setModulePosForce(emw, Vec(lastWidget->box.getRight(), lastWidget->box.pos.y));

		history::ModuleAdd* h = new history::ModuleAdd;
		h->name = "add Wave Artifacts expander";
		h->setModule(emw);
		APP->history->push(h);
	}
};

Model* modelWaveArtifact = createModel<WaveArtifact, WaveArtifactWidget>("waveArtifact");
