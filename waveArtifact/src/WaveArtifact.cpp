#include <osdialog.h>

#include "Layout.hpp"
#include "WaveArtifact.hpp"
#include "WaveArtifactExpander.hpp"
#include "widgets/AdsrScreen.hpp"
#include "widgets/ArtifactColor.hpp"
#include "widgets/WavetableMap.hpp"
#include "widgets/GrainControls.hpp"
#include "widgets/LfoControls.hpp"
#include "widgets/LinkedKnob.hpp"
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
	/** Name in the granular engine, where the same value is the grain START (a place in the sample) */
	std::string grainName;
	bool grain() {
		return module && module->params[WaveArtifact::ENGINE_MODE_PARAM].getValue() > 0.5f;
	}
	std::string getLabel() override {
		return grain() ? grainName : ParamQuantity::getLabel();
	}
	std::string getDisplayValueString() override {
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(module);
		const wa::WavetableBank* bank = m ? m->activeBank.load() : nullptr;
		if (!bank)
			return string::f("%.0f%%", getValue() * 100.f);
		if (grain())
			return string::f("%.2f s of %.2f s", getValue() * bank->sourceSeconds, bank->sourceSeconds);
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
		configParam<WaveQuantity>(MARKER_WAVE_PARAM + i, 0.f, 1.f, DEFAULT_MARKER_POS[i], name + " artifact")->grainName = name + " grain START";
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

		// Granular engine (the knobs that change meaning on the GRAIN engine). Defaults: synced, a grain per
		// 1/16 note, 1.7 grains overlapping, frozen at START.
		{
			std::string g = name + " grain ";
			auto sync = [&](GrainSyncQuantity* q) {
				q->syncId = MARKER_GRAIN_SYNC_PARAM + i;
				q->tripletId = MARKER_GRAIN_TRIPLET_PARAM + i;
				q->dottedId = MARKER_GRAIN_DOTTED_PARAM + i;
			};
			sync(configParam<GrainDensityQuantity>(MARKER_GRAIN_DENSITY_PARAM + i, 0.f, 1.f, 0.85f, g + "density"));
			sync(configParam<GrainSizeQuantity>(MARKER_GRAIN_SIZE_PARAM + i, 0.f, 1.f, 0.55f, g + "size"));
			configParam<GrainSprayQuantity>(MARKER_GRAIN_SPRAY_PARAM + i, 0.f, 1.f, 0.f, g + "spray (random start offset)");
			configSwitch(MARKER_GRAIN_WINDOW_PARAM + i, 0.f, wa::NUM_GRAIN_WINDOWS - 1, 0.f, g + "window", grainWindowNames());
			configSwitch(MARKER_GRAIN_SYNC_PARAM + i, 0.f, 1.f, 1.f, g + "sync", {"Free (grains per second)", "Tempo sync (BPM / CLOCK)"});
			configSwitch(MARKER_GRAIN_TRIPLET_PARAM + i, 0.f, 1.f, 0.f, g + "triplets", {"Off", "On"});
			configSwitch(MARKER_GRAIN_DOTTED_PARAM + i, 0.f, 1.f, 0.f, g + "dotted", {"Off", "On"});
			configParam(MARKER_GRAIN_SPREAD_PARAM + i, 0.f, 1.f, 0.f, g + "stereo spread", "%", 0.f, 100.f);
			configParam<GrainPitchScatterQuantity>(MARKER_GRAIN_PSCATTER_PARAM + i, 0.f, 1.f, 0.f, g + "pitch scatter");
			sync(configParam<GrainScanQuantity>(MARKER_GRAIN_SCAN_PARAM + i, -1.f, 1.f, 0.f, g + "scan"));
			configSwitch(MARKER_GRAIN_DIR_PARAM + i, 0.f, wa::NUM_GRAIN_DIRECTIONS - 1, 0.f, g + "direction", grainDirectionNames());
			for (int id : {MARKER_GRAIN_WINDOW_PARAM, MARKER_GRAIN_SYNC_PARAM, MARKER_GRAIN_TRIPLET_PARAM, MARKER_GRAIN_DOTTED_PARAM, MARKER_GRAIN_DIR_PARAM})
				getParamQuantity(id + i)->randomizeEnabled = false;
		}

		// POLY: each marker's note. Defaults: 5 ms attack, 1 s at full level, 300 ms tail.
		configParam(MARKER_ATTACK_PARAM + i, 0.f, 1.f, 0.18896f, name + " attack (POLY)", " s", wa::ADSR_ATTACK_RATIO, wa::ADSR_ATTACK_MIN);
		configParam(MARKER_DECAY_PARAM + i, 0.f, 1.f, 0.48533f, name + " decay (POLY)", " s", wa::ADSR_FALL_RATIO, wa::ADSR_FALL_MIN);
		configParam(MARKER_SUSTAIN_PARAM + i, 0.f, 1.f, 1.f, name + " sustain (POLY)", "%", 0.f, 100.f);
		configParam(MARKER_RELEASE_PARAM + i, 0.f, 1.f, 0.53866f, name + " release (POLY)", " s", wa::ADSR_FALL_RATIO, wa::ADSR_FALL_MIN);
		configParam<AdsrHoldQuantity>(MARKER_HOLD_PARAM + i, 0.f, 1.f, 0.66667f, name + " note length (POLY)");

		// Randomizing these would shuffle the bar layout, not the sound.
		getParamQuantity(MARKER_ACTIVE_PARAM + i)->randomizeEnabled = false;
		getParamQuantity(MARKER_POS_PARAM + i)->randomizeEnabled = false;
	}

	configSwitch(PLAY_MODE_PARAM, 0.f, 1.f, 0.f, "Play mode", {"Morph (the playhead crossfades neighboring markers)", "Poly (the playhead starts each marker's own note)"});
	getParamQuantity(PLAY_MODE_PARAM)->randomizeEnabled = false;
	configSwitch(ENGINE_MODE_PARAM, 0.f, 1.f, 0.f, "Engine", {"Wavetable (WAVE)", "Granular (GRAIN)"});
	getParamQuantity(ENGINE_MODE_PARAM)->randomizeEnabled = false;
	configSwitch(GRAIN_SEEDED_PARAM, 0.f, 1.f, 1.f, "Grain scatter repeats every loop cycle while synced", {"Off (continuous random)", "On (seeded)"});
	getParamQuantity(GRAIN_SEEDED_PARAM)->randomizeEnabled = false;

	configSwitch(MASTER_MODE_PARAM, 0.f, 4.f, 1.f, "Master filter type", {"Off", "Low-pass", "Band-pass", "High-pass", "Notch"});
	configParam(MASTER_CUTOFF_PARAM, 0.f, 1.f, 1.f, "Master filter cutoff", " Hz", 1000.f, 20.f);
	configParam(MASTER_RES_PARAM, 0.f, 1.f, 0.f, "Master filter resonance", "%", 0.f, 100.f);
	configParam(MASTER_SPREAD_PARAM, -1.f, 1.f, 0.f, "Master filter spread (right above left)", " oct", 0.f, 2.f);
	configParam(MASTER_FDBK_PARAM, 0.f, 1.f, 0.f, "Master filter feedback (self-oscillates above ~75%)", "%", 0.f, 100.f);
	configParam(MASTER_DRIVE_PARAM, 0.f, 1.f, 0.f, "Master drive (warm saturation)", "%", 0.f, 100.f);
	configParam(MASTER_GAIN_PARAM, 0.f, 2.f, 1.f, "Master gain", "%", 0.f, 100.f);
	// Bipolar like an attenuverter: -100% turns an envelope into a ducker.
	configParam(MASTER_GAIN_AMT_PARAM, -1.f, 1.f, 0.f, "Master gain CV amount (10 V at 100% = +200%)", "%", 0.f, 100.f);
	configInput(MASTER_GAIN_INPUT, "Master gain CV (VCA: set GAIN to 0% and AMT to 100% for 0..10 V = 0..200%)");
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
	// 20 ms .. 2 s, log. Default 0.2 (50 ms): rounds the edge of a saw / square so POSITION does not jump.
	configParam(POS_LFO_SMOOTH_PARAM, 0.f, 1.f, 0.2f, "Position LFO smoothing", " ms", 100.f, 1000.f * wa::MIN_WAVE_SMOOTH);
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
	// Randomize must not silence the module.
	getParamQuantity(STOP_PARAM)->randomizeEnabled = false;
	configOutput(LEFT_OUTPUT, "Left");
	configOutput(RIGHT_OUTPUT, "Right");
	configOutput(CLOCK_OUTPUT, "Clock out (one 10 V trigger per beat: internal BPM, or the CLOCK input)");
	for (int i = 0; i < NUM_MARKERS; i++) {
		std::string name = "Artifact " + std::to_string(i + 1);
		configOutput(ARTIFACT_L_OUTPUT + i, name + " left (follows POSITION; mono when right is unpatched; patched = leaves the main L/R)");
		configOutput(ARTIFACT_R_OUTPUT + i, name + " right (follows POSITION; patched = leaves the main L/R)");
	}

	for (int i = 0; i < NUM_MARKERS; i++) {
		shownIndex[i].store(0.f);
		shownCv[i].store(false);
		shownHeard[i].store(0.f);
		shownGrainRead[i].store(0.f);
		shownGrainPos[i].store(0.f);
		shownGrainSeconds[i].store(0.f);
		shownGrainCount[i].store(0);
	}
	// Modulation arrives from expanders on the right.
	rightExpander.producerMessage = new wa::ToMainMessage;
	rightExpander.consumerMessage = new wa::ToMainMessage;
	setStatus("Load a WAV (max 10 MB)");
}

void WaveArtifact::onRandomize(const RandomizeEvent& e) {
	Module::onRandomize(e);
	// Rack calls this with the engine locked, so the expanders' params can be set directly. They
	// are walked in chain order, like the messages; each applies its own randomizeEnabled flags.
	Module* last = this;
	for (int n = 0; n < wa::MAX_EXPANDERS && isWaveArtifactExpander(last->rightExpander.module); n++) {
		last = last->rightExpander.module;
		last->onRandomize(e);
	}
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
	filePath.clear();
	folderWavs.clear();
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
	if (args.sampleRate != grainEngine.getSampleRate())
		grainEngine.setSampleRate(args.sampleRate);

	// A new bank waits until the engine has faded to silence. Swap only once the UI has freed
	// the previous retired bank, so nothing is ever deleted on the audio thread.
	bool swapWanted = pendingBank.load(std::memory_order_relaxed) && !retiredBank.load();
	// Engine switch: the engine in use fades out (5 ms), then the other one takes over.
	int wantEngine = params[ENGINE_MODE_PARAM].getValue() > 0.5f ? 1 : 0;
	bool switching = wantEngine != engineApplied;

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
		out->granular = engineApplied == 1;
		out->poly = params[PLAY_MODE_PARAM].getValue() > 0.5f;
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
	// POLY listens to the LFO before its SMOOTH: a saw's reset is then one jump that starts one marker, not
	// a fast sweep that starts them all. (No click to smooth away here: the notes have their own ADSR.)
	float triggerPosition = position;
	{
		int shape = (int) std::round(params[POS_LFO_SHAPE_PARAM].getValue());
		bool sync = params[POS_LFO_SYNC_PARAM].getValue() > 0.5f;
		bool triplets = params[POS_LFO_TRIPLET_PARAM].getValue() > 0.5f;
		bool dotted = params[POS_LFO_DOTTED_PARAM].getValue() > 0.5f;
		float rawLfo = posLfo.process(shape, sync, params[POS_LFO_RATE_PARAM].getValue(), triplets, dotted, clock, args.sampleTime);
		float lfo = rawLfo;
		// SMOOTH acts on the LFO alone (knob and CV stay fast): a saw / square edge would otherwise
		// throw the playhead across the markers in a few ms, which is heard as a click.
		lfo = posLfoSmoother.process(lfo, wa::MIN_WAVE_SMOOTH * std::exp(params[POS_LFO_SMOOTH_PARAM].getValue() * LN_100), args.sampleTime);
		float amt = params[POS_LFO_AMT_PARAM].getValue();
		if (amt > 0.f) {
			triggerPosition = wa::lfoCentered(position, amt, rawLfo);
			position = wa::lfoCentered(position, amt, lfo);
		}
	}
	in.position = position;
	// POLY: which markers the playhead starts, and each one's ADSR. Leaving POLY keeps the last levels while
	// the engine fades out, so nothing is cut off.
	bool polyWanted = params[PLAY_MODE_PARAM].getValue() > 0.5f;
	if (polyWanted) {
		if (!polyRunning)
			voices.reset();
		float markerPos[NUM_MARKERS];
		bool markerOn[NUM_MARKERS];
		wa::AdsrSettings adsr[NUM_MARKERS];
		for (int i = 0; i < NUM_MARKERS; i++) {
			markerPos[i] = in.markers[i].pos;
			markerOn[i] = in.markers[i].active;
			adsr[i].attack = wa::adsrAttackSeconds(params[MARKER_ATTACK_PARAM + i].getValue());
			adsr[i].decay = wa::adsrFallSeconds(params[MARKER_DECAY_PARAM + i].getValue());
			adsr[i].sustain = params[MARKER_SUSTAIN_PARAM + i].getValue();
			adsr[i].release = wa::adsrFallSeconds(params[MARKER_RELEASE_PARAM + i].getValue());
			adsr[i].hold = wa::adsrHoldSeconds(params[MARKER_HOLD_PARAM + i].getValue());
		}
		// Expander GATEs and a press on a slot play markers directly.
		wa::VoiceGates direct;
		int pressed = pressedMarker.load(std::memory_order_relaxed);
		for (int i = 0; i < NUM_MARKERS; i++) {
			direct.patched[i] = mod->gatePatched[i];
			direct.high[i] = mod->gateHigh[i] || pressed == i;
		}
		voices.process(markerPos, markerOn, triggerPosition, adsr, args.sampleTime, &direct);
	}
	polyRunning = polyWanted;
	in.poly = polyWanted;
	for (int i = 0; i < NUM_MARKERS; i++) {
		in.voiceEnv[i] = voices.env[i];
		in.voiceStart[i] = polyWanted && voices.start[i];
	}
	in.voct = params[FREQ_PARAM].getValue() + inputs[VOCT_INPUT].getVoltage();
	in.bank = activeBank.load(std::memory_order_relaxed);
	// STOP uses the same click-free fade as a bank swap (main, artifact outputs and preview).
	in.holdSilent = swapWanted || switching || params[STOP_PARAM].getValue() > 0.5f;
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
	// Gain CV: 10 V at 100% sweeps the whole knob range (0..200%). The engine clamps to 0..200% and
	// smooths it, so a gate cannot click. A NaN would stick in the smoother forever: treat it as 0.
	if (inputs[MASTER_GAIN_INPUT].isConnected()) {
		float g = in.masterGain + params[MASTER_GAIN_AMT_PARAM].getValue() * inputs[MASTER_GAIN_INPUT].getVoltage() / 10.f * 2.f;
		in.masterGain = std::isfinite(g) ? g : 0.f;
	}
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
	if (engineApplied == 1) {
		// Granular: the same markers, sections and master, with the knobs reinterpreted (see GrainEngine.hpp).
		wa::GrainInput gin;
		for (int i = 0; i < NUM_MARKERS; i++) {
			const wa::MarkerInput& wm = in.markers[i];
			wa::GrainMarkerInput& g = gin.markers[i];
			g.active = wm.active;
			g.pos = wm.pos;
			// START is the marker's WAVE value (plus the expander's WAVE CV), folded at the ends
			g.start = wa::fold01(wm.wave);
			// The expander's FINE input is DENSITY here: 0..10 V at AMT 100% adds 0..1 of the knob. (SCAN has no
			// CV: changing its speed or loop length makes the read position jump. START is for moving by CV.)
			g.density = clamp(params[MARKER_GRAIN_DENSITY_PARAM + i].getValue() + 0.5f * modOf(i, wa::MOD_FINE), 0.f, 1.f);
			g.size = params[MARKER_GRAIN_SIZE_PARAM + i].getValue();
			g.spray = params[MARKER_GRAIN_SPRAY_PARAM + i].getValue();
			g.window = clamp((int) std::round(params[MARKER_GRAIN_WINDOW_PARAM + i].getValue()), 0, wa::NUM_GRAIN_WINDOWS - 1);
			g.sync = params[MARKER_GRAIN_SYNC_PARAM + i].getValue() > 0.5f;
			g.triplet = params[MARKER_GRAIN_TRIPLET_PARAM + i].getValue() > 0.5f;
			g.dotted = params[MARKER_GRAIN_DOTTED_PARAM + i].getValue() > 0.5f;
			// The expander's DRIVE input drives P.SPRD while granular. PAN is PAN on both engines.
			g.pitchScatter = clamp(params[MARKER_GRAIN_PSCATTER_PARAM + i].getValue() + modOf(i, wa::MOD_DRIVE), 0.f, 1.f);
			g.scan = params[MARKER_GRAIN_SCAN_PARAM + i].getValue();
			g.spread = params[MARKER_GRAIN_SPREAD_PARAM + i].getValue();
			g.pan = wm.pan;
			g.direction = clamp((int) std::round(params[MARKER_GRAIN_DIR_PARAM + i].getValue()), 0, wa::NUM_GRAIN_DIRECTIONS - 1);
			g.cutoff = wm.cutoff;
			g.filterMode = wm.filterMode;
			g.res = wm.res;
			g.gain = wm.gain;
			g.pitch = wm.pitch;
			g.voct = wm.voct;
		}
		gin.position = in.position;
		gin.voct = in.voct;
		gin.bank = in.bank;
		gin.clock = &clock;
		gin.reset = reset;
		gin.seeded = params[GRAIN_SEEDED_PARAM].getValue() > 0.5f;
		gin.holdSilent = in.holdSilent;
		gin.poly = in.poly;
		for (int i = 0; i < NUM_MARKERS; i++) {
			gin.wantDirect[i] = in.wantDirect[i];
			gin.voiceEnv[i] = in.voiceEnv[i];
			gin.voiceStart[i] = in.voiceStart[i];
		}
		gin.masterMode = in.masterMode;
		gin.masterCutoff = in.masterCutoff;
		gin.masterRes = in.masterRes;
		gin.masterSpread = in.masterSpread;
		gin.masterFdbk = in.masterFdbk;
		gin.masterDrive = in.masterDrive;
		gin.masterGain = in.masterGain;
		gin.masterCutoffMod = in.masterCutoffMod;
		gin.masterResMod = in.masterResMod;
		gin.masterSpreadMod = in.masterSpreadMod;
		gin.masterFdbkMod = in.masterFdbkMod;
		gin.masterDriveMod = in.masterDriveMod;
		grainEngine.process(gin, &l, &r);
	}
	else {
		engine.process(in, &l, &r);
	}
	bool activeSilent = (engineApplied == 1) ? grainEngine.isSilent() : engine.isSilent();
	if (switching && activeSilent)
		engineApplied = wantEngine;
	outputs[LEFT_OUTPUT].setVoltage(5.f * l);
	outputs[RIGHT_OUTPUT].setVoltage(5.f * r);
	for (int i = 0; i < NUM_MARKERS; i++) {
		float dl, dr;
		if (engineApplied == 1)
			grainEngine.getDirect(i, &dl, &dr);
		else
			engine.getDirect(i, &dl, &dr);
		bool stereo = outputs[ARTIFACT_R_OUTPUT + i].isConnected();
		outputs[ARTIFACT_L_OUTPUT + i].setVoltage(5.f * (stereo ? dl : 0.5f * (dl + dr)));
		outputs[ARTIFACT_R_OUTPUT + i].setVoltage(5.f * dr);
	}

	// Display values, a few hundred times a second is plenty for a 60 Hz screen.
	if (++displayCounter >= 64) {
		displayCounter = 0;
		if (engineApplied == 1) {
			const wa::WavetableBank* bank = in.bank;
			float tables = bank ? (float) std::max(bank->numTables - 1, 0) : 0.f;
			for (int i = 0; i < NUM_MARKERS; i++) {
				// The old displays think in tables: the table under the place each cloud reads
				shownIndex[i].store(grainEngine.getReadPosition(i) * tables, std::memory_order_relaxed);
				shownHeard[i].store(grainEngine.getHeard(i), std::memory_order_relaxed);
				shownGrainRead[i].store(grainEngine.getReadPosition(i), std::memory_order_relaxed);
			}
			shownPosition.store(grainEngine.getPosition(), std::memory_order_relaxed);
			shownPreview.store(0.f, std::memory_order_relaxed);
		}
		else {
			for (int i = 0; i < NUM_MARKERS; i++) {
				shownIndex[i].store(engine.getTableIndex(i), std::memory_order_relaxed);
				shownHeard[i].store(engine.getHeard(i), std::memory_order_relaxed);
			}
			shownPosition.store(engine.getPosition(), std::memory_order_relaxed);
			shownPreview.store(engine.getPreviewLevel(), std::memory_order_relaxed);
		}
		for (int i = 0; i < NUM_MARKERS; i++)
			shownCv[i].store(mod->modulated[i], std::memory_order_relaxed);
	}
	if (engineApplied == 1) {
		// Latest grain starts of each cloud (a ring of 32 in the engine), for the display's flashes.
		uint32_t count = grainEngine.eventCount();
		uint32_t from = (count - grainEventsSeen > (uint32_t) wa::GrainEngine::NUM_EVENTS) ? count - wa::GrainEngine::NUM_EVENTS : grainEventsSeen;
		for (uint32_t e = from; e != count; e++) {
			const wa::GrainEvent& ev = grainEngine.events()[e % wa::GrainEngine::NUM_EVENTS];
			shownGrainPos[ev.marker].store(ev.pos, std::memory_order_relaxed);
			shownGrainSeconds[ev.marker].store(ev.seconds, std::memory_order_relaxed);
			shownGrainCount[ev.marker].fetch_add(1, std::memory_order_relaxed);
		}
		grainEventsSeen = count;
	}

	if (swapWanted && activeSilent) {
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
	grainEngine.setSampleRate(e.sampleRate);
}

/** WAV files directly in `dir`, sorted by name ignoring case. */
static std::vector<std::string> listWavs(const std::string& dir) {
	std::vector<std::string> wavs;
	if (dir.empty() || !system::isDirectory(dir))
		return wavs;
	for (const std::string& p : system::getEntries(dir)) {
		if (string::lowercase(system::getExtension(p)) == ".wav" && system::isFile(p))
			wavs.push_back(p);
	}
	std::sort(wavs.begin(), wavs.end(), [](const std::string& a, const std::string& b) {
		std::string la = string::lowercase(system::getFilename(a));
		std::string lb = string::lowercase(system::getFilename(b));
		return la != lb ? la < lb : a < b;
	});
	return wavs;
}

bool WaveArtifact::hasSiblings() {
	return folderWavs.size() > 1;
}

void WaveArtifact::loadSibling(int dir) {
	if (filePath.empty())
		return;
	if (analyzing) {
		setStatus("Busy, still analyzing");
		return;
	}
	// Re-scan: files may have been added or removed since the last load.
	std::vector<std::string> wavs = listWavs(system::getDirectory(filePath));
	if (wavs.empty())
		return;
	std::string current = system::getFilename(filePath);
	int n = (int) wavs.size();
	int at = -1;
	for (int i = 0; i < n; i++) {
		if (system::getFilename(wavs[i]) == current) {
			at = i;
			break;
		}
	}
	int next;
	if (at >= 0) {
		next = ((at + dir) % n + n) % n;
	}
	else {
		// The current file is gone: take the first name after it (or before it, going back).
		std::string lc = string::lowercase(current);
		next = (dir > 0) ? 0 : n - 1;
		for (int i = 0; i < n; i++) {
			int k = (dir > 0) ? i : n - 1 - i;
			std::string lk = string::lowercase(system::getFilename(wavs[k]));
			if ((dir > 0) ? lk > lc : lk < lc) {
				next = k;
				break;
			}
		}
	}
	// Only this file left: nothing to step to (compare indices, not paths: separators may differ).
	if (n == 1 && at >= 0)
		return;
	loadFile(wavs[next]);
}

void WaveArtifact::loadFile(const std::string& path) {
	if (analyzing) {
		setStatus("Busy, still analyzing");
		return;
	}
	// Remember the path even if this file fails, so the arrows can step past it.
	filePath = path;
	folderWavs = listWavs(system::getDirectory(path));
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
			WaveArtifact::MARKER_PITCH_PARAM, WaveArtifact::MARKER_PITCH_FREE_PARAM, WaveArtifact::MARKER_FINE_PARAM, WaveArtifact::MARKER_PAN_PARAM,
			WaveArtifact::MARKER_ATTACK_PARAM, WaveArtifact::MARKER_DECAY_PARAM, WaveArtifact::MARKER_SUSTAIN_PARAM, WaveArtifact::MARKER_HOLD_PARAM, WaveArtifact::MARKER_RELEASE_PARAM})
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


const int WaveArtifact::MARKER_SETTING_PARAMS[WaveArtifact::NUM_MARKER_SETTINGS] = {
	MARKER_WAVE_PARAM, MARKER_SMOOTH_PARAM,
	MARKER_LFO_RATE_PARAM, MARKER_LFO_AMT_PARAM, MARKER_LFO_SYNC_PARAM, MARKER_LFO_TRIPLET_PARAM, MARKER_LFO_DOTTED_PARAM, MARKER_LFO_SHAPE_PARAM,
	MARKER_GAIN_PARAM, MARKER_CUTOFF_PARAM, MARKER_RES_PARAM, MARKER_FILTER_MODE_PARAM, MARKER_DRIVE_PARAM,
	MARKER_PITCH_PARAM, MARKER_PITCH_FREE_PARAM, MARKER_FINE_PARAM, MARKER_PAN_PARAM,
	MARKER_GRAIN_DENSITY_PARAM, MARKER_GRAIN_SIZE_PARAM, MARKER_GRAIN_SPRAY_PARAM, MARKER_GRAIN_WINDOW_PARAM,
	MARKER_GRAIN_SYNC_PARAM, MARKER_GRAIN_TRIPLET_PARAM, MARKER_GRAIN_DOTTED_PARAM,
	MARKER_GRAIN_SPREAD_PARAM, MARKER_GRAIN_PSCATTER_PARAM, MARKER_GRAIN_SCAN_PARAM, MARKER_GRAIN_DIR_PARAM,
	MARKER_ATTACK_PARAM, MARKER_DECAY_PARAM, MARKER_SUSTAIN_PARAM, MARKER_HOLD_PARAM, MARKER_RELEASE_PARAM,
};

const int WaveArtifact::MARKER_ENVELOPE_PARAMS[WaveArtifact::NUM_ENVELOPE_PARAMS] = {
	MARKER_ATTACK_PARAM, MARKER_DECAY_PARAM, MARKER_SUSTAIN_PARAM, MARKER_HOLD_PARAM, MARKER_RELEASE_PARAM,
};

/** Settings that are a choice, not an amount: moving "by the same amount" means nothing for these. */
static bool isChoiceSetting(int id) {
	for (int choice : {WaveArtifact::MARKER_FILTER_MODE_PARAM, WaveArtifact::MARKER_LFO_SHAPE_PARAM, WaveArtifact::MARKER_LFO_SYNC_PARAM,
			WaveArtifact::MARKER_LFO_TRIPLET_PARAM, WaveArtifact::MARKER_LFO_DOTTED_PARAM, WaveArtifact::MARKER_PITCH_FREE_PARAM,
			WaveArtifact::MARKER_GRAIN_WINDOW_PARAM, WaveArtifact::MARKER_GRAIN_SYNC_PARAM, WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM,
			WaveArtifact::MARKER_GRAIN_DOTTED_PARAM, WaveArtifact::MARKER_GRAIN_DIR_PARAM}) {
		if (id == choice)
			return true;
	}
	return false;
}

bool WaveArtifact::linkStep(int paramId, bool dragging) {
	int sel = selectedMarker;
	bool sameMarker = sel == linkSelected;
	int k = -1;
	if (sameMarker && paramId >= 0) {
		for (int s = 0; s < NUM_MARKER_SETTINGS; s++) {
			if (MARKER_SETTING_PARAMS[s] + sel == paramId)
				k = s;
		}
	}
	bool ended = false;
	// A move ends when the mouse lets go or leaves that control, or Shift is released. Finished before a new
	// one can begin on the same frame.
	if (linkSetting >= 0 && (k != linkSetting || !dragging))
		ended = true;
	if (k >= 0 && !ended) {
		int base = MARKER_SETTING_PARAMS[k];
		float own = params[base + sel].getValue();
		float value = own + linkBeyond;
		if (value != linkLast[k]) {
			if (linkSetting != k) {
				linkSetting = k;
				linkMarker = sel;
				linkBaseValue = linkLast[k];
				for (int m = 0; m < NUM_MARKERS; m++)
					linkBase[m] = params[base + m].getValue();
				linkBase[sel] = linkLast[k];
			}
			// Past the knob's own limit there is only something to do until every other marker is at theirs.
			float most = 0.f;
			for (int m = 0; m < NUM_MARKERS; m++) {
				if (m == sel || !isMarkerActive(m))
					continue;
				ParamQuantity* q = getParamQuantity(base + m);
				float limit = (linkBeyond > 0.f) ? q->getMaxValue() : q->getMinValue();
				float need = limit - linkBase[m] - own + linkBaseValue;
				most = (linkBeyond > 0.f) ? std::max(most, need) : std::min(most, need);
			}
			linkBeyond = (linkBeyond > 0.f) ? std::min(linkBeyond, most) : std::max(linkBeyond, most);
			value = own + linkBeyond;
			for (int m = 0; m < NUM_MARKERS; m++) {
				if (m == sel || !isMarkerActive(m))
					continue;
				ParamQuantity* q = getParamQuantity(base + m);
				float v = isChoiceSetting(base) ? value : clamp(linkBase[m] + value - linkBaseValue, q->getMinValue(), q->getMaxValue());
				params[base + m].setValue(v);
			}
			// A single click or scroll step: done on this frame.
			ended = !dragging;
		}
	}
	for (int s = 0; s < NUM_MARKER_SETTINGS; s++)
		linkLast[s] = params[MARKER_SETTING_PARAMS[s] + sel].getValue();
	if (linkSetting >= 0 && !ended)
		linkLast[linkSetting] += linkBeyond;
	if (linkSetting < 0 && k < 0)
		linkBeyond = 0.f;
	linkSelected = sel;
	return ended;
}

void WaveArtifact::linkFinish() {
	linkSetting = -1;
	linkBeyond = 0.f;
}

void WaveArtifact::copyMarker(int i) {
	for (int k = 0; k < NUM_MARKER_SETTINGS; k++)
		markerClipboard[k] = params[MARKER_SETTING_PARAMS[k] + i].getValue();
	markerClipboardFrom = i;
}

void WaveArtifact::pasteMarker(int i) {
	if (markerClipboardFrom < 0)
		return;
	for (int k = 0; k < NUM_MARKER_SETTINGS; k++)
		params[MARKER_SETTING_PARAMS[k] + i].setValue(markerClipboard[k]);
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
	// Open in the folder of the last WAV, so loading the next one is quick.
	std::string dir = module->filePath.empty() ? "" : system::getDirectory(module->filePath);
	char* path = osdialog_file(OSDIALOG_OPEN, dir.empty() ? NULL : dir.c_str(), NULL, filters);
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

/** STOP as a round warning bullet: a red dot with an exclamation mark, no text. Dim when playing,
bright and glowing when stopped. Same click behaviour as a lit toggle. */
struct StopBullet : LitToggle {
	/** Round glow around the bullet (a radial gradient), drawn on the light layer while stopped. */
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1 && isOn()) {
			Vec c = box.size.div(2.f);
			float r = std::min(box.size.x, box.size.y) / 2.f;
			NVGpaint glow = nvgRadialGradient(args.vg, c.x, c.y, r * 0.95f, r * 1.7f, nvgTransRGBAf(color, 0.22f), nvgTransRGBAf(color, 0.f));
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, c.x, c.y, r * 1.7f);
			nvgFillPaint(args.vg, glow);
			nvgFill(args.vg);
		}
		ParamWidget::drawLayer(args, layer);
	}

	void draw(const DrawArgs& args) override {
		bool on = isOn();
		Vec c = box.size.div(2.f);
		float r = std::min(box.size.x, box.size.y) / 2.f;
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, c.x, c.y, r - 0.4f);
		nvgFillColor(args.vg, on ? nvgTransRGBAf(color, 0.95f) : nvgRGB(0x14, 0x16, 0x1d));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgTransRGBAf(color, on ? 1.f : 0.5f));
		nvgStrokeWidth(args.vg, 0.7f);
		nvgStroke(args.vg);
		// Exclamation mark: a bar and a dot
		NVGcolor mark = on ? nvgRGB(0x10, 0x12, 0x1a) : nvgTransRGBAf(color, 0.85f);
		nvgBeginPath(args.vg);
		nvgRect(args.vg, c.x - r * 0.13f, c.y - r * 0.55f, r * 0.26f, r * 0.75f);
		nvgFillColor(args.vg, mark);
		nvgFill(args.vg);
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, c.x, c.y + r * 0.62f, r * 0.15f);
		nvgFillColor(args.vg, mark);
		nvgFill(args.vg);
	}
};

struct WaveArtifactWidget : ModuleWidget {
	/** Per-marker controls at the same panel spots: shared ones, the wavetable engine's and the granular engine's */
	static const int EDIT_KNOBS = 32;
	ParamWidget* editKnobs[WaveArtifact::NUM_MARKERS][EDIT_KNOBS] = {};
	/** 0 = both engines, 1 = wavetable only, 2 = granular only */
	int editKnobEngine[EDIT_KNOBS] = {};
	/** Shown only in POLY: the envelope, with its labels and screen */
	bool editKnobPoly[EDIT_KNOBS] = {};
	PanelLabels* envelopeLabels = nullptr;
	widget::Widget* envelopeScreen = nullptr;

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
		labels->module = module;
		labels->modeParam = WaveArtifact::ENGINE_MODE_PARAM;
		labels->add(Vec(PANEL_W / 2.f, 6.f), "Wave Artifacts", 13.f);
		// Marker sections, row 1. addModal: the granular engine's title for the same knob.
		labels->addModal(Vec(WAVE_KNOB.x, ROW1_LABEL_Y), "WAVE", "START");
		labels->add(Vec(GAIN_KNOB.x, ROW1_LABEL_Y), "GAIN");
		labels->add(Vec(CUTOFF_KNOB.x, ROW1_LABEL_Y), "CUTOFF");
		labels->addModal(Vec(DRIVE_KNOB.x, ROW1_LABEL_Y), "DRIVE", "P.SPRD");
		labels->add(Vec(PITCH_KNOB.x, ROW1_LABEL_Y), "PITCH");
		labels->add(Vec(PAN_KNOB.x, ROW2_LABEL_Y), "PAN", 6.f);
		labels->addModal(Vec(SPREAD_LABEL_X, ADSR_Y), "", "SPREAD", 6.f);
		// Row 2
		labels->addModal(Vec(LFO_SHAPE_SCREEN.x, ROW1_LABEL_Y), "LFO SHAPE", "WINDOW", 6.f);
		labels->addModal(Vec(LFO_RATE_KNOB.x, ROW1_LABEL_Y), "RATE", "SIZE", 6.f);
		labels->addModal(Vec(LFO_AMT_KNOB.x, ROW2_LABEL_Y), "AMT", "SPRAY", 6.f);
		labels->addModal(Vec(SMOOTH_KNOB.x, ROW2_LABEL_Y), "SMOOTH", "DENSITY", 6.f, 5.f);
		labels->add(Vec(RES_KNOB.x, ROW2_LABEL_Y), "RES", 6.f);
		labels->add(Vec(FILTER_MODE_BUTTON.x, ROW2_LABEL_Y), "TYPE", 6.f);
		labels->addModal(Vec(FINE_KNOB.x, ROW2_LABEL_Y), "FINE", "SCAN", 6.f);
		labels->add(Vec(CLOCK_INPUT.x, ROW2_LABEL_Y + 0.5f), "CLOCK", 6.f);
		labels->add(Vec(BPM_KNOB.x, ROW1_LABEL_Y), "BPM", 6.f);
		labels->add(Vec(CLOCK_OUTPUT.x, ROW1_LABEL_Y - 0.4f), "CLK OUT", 5.f, true);
		// Controls band
		labels->add(Vec(POSITION_KNOB.x, CONTROL_LABEL_Y), "POSITION");
		labels->add(Vec(POS_ATTEN.x, POS_CV_LABEL_Y), "CV AMT", 6.f);
		labels->add(Vec(POS_LFO_SHAPE_SCREEN.x, POS_LFO_LABEL_Y), "POS LFO", 6.f);
		labels->add(Vec(POS_LFO_RATE_KNOB.x, POS_LFO_KNOB_LABEL_Y), "RATE", 6.f);
		labels->add(Vec(POS_LFO_SMOOTH_KNOB.x, POS_LFO_KNOB_LABEL_Y), "SMOOTH", 5.f);
		labels->add(Vec(POS_LFO_AMT_KNOB.x, POS_LFO_KNOB_LABEL_Y), "AMT", 6.f);
		labels->add(Vec(POS_INPUT.x, CONTROL_LABEL_Y), "POS CV", 6.f);
		labels->add(Vec(FREQ_KNOB.x, CONTROL_LABEL_Y), "FREQ");
		labels->add(Vec(VOCT_INPUT.x, CONTROL_LABEL_Y), "V/OCT");
		labels->add(Vec(MASTER_MODE_BUTTON.x, MASTER_LABEL_Y), "MASTER", 6.f);
		const char* masterNames[5] = {"CUTOFF", "RES", "DRIVE", "SPREAD", "FDBK"};
		for (int k = 0; k < 5; k++)
			labels->add(Vec(MASTER_COL_X[k], MASTER_LABEL_Y), masterNames[k], 6.f);
		labels->add(Vec(MASTER_GAIN_KNOB.x, MASTER_GAIN_LABEL_Y), "GAIN", 6.f, true);
		labels->add(Vec(MASTER_GAIN_AMT.x, MASTER_GAIN_LABEL_Y), "AMT", 5.f, true);
		labels->add(Vec(MASTER_GAIN_CV.x, MASTER_GAIN_LABEL_Y), "CV", 5.f, true);
		labels->add(Vec(LEFT_OUTPUT.x, OUTPUT_LABEL_Y), "L", 6.f, true);
		labels->add(Vec(RIGHT_OUTPUT.x, OUTPUT_LABEL_Y), "R", 6.f, true);
		// Artifact outputs column
		labels->add(Vec(ARTOUT_BOX_POS.x + ARTOUT_BOX_SIZE.x / 2.f, ARTOUT_HEADER_Y), "ARTIFACT OUT", 5.f, true);
		labels->add(Vec(ARTOUT_L_X, ARTOUT_LR_LABEL_Y), "L", 6.f, true);
		labels->add(Vec(ARTOUT_R_X, ARTOUT_LR_LABEL_Y), "R", 6.f, true);
		// ADSR strip
		labels->add(Vec(PLAY_MODE_LABEL_X, ADSR_Y), "PLAY", 6.f);
		addChild(labels);
		envelopeLabels = createWidget<PanelLabels>(Vec(0, 0));
		envelopeLabels->box.size = box.size;
		const char* adsrNames[5] = {"A", "D", "S", "H", "R"};
		for (int k = 0; k < 5; k++)
			envelopeLabels->add(Vec(ADSR_KNOB_X[k] - ADSR_LABEL_DX, ADSR_Y), adsrNames[k], 6.f);
		envelopeLabels->setVisible(false);
		addChild(envelopeLabels);

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
			// engine: 0 = both, 1 = wavetable only, 2 = granular only. Hidden widgets take no clicks.
			auto add = [&](ParamWidget* w, int engine = 0) {
				editKnobEngine[k] = engine;
				editKnobs[i][k++] = w;
				addParam(w);
				w->setVisible(i == 0 && engine != 2 && !editKnobPoly[k - 1]);
			};
			const int W = 1, G = 2;
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(WAVE_KNOB), module, WaveArtifact::MARKER_WAVE_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createLfoRateKnob<LinkedKnob<LfoRateKnobT<Trimpot>>>(LFO_RATE_KNOB, module, WaveArtifact::MARKER_LFO_RATE_PARAM + i,
				WaveArtifact::MARKER_LFO_SYNC_PARAM + i, WaveArtifact::MARKER_LFO_TRIPLET_PARAM + i,
				WaveArtifact::MARKER_LFO_DOTTED_PARAM + i, WaveArtifact::MARKER_LFO_AMT_PARAM + i), MARKER_COLORS[i]), W);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(LFO_AMT_KNOB), module, WaveArtifact::MARKER_LFO_AMT_PARAM + i), MARKER_COLORS[i]), W);
			add(createLfoShapeScreen(LFO_SHAPE_SCREEN, LFO_SHAPE_SIZE, module, WaveArtifact::MARKER_LFO_SHAPE_PARAM + i, MARKER_COLORS[i]), W);
			add(createLitToggle(LFO_SYNC_BUTTON, module, WaveArtifact::MARKER_LFO_SYNC_PARAM + i, "SYNC", MARKER_COLORS[i]), W);
			add(createLitToggle(LFO_TRIP_BUTTON, module, WaveArtifact::MARKER_LFO_TRIPLET_PARAM + i, "T", MARKER_COLORS[i], LFO_TOGGLE_SMALL), W);
			add(createLitToggle(LFO_DOT_BUTTON, module, WaveArtifact::MARKER_LFO_DOTTED_PARAM + i, "D", MARKER_COLORS[i], LFO_TOGGLE_SMALL), W);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(SMOOTH_KNOB), module, WaveArtifact::MARKER_SMOOTH_PARAM + i), MARKER_COLORS[i]), W);
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(GAIN_KNOB), module, WaveArtifact::MARKER_GAIN_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(CUTOFF_KNOB), module, WaveArtifact::MARKER_CUTOFF_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(DRIVE_KNOB), module, WaveArtifact::MARKER_DRIVE_PARAM + i), MARKER_COLORS[i]), W);
			add(withArc(createParamCentered<LinkedKnob<RoundSmallBlackKnob>>(mm2px(RES_KNOB), module, WaveArtifact::MARKER_RES_PARAM + i), MARKER_COLORS[i]));
			add(createModeButton(FILTER_MODE_BUTTON, module, WaveArtifact::MARKER_FILTER_MODE_PARAM + i, {"LP", "BP", "HP", "NOTCH"}, MARKER_COLORS[i]));
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(PITCH_KNOB), module, WaveArtifact::MARKER_PITCH_PARAM + i), MARKER_COLORS[i], true));
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(FINE_KNOB), module, WaveArtifact::MARKER_FINE_PARAM + i), MARKER_COLORS[i], true), W);
			add(createLitToggle(PITCH_FREE_BUTTON, module, WaveArtifact::MARKER_PITCH_FREE_PARAM + i, "FREE", MARKER_COLORS[i]));
			add(withArc(createParamCentered<LinkedKnob<RoundSmallBlackKnob>>(mm2px(PAN_KNOB), module, WaveArtifact::MARKER_PAN_PARAM + i), MARKER_COLORS[i], true));

			// Granular engine: its own controls at the spots of the wavetable-only ones
			// (SMOOTH -> DENSITY, RATE -> SIZE, AMT -> SPRAY, LFO SHAPE -> WINDOW, SYNC / T / D, DRIVE -> P.SPRD, FINE -> SCAN). SPREAD has its own spot in the bottom strip.
			add(withArc(createLfoRateKnob<LinkedKnob<LfoRateKnobT<Trimpot>>>(SMOOTH_KNOB, module, WaveArtifact::MARKER_GRAIN_DENSITY_PARAM + i,
				WaveArtifact::MARKER_GRAIN_SYNC_PARAM + i, WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM + i,
				WaveArtifact::MARKER_GRAIN_DOTTED_PARAM + i), MARKER_COLORS[i]), G);
			add(withArc(createLfoRateKnob<LinkedKnob<LfoRateKnobT<Trimpot>>>(LFO_RATE_KNOB, module, WaveArtifact::MARKER_GRAIN_SIZE_PARAM + i,
				WaveArtifact::MARKER_GRAIN_SYNC_PARAM + i, WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM + i,
				WaveArtifact::MARKER_GRAIN_DOTTED_PARAM + i), MARKER_COLORS[i]), G);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(LFO_AMT_KNOB), module, WaveArtifact::MARKER_GRAIN_SPRAY_PARAM + i), MARKER_COLORS[i]), G);
			add(createGrainWindowScreen(LFO_SHAPE_SCREEN, LFO_SHAPE_SIZE, module, WaveArtifact::MARKER_GRAIN_WINDOW_PARAM + i, MARKER_COLORS[i]), G);
			add(createLitToggle(LFO_SYNC_BUTTON, module, WaveArtifact::MARKER_GRAIN_SYNC_PARAM + i, "SYNC", MARKER_COLORS[i]), G);
			add(createLitToggle(LFO_TRIP_BUTTON, module, WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM + i, "T", MARKER_COLORS[i], LFO_TOGGLE_SMALL), G);
			add(createLitToggle(LFO_DOT_BUTTON, module, WaveArtifact::MARKER_GRAIN_DOTTED_PARAM + i, "D", MARKER_COLORS[i], LFO_TOGGLE_SMALL), G);
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(DRIVE_KNOB), module, WaveArtifact::MARKER_GRAIN_PSCATTER_PARAM + i), MARKER_COLORS[i]), G);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(FINE_KNOB), module, WaveArtifact::MARKER_GRAIN_SCAN_PARAM + i), MARKER_COLORS[i], true), G);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(SPREAD_KNOB), module, WaveArtifact::MARKER_GRAIN_SPREAD_PARAM + i), MARKER_COLORS[i]), G);

			// POLY: the marker's envelope, on both engines.
			for (int a = 0; a < WaveArtifact::NUM_ENVELOPE_PARAMS; a++) {
				editKnobPoly[k] = true;
				add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(Vec(ADSR_KNOB_X[a], ADSR_Y)), module, WaveArtifact::MARKER_ENVELOPE_PARAMS[a] + i), MARKER_COLORS[i]));
			}
		}
		// MORPH (one voice crossfading between markers) or POLY (each marker its own note).
		{
			ModeButton* b = createModeButton(PLAY_MODE_BUTTON, module, WaveArtifact::PLAY_MODE_PARAM, {"MORPH", "POLY"}, SCREEN_TEXT);
			b->box.size = mm2px(PLAY_MODE_BUTTON_SIZE);
			b->box.pos = mm2px(PLAY_MODE_BUTTON).minus(b->box.size.div(2.f));
			b->colors = {SCREEN_TEXT, nvgRGB(0xff, 0xc8, 0x6e)};
			addParam(b);
			AdsrScreen* screen = createWidget<AdsrScreen>(mm2px(ADSR_SCREEN_POS));
			screen->box.size = mm2px(ADSR_SCREEN_SIZE);
			screen->module = module;
			screen->setVisible(false);
			addChild(screen);
			envelopeScreen = screen;
		}
		// Engine switch in the header: WAVE (wavetable) or GRAIN (granular). The knobs keep their places
		// and change meaning (labels, tooltips and values follow).
		{
			ModeButton* b = createModeButton(Vec(21.f, 5.8f), module, WaveArtifact::ENGINE_MODE_PARAM, {"WAVE", "GRAIN"}, SCREEN_TEXT);
			b->box.size = mm2px(Vec(14.f, 5.4f));
			b->box.pos = mm2px(Vec(21.f, 5.8f)).minus(b->box.size.div(2.f));
			b->colors = {SCREEN_TEXT, nvgRGB(0xff, 0xc8, 0x6e)};
			addParam(b);
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
		addParam(createParamCentered<Trimpot>(mm2px(POS_LFO_SMOOTH_KNOB), module, WaveArtifact::POS_LFO_SMOOTH_PARAM));
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
		addParam(createParamCentered<Trimpot>(mm2px(MASTER_GAIN_AMT), module, WaveArtifact::MASTER_GAIN_AMT_PARAM));
		addInput(createInputCentered<ThemedPJ301MPort>(mm2px(MASTER_GAIN_CV), module, WaveArtifact::MASTER_GAIN_INPUT));
		addParam(createLitToggle<StopBullet>(STOP_BUTTON, module, WaveArtifact::STOP_PARAM, "", STOP_COLOR, STOP_BUTTON_SIZE));
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
			// Shift + a per-marker control moves it on every marker. A drag counts while the mouse is down;
			// otherwise the control under the mouse (a scroll step), but not with Ctrl: Ctrl+Shift+Z is redo.
			int mods = APP->window->getMods() & RACK_MOD_MASK;
			ParamWidget* held = nullptr;
			bool dragging = false;
			if (mods & GLFW_MOD_SHIFT) {
				widget::Widget* dragged = APP->event->getDraggedWidget();
				if (dragged) {
					held = dynamic_cast<ParamWidget*>(dragged);
					dragging = held != nullptr;
				}
				else if (!(mods & RACK_MOD_CTRL)) {
					held = dynamic_cast<ParamWidget*>(APP->event->getHoveredWidget());
				}
			}
			if (m->linkStep(held && held->module == m ? held->paramId : -1, dragging)) {
				pushLinkedMoveUndo(m);
				m->linkFinish();
			}
			// Rack makes a Shift drag 4x faster. Here Shift means "all markers", so the speed stays normal.
			for (int k = 0; k < EDIT_KNOBS; k++) {
				Knob* knob = dynamic_cast<Knob*>(editKnobs[m->selectedMarker][k]);
				if (knob)
					knob->speed = (mods == GLFW_MOD_SHIFT) ? 0.25f : 1.f;
			}
			bool grain = m->params[WaveArtifact::ENGINE_MODE_PARAM].getValue() > 0.5f;
			bool poly = m->params[WaveArtifact::PLAY_MODE_PARAM].getValue() > 0.5f;
			for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
				for (int k = 0; k < EDIT_KNOBS; k++) {
					int e = editKnobEngine[k];
					editKnobs[i][k]->setVisible(i == m->selectedMarker && (e == 0 || (e == 1) != grain) && (poly || !editKnobPoly[k]));
				}
			}
			envelopeLabels->setVisible(poly);
			envelopeScreen->setVisible(poly);
		}
		ModuleWidget::step();
	}

	void appendContextMenu(Menu* menu) override {
		WaveArtifact* m = getModule<WaveArtifact>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Load WAV...", "", [=]() {
			openLoadDialog(m);
		}));
		menu->addChild(createMenuItem("Previous WAV in folder", "", [=]() {
			m->loadSibling(-1);
		}, !m->hasSiblings()));
		menu->addChild(createMenuItem("Next WAV in folder", "", [=]() {
			m->loadSibling(1);
		}, !m->hasSiblings()));
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
		menu->addChild(createIndexSubmenuItem("Engine",
			{"Wavetable (WAVE)", "Granular (GRAIN)"},
			[=]() {
				return (size_t) (m->params[WaveArtifact::ENGINE_MODE_PARAM].getValue() > 0.5f ? 1 : 0);
			},
			[=](size_t i) {
				m->params[WaveArtifact::ENGINE_MODE_PARAM].setValue((float) i);
			}
		));
		// Granular settings that have no knob
		menu->addChild(createIndexSubmenuItem("Grain direction (selected marker)",
			grainDirectionNames(),
			[=]() {
				return (size_t) clamp((int) std::round(m->params[WaveArtifact::MARKER_GRAIN_DIR_PARAM + m->selectedMarker].getValue()), 0, wa::NUM_GRAIN_DIRECTIONS - 1);
			},
			[=](size_t i) {
				m->params[WaveArtifact::MARKER_GRAIN_DIR_PARAM + m->selectedMarker].setValue((float) i);
			}
		));
		menu->addChild(createCheckMenuItem("Grain scatter repeats every loop (seeded)", "",
			[=]() {
				return m->params[WaveArtifact::GRAIN_SEEDED_PARAM].getValue() > 0.5f;
			},
			[=]() {
				Param& p = m->params[WaveArtifact::GRAIN_SEEDED_PARAM];
				p.setValue(p.getValue() > 0.5f ? 0.f : 1.f);
			}
		));
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
		// Overwrites every marker, so it asks first (a submenu, one more click).
		menu->addChild(createSubmenuItem("⚠ Auto arrange artifacts", "", [=](Menu* sub) {
			sub->addChild(createMenuLabel("⚠ Resets your current artifacts:"));
			sub->addChild(createMenuLabel("all 5 on, evenly spaced, settings back to default"));
			sub->addChild(createMenuItem("Auto arrange now", "", [=]() {
				m->autoArrange();
			}));
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

	/** Creates an expander set to the first marker no expander in the chain uses yet, and slots it in
	so the chain stays in ascending marker order: right after the last expander with a lower marker
	(or right after the main module), pushing the rest of the row to the right. E.g. with one
	expander on marker 5, the new one (marker 1) goes first. One undo step, which also moves the
	pushed modules back. */
	void addExpander() {
		WaveArtifact* m = getModule<WaveArtifact>();
		if (!m)
			return;
		bool used[WaveArtifact::NUM_MARKERS] = {};
		std::vector<Module*> chain;
		Module* last = m;
		while (isWaveArtifactExpander(last->rightExpander.module)) {
			last = last->rightExpander.module;
			chain.push_back(last);
			used[((WaveArtifactExpander*) last)->getMarker()] = true;
			if ((int) chain.size() >= wa::MAX_EXPANDERS)
				return;
		}
		int marker = (int) chain.size() % WaveArtifact::NUM_MARKERS;
		for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
			if (!used[i]) {
				marker = i;
				break;
			}
		}
		// Insert before the first expander with a higher marker; at the end if there is none.
		Module* before = m;
		for (Module* e : chain) {
			if (((WaveArtifactExpander*) e)->getMarker() > marker)
				break;
			before = e;
		}
		ModuleWidget* beforeWidget = APP->scene->rack->getModule(before->id);
		if (!beforeWidget)
			return;

		// Positions before the insert, to record the modules it pushes aside.
		std::map<int64_t, Vec> oldPos;
		for (ModuleWidget* w : APP->scene->rack->getModules())
			oldPos[w->module ? w->module->id : -1] = w->box.pos;

		Model* model = modelWaveArtifactExpander;
		Module* em = model->createModule();
		em->params[WaveArtifactExpander::MARKER_PARAM].setValue((float) marker);
		APP->engine->addModule(em);
		ModuleWidget* emw = model->createModuleWidget(em);
		APP->scene->rack->addModule(emw);
		// Placed at the right edge of `before`: whatever sat there (the higher-marker expanders and
		// anything touching them) is shoved right to make room.
		APP->scene->rack->setModulePosForce(emw, Vec(beforeWidget->box.getRight(), beforeWidget->box.pos.y));

		// Undo removes the expander, then moves the pushed modules back; redo does the reverse.
		history::ComplexAction* complex = new history::ComplexAction;
		complex->name = "add Wave Artifacts expander";
		for (ModuleWidget* w : APP->scene->rack->getModules()) {
			if (w == emw || !w->module)
				continue;
			auto it = oldPos.find(w->module->id);
			if (it == oldPos.end() || it->second.equals(w->box.pos))
				continue;
			history::ModuleMove* mv = new history::ModuleMove;
			mv->moduleId = w->module->id;
			mv->oldPos = it->second;
			mv->newPos = w->box.pos;
			complex->push(mv);
		}
		history::ModuleAdd* h = new history::ModuleAdd;
		h->setModule(emw);
		complex->push(h);
		APP->history->push(complex);
	}
};

Model* modelWaveArtifact = createModel<WaveArtifact, WaveArtifactWidget>("waveArtifact");
