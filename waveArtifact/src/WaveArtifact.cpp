#include <osdialog.h>

#include "Layout.hpp"
#include "WaveArtifact.hpp"
#include "WaveArtifactExpander.hpp"
#include "WaveArtifactSequencer.hpp"
#include "widgets/AdsrScreen.hpp"
#include "widgets/ArtifactColor.hpp"
#include "widgets/WavetableMap.hpp"
#include "widgets/MiniSlider.hpp"
#include "widgets/ScanLength.hpp"
#include "widgets/GrainControls.hpp"
#include "widgets/LfoControls.hpp"
#include "widgets/LinkedKnob.hpp"
#include "widgets/LfoWidgets.hpp"
#include "widgets/MarkerBadge.hpp"
#include "widgets/MarkerBar.hpp"
#include "widgets/ModeButton.hpp"
#include "widgets/PanelLabels.hpp"
#include "widgets/StepValueBars.hpp"

namespace {

const float DEFAULT_MARKER_POS[WaveArtifact::NUM_MARKERS] = {0.f, 1.f, 0.25f, 0.5f, 0.75f};
const size_t MAX_SHOWN_NAME = 22;
const float LN_100 = 4.605170186f;


/** SAMPLE BEAT mode of a marker (wa::SampleBeatMode); OFF unless the engine switch is on SAMPLE */
static int sampleBeatMode(Module* module, int marker) {
	if (!module || (int) std::round(module->params[WaveArtifact::ENGINE_MODE_PARAM].getValue()) != 2)
		return wa::SAMPLE_BEAT_OFF;
	return clamp((int) std::round(module->params[WaveArtifact::MARKER_SAMPLE_BEAT_PARAM + marker].getValue()), 0, wa::NUM_SAMPLE_BEAT_MODES - 1);
}

/** SAMPLE BEAT: LENGTH as a note value (index into SAMPLE_DIVISIONS), with the marker's T / D */
static int sampleLengthDivision(Module* module, int marker) {
	return wa::sampleBeatDivision(module->params[WaveArtifact::MARKER_GRAIN_SCAN_LENGTH_PARAM + marker].getValue(),
		module->params[WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM + marker].getValue() > 0.5f,
		module->params[WaveArtifact::MARKER_GRAIN_DOTTED_PARAM + marker].getValue() > 0.5f);
}

static std::string beatsText(float beats) {
	if (beats >= 1.f)
		return string::f("%g beat%s", std::round(beats * 100.f) / 100.f, beats > 1.01f ? "s" : "");
	return string::f("1/%d beat", (int) std::lround(1.f / beats));
}

/** SAMPLE BEAT button: FIT shows how many beats what repeats lasts now */
struct SampleBeatQuantity : SwitchQuantity {
	std::string getDisplayValueString() override {
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(module);
		int mode = (int) std::round(getValue());
		if (!m || mode != wa::SAMPLE_BEAT_FIT)
			return SwitchQuantity::getDisplayValueString();
		float beats = m->shownBeats[paramId - WaveArtifact::MARKER_SAMPLE_BEAT_PARAM].load();
		if (beats <= 0.f)
			return SwitchQuantity::getDisplayValueString();
		return "FIT: it lasts " + beatsText(beats);
	}
};

/** SAMPLE LOOP point: free, or on a grid of the slice in BEAT / FIT (wa::sampleLoopSteps) */
struct SampleLoopPointQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(module);
		int marker = paramId - WaveArtifact::MARKER_SAMPLE_LOOP_POINT_PARAM;
		int mode = sampleBeatMode(module, marker);
		float beats = 0.f;
		if (mode == wa::SAMPLE_BEAT_CUT)
			beats = wa::SAMPLE_DIVISIONS[sampleLengthDivision(module, marker)].beats;
		else if (mode == wa::SAMPLE_BEAT_FIT && m)
			beats = m->shownBeats[marker].load();
		if (beats <= 0.f)
			return ParamQuantity::getDisplayValueString();
		int steps = wa::sampleLoopSteps(beats);
		int at = std::min((int) std::lround(getValue() * steps), steps - 1);
		return at == 0 ? "the whole slice" : string::f("from step %d of %d (%.0f%% into the slice)", at + 1, steps, 100.f * at / steps);
	}
	std::string getUnit() override {
		return sampleBeatMode(module, paramId - WaveArtifact::MARKER_SAMPLE_LOOP_POINT_PARAM) == wa::SAMPLE_BEAT_OFF ? ParamQuantity::getUnit() : "";
	}
};

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

static bool isChoiceSetting(int id);

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
			configSwitch(MARKER_GRAIN_KEEP_SPEED_PARAM + i, 0.f, 1.f, 0.f, g + "keep speed",
				{"Off (pitch changes the speed too, like tape)", "On (pitch changes only the pitch: the sample keeps its timing)"});
			configSwitch(MARKER_GRAIN_SCAN_MODE_PARAM + i, 0.f, 1.f, 0.f, g + "scan loop",
				{"Loop (back to START at the end of the slice)", "Ping-pong (forwards, then backwards)"});
			configParam(MARKER_GRAIN_SCAN_FADE_PARAM + i, 0.f, 0.5f, 0.05f, g + "scan fade at the slice ends", "%", 0.f, 100.f);
			configParam<GrainScanLengthQuantity>(MARKER_GRAIN_SCAN_LENGTH_PARAM + i, 0.f, 1.f, 1.f, g + "scan loop length");
			for (int id : {MARKER_GRAIN_WINDOW_PARAM, MARKER_GRAIN_SYNC_PARAM, MARKER_GRAIN_TRIPLET_PARAM, MARKER_GRAIN_DOTTED_PARAM, MARKER_GRAIN_DIR_PARAM, MARKER_GRAIN_KEEP_SPEED_PARAM, MARKER_GRAIN_SCAN_MODE_PARAM})
				getParamQuantity(id + i)->randomizeEnabled = false;
			// SAMPLE engine: the slice end. It shares START, LENGTH, FADE and the direction with GRAIN.
			configSwitch(MARKER_SAMPLE_LOOP_PARAM + i, 0.f, wa::NUM_PLAYER_LOOPS - 1, 1.f, name + " sample: end of the slice",
				{"One shot (each note plays the slice once; loops in MORPH)", "Loop (back to START, crossfaded by FADE)", "Ping-pong (forwards, then backwards)"});
			getParamQuantity(MARKER_SAMPLE_LOOP_PARAM + i)->randomizeEnabled = false;
			configParam<SampleLoopPointQuantity>(MARKER_SAMPLE_LOOP_POINT_PARAM + i, 0.f, 1.f, 0.f, name + " sample loop point (into the slice)", "%", 0.f, 100.f);
			configSwitch<SampleBeatQuantity>(MARKER_SAMPLE_BEAT_PARAM + i, 0.f, (float) (wa::NUM_SAMPLE_BEAT_MODES - 1), 0.f, name + " sample tempo",
				{"FREE (LENGTH and LOOP in time, its own speed)", "BEAT (LENGTH a note value 1/128 .. 4 bars, LOOP on a grid, its own speed)", "FIT (the speed changes so it lasts whole beats; LOOP on a grid)"});
			getParamQuantity(MARKER_SAMPLE_BEAT_PARAM + i)->randomizeEnabled = false;
			configSwitch(MARKER_SAMPLE_HIT_SNAP_PARAM + i, 0.f, 1.f, 0.f, name + " HIT (START snaps to the hits)", {"Off (START plays where the knob is)", "On (START plays from the nearest hit)"});
			getParamQuantity(MARKER_SAMPLE_HIT_SNAP_PARAM + i)->randomizeEnabled = false;
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
		configSwitch(MARKER_MUTE_PARAM + i, 0.f, 1.f, 0.f, name + " mute", {"Playing", "Muted"});
		getParamQuantity(MARKER_MUTE_PARAM + i)->randomizeEnabled = false;

		// Its modulation, set on its expander (MarkerMod.hpp). LFO 1 Hz synced by default, AD instant; DEPTH 0, so
		// nothing moves until it is turned (or locked on a step). Not randomized: a random depth on a random
		// destination is noise, not a variation.
		configLfoRate(this, MARKER_MOD_LFO_RATE_PARAM + i, MARKER_MOD_LFO_SYNC_PARAM + i, MARKER_MOD_LFO_TRIPLET_PARAM + i,
			MARKER_MOD_LFO_DOTTED_PARAM + i, name + " expander LFO rate", 0.606f);
		configSwitch(MARKER_MOD_LFO_SHAPE_PARAM + i, 0.f, wa::NUM_LFO_SHAPES - 1, 0.f, name + " expander LFO shape", lfoShapeNames());
		configSwitch(MARKER_MOD_LFO_SYNC_PARAM + i, 0.f, 1.f, 1.f, name + " expander LFO sync", {"Free (Hz)", "Tempo sync (BPM / CLOCK)"});
		configSwitch(MARKER_MOD_LFO_TRIPLET_PARAM + i, 0.f, 1.f, 0.f, name + " expander LFO triplets", {"Off", "On"});
		configSwitch(MARKER_MOD_LFO_DOTTED_PARAM + i, 0.f, 1.f, 0.f, name + " expander LFO dotted", {"Off", "On"});
		std::vector<std::string> dests(wa::MOD_DEST_NAMES, wa::MOD_DEST_NAMES + wa::NUM_MOD_DESTS);
		configSwitch(MARKER_MOD_LFO_DEST_PARAM + i, 0.f, wa::NUM_MOD_DESTS - 1, wa::MOD_DEST_CUTOFF, name + " expander LFO destination", dests);
		configParam(MARKER_MOD_LFO_DEPTH_PARAM + i, -1.f, 1.f, 0.f, name + " expander LFO depth", "%", 0.f, 100.f);
		configParam(MARKER_MOD_AD_ATTACK_PARAM + i, 0.f, 1.f, 0.f, name + " expander AD attack", " s", wa::MOD_AD_ATTACK_MAX / wa::MOD_AD_MIN_TIME, wa::MOD_AD_MIN_TIME);
		configParam(MARKER_MOD_AD_DECAY_PARAM + i, 0.f, 1.f, 0.5f, name + " expander AD decay", " s", wa::MOD_AD_DECAY_MAX / wa::MOD_AD_MIN_TIME, wa::MOD_AD_MIN_TIME);
		configSwitch(MARKER_MOD_AD_PERC_PARAM + i, 0.f, 1.f, 0.f, name + " expander AD mode",
			{"Gate (GATE holds it; notes and trigs run attack then decay)", "Percussive AD (decays even while GATE is held)"});
		configSwitch(MARKER_MOD_AD_DEST_PARAM + i, 0.f, wa::NUM_MOD_DESTS - 1, wa::MOD_DEST_CUTOFF, name + " expander AD destination", dests);
		configParam(MARKER_MOD_AD_DEPTH_PARAM + i, -1.f, 1.f, 0.f, name + " expander AD depth", "%", 0.f, 100.f);
		for (int id : {MARKER_MOD_LFO_RATE_PARAM, MARKER_MOD_LFO_SHAPE_PARAM, MARKER_MOD_LFO_SYNC_PARAM, MARKER_MOD_LFO_TRIPLET_PARAM,
				MARKER_MOD_LFO_DOTTED_PARAM, MARKER_MOD_LFO_DEST_PARAM, MARKER_MOD_LFO_DEPTH_PARAM, MARKER_MOD_AD_ATTACK_PARAM,
				MARKER_MOD_AD_DECAY_PARAM, MARKER_MOD_AD_PERC_PARAM, MARKER_MOD_AD_DEST_PARAM, MARKER_MOD_AD_DEPTH_PARAM})
			getParamQuantity(id + i)->randomizeEnabled = false;
		modLfos[i].seed(random::u32());
	}

	configSwitch(PLAY_MODE_PARAM, 0.f, 4.f, 0.f, "Play mode", {"Morph (the playhead crossfades neighboring markers)", "Poly (the playhead starts each marker's own note)",
		"Step (each POS LFO cycle plays the next marker, left to right)", "All (each POS LFO cycle plays every marker together)",
		"Seq (the step sequencer on the left plays the markers: a grid with velocity per step)"});
	getParamQuantity(PLAY_MODE_PARAM)->randomizeEnabled = false;
	configSwitch(ENGINE_MODE_PARAM, 0.f, 2.f, 0.f, "Engine", {"Wavetable (WAVE)", "Granular (GRAIN)", "Sample player (SAMPLE)"});
	getParamQuantity(ENGINE_MODE_PARAM)->randomizeEnabled = false;
	configSwitch(GRAIN_SEEDED_PARAM, 0.f, 1.f, 1.f, "Grain scatter repeats every loop cycle while synced", {"Off (continuous random)", "On (seeded)"});
	getParamQuantity(GRAIN_SEEDED_PARAM)->randomizeEnabled = false;
	configSwitch(GRAIN_NOTE_RESTART_PARAM, 0.f, 1.f, 1.f, "Grain notes restart from the same point", {"Off (grains follow the beat)", "On (every note the same)"});
	getParamQuantity(GRAIN_NOTE_RESTART_PARAM)->randomizeEnabled = false;
	configSwitch(SAMPLE_ZERO_SNAP_PARAM, 0.f, 1.f, 1.f, "Sample: snap START and loops to zero crossings", {"Off (exactly where set)", "On (the nearest zero crossing, up to 5 ms away)"});
	getParamQuantity(SAMPLE_ZERO_SNAP_PARAM)->randomizeEnabled = false;
	configParam(SAMPLE_HIT_SENSITIVITY_PARAM, 0.f, 1.f, wa::TRANSIENT_DEFAULT_SENSITIVITY, "Hits found (fewer .. more)", "%", 0.f, 100.f);
	getParamQuantity(SAMPLE_HIT_SENSITIVITY_PARAM)->randomizeEnabled = false;

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
		configOutput(SEND_OUTPUT + i, name + " send (what its filter gets: 2 channels L, R; 1 channel L + R for a mono module on its return)");
		configInput(RETURN_INPUT + i, name + " return (replaces its filter: 2 channels L, R, or 1 for both sides)");
	}

	for (int i = 0; i < NUM_MARKERS; i++) {
		shownIndex[i].store(0.f);
		shownCv[i].store(false);
		shownReturn[i].store(false);
		shownHeard[i].store(0.f);
		shownGrainRead[i].store(0.f);
		shownScanStart[i].store(0.f);
		shownScanEnd[i].store(-1.f);
		shownBeats[i].store(0.f);
		shownGrainPos[i].store(0.f);
		shownGrainSeconds[i].store(0.f);
		shownGrainCount[i].store(0);
	}
	// Modulation arrives from expanders on the right.
	rightExpander.producerMessage = new wa::ToMainMessage;
	rightExpander.consumerMessage = new wa::ToMainMessage;
	// The step sequencer sits on the left.
	leftExpander.producerMessage = new wa::FromSequencerMessage;
	leftExpander.consumerMessage = new wa::FromSequencerMessage;
	// Parameter locks: which params are lockable settings, and which of those jump instead of gliding.
	static_assert(wa::NUM_LOCKABLE == NUM_MARKER_SETTINGS, "StepLocks.hpp LOCK_NAMES must follow MARKER_SETTING_PARAMS");
	for (int p = 0; p < PARAMS_LEN; p++)
		lockIndex[p] = -1;
	for (int k = 0; k < NUM_MARKER_SETTINGS; k++) {
		// Not lockable: always the knob, and no step value (see wa::NOT_LOCKABLE)
		lockIndex[MARKER_SETTING_PARAMS[k]] = wa::isLockable(k) ? (int8_t) k : (int8_t) -1;
		if (isChoiceSetting(MARKER_SETTING_PARAMS[k]))
			lockJumpMask |= (uint64_t) 1u << k;
	}
	setStatus("Load a WAV (max 10 MB)");
}

void WaveArtifact::onRandomize(const RandomizeEvent& e) {
	Module::onRandomize(e);
	// The knobs themselves change, also while a sequencer step is selected (the sequencer's RND randomizes locks).
	lockResync = true;
	// Rack calls this with the engine locked, so the expanders' params can be set directly. They
	// are walked in chain order, like the messages; each applies its own randomizeEnabled flags.
	Module* last = this;
	for (int n = 0; n < wa::MAX_EXPANDERS && isWaveArtifactExpander(last->rightExpander.module); n++) {
		last = last->rightExpander.module;
		last->onRandomize(e);
	}
}

static std::vector<std::string> listWavs(const std::string& dir);

/** The loaded WAV's copy in the patch's storage folder */
static const char* const PATCH_SAMPLE_NAME = "sample.wav";

/** FNV-1a of a file's bytes: tells the WAV a patch saved from another one (a preset loaded into a module whose
storage folder still holds its own earlier sample) */
static uint32_t wavHashOf(const std::vector<uint8_t>& bytes) {
	uint32_t h = 2166136261u;
	for (uint8_t b : bytes) {
		h ^= b;
		h *= 16777619u;
	}
	return h;
}

json_t* WaveArtifact::dataToJson() {
	json_t* root = json_object();
	if (!wavBytes.empty()) {
		json_object_set_new(root, "filePath", json_string(filePath.c_str()));
		json_object_set_new(root, "fileName", json_string(fileName.c_str()));
		json_object_set_new(root, "sampleHash", json_integer(wavHash));
	}
	json_object_set_new(root, "tableCountMode", json_integer((int) tableCountMode));
	json_object_set_new(root, "tableTone", json_integer((int) tableTone));
	json_object_set_new(root, "monoOutput", json_boolean(monoOutput.load()));
	return root;
}

void WaveArtifact::dataFromJson(json_t* root) {
	if (json_t* j = json_object_get(root, "tableCountMode"))
		tableCountMode = (wa::TableCountMode) clamp((int) json_integer_value(j), 0, (int) wa::TABLE_MODES_LEN - 1);
	if (json_t* j = json_object_get(root, "monoOutput"))
		monoOutput = json_is_true(j);
	if (json_t* j = json_object_get(root, "tableTone"))
		tableTone = (wa::TableTone) clamp((int) json_integer_value(j), 0, (int) wa::TABLE_TONES_LEN - 1);
	json_t* f = json_object_get(root, "filePath");
	json_t* n = json_object_get(root, "fileName");
	json_t* h = json_object_get(root, "sampleHash");
	if (f || n) {
		savedFilePath = (f && json_is_string(f)) ? json_string_value(f) : "";
		savedFileName = (n && json_is_string(n)) ? json_string_value(n) : "";
		savedHash = h ? (uint32_t) json_integer_value(h) : 0u;
		savedSamplePending = true;
	}
}

void WaveArtifact::onSave(const SaveEvent& e) {
	Module::onSave(e);
	// Keep a copy of the loaded WAV in the patch, so the project opens without the original file. Written once per
	// sample (Rack saves every few seconds into its autosave folder, and archives that folder with the patch).
	if (wavBytes.empty())
		return;
	std::string dir = createPatchStorageDirectory();
	if (dir.empty())
		return;
	std::string path = system::join(dir, PATCH_SAMPLE_NAME);
	if (path == storedPath && wavHash == storedHash && system::exists(path))
		return;
	FILE* fp = std::fopen(path.c_str(), "wb");
	if (!fp)
		return;
	size_t written = std::fwrite(wavBytes.data(), 1, wavBytes.size(), fp);
	std::fclose(fp);
	if (written == wavBytes.size()) {
		storedPath = path;
		storedHash = wavHash;
	}
}

void WaveArtifact::loadSavedSample() {
	savedSamplePending = false;
	if (analyzing)
		return;
	// The patch's own copy when it is the WAV the patch saved, else the original file where it was
	std::vector<uint8_t> bytes;
	std::string stored = system::join(getPatchStorageDirectory(), PATCH_SAMPLE_NAME);
	if (system::exists(stored)) {
		bytes = system::readFile(stored);
		if (savedHash != 0u && wavHashOf(bytes) != savedHash)
			bytes.clear();
		else if (!bytes.empty()) {
			storedPath = stored;
			storedHash = wavHashOf(bytes);
		}
	}
	if (bytes.empty() && !savedFilePath.empty() && system::exists(savedFilePath) && system::getFileSize(savedFilePath) <= wa::MAX_FILE_BYTES)
		bytes = system::readFile(savedFilePath);
	filePath = savedFilePath;
	folderWavs = filePath.empty() ? std::vector<std::string>() : listWavs(system::getDirectory(filePath));
	std::string name = savedFileName.empty() ? system::getFilename(savedFilePath) : savedFileName;
	if (bytes.empty()) {
		setStatus("Sample not found: " + name);
		return;
	}
	wavBytes = std::move(bytes);
	wavHash = wavHashOf(wavBytes);
	fileName = name;
	startAnalysis();
}

void WaveArtifact::onReset(const ResetEvent& e) {
	Module::onReset(e);
	// Every knob goes back to its default, also while a sequencer step is selected (not into that step's locks).
	lockResync = true;
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
	wavHash = 0;
	monoOutput = false;
	savedSamplePending = false;
	fileName.clear();
	filePath.clear();
	folderWavs.clear();
	setStatus("Load a WAV (max 10 MB)");
}

WaveArtifact::~WaveArtifact() {
	if (worker.joinable())
		worker.join();
	delete pendingBank.exchange(nullptr);
	delete hitsPending.exchange(nullptr);
	delete hitsRetired.exchange(nullptr);
	delete hitsActive;
	delete activeBank.exchange(nullptr);
	delete retiredBank.exchange(nullptr);
	delete (wa::ToMainMessage*) rightExpander.producerMessage;
	delete (wa::ToMainMessage*) rightExpander.consumerMessage;
	delete (wa::FromSequencerMessage*) leftExpander.producerMessage;
	delete (wa::FromSequencerMessage*) leftExpander.consumerMessage;
}

void WaveArtifact::refreshSettings(int i) {
	MarkerSettings& s = settings[i];
	s.active = params[MARKER_ACTIVE_PARAM + i].getValue() > 0.5f;
	s.muted = isMarkerMuted(i);
	s.pos = params[MARKER_POS_PARAM + i].getValue();
	s.wave = mv(MARKER_WAVE_PARAM, i);
	s.lfoSync = mv(MARKER_LFO_SYNC_PARAM, i) > 0.5f;
	s.lfoTriplets = mv(MARKER_LFO_TRIPLET_PARAM, i) > 0.5f;
	s.lfoDotted = mv(MARKER_LFO_DOTTED_PARAM, i) > 0.5f;
	s.lfoShape = (int) std::round(mv(MARKER_LFO_SHAPE_PARAM, i));
	s.lfoRate = mv(MARKER_LFO_RATE_PARAM, i);
	s.lfoAmount = mv(MARKER_LFO_AMT_PARAM, i);
	s.waveSmooth = wa::MIN_WAVE_SMOOTH * std::exp(mv(MARKER_SMOOTH_PARAM, i) * LN_100);
	s.gain = mv(MARKER_GAIN_PARAM, i);
	s.filterMode = (int) std::round(mv(MARKER_FILTER_MODE_PARAM, i));
	s.res = mv(MARKER_RES_PARAM, i);
	// Semitone mode plays whole semitones even if the knob holds a fraction (it is re-snapped in uiStep).
	float pitch = mv(MARKER_PITCH_PARAM, i);
	s.pitch = (mv(MARKER_PITCH_FREE_PARAM, i) > 0.5f) ? pitch : std::round(pitch);
	s.cutoff = mv(MARKER_CUTOFF_PARAM, i);
	s.drive = mv(MARKER_DRIVE_PARAM, i);
	s.fine = mv(MARKER_FINE_PARAM, i);
	s.pan = mv(MARKER_PAN_PARAM, i);
	// Its expander LFO and AD
	s.modLfoSync = mv(MARKER_MOD_LFO_SYNC_PARAM, i) > 0.5f;
	s.modLfoTriplets = mv(MARKER_MOD_LFO_TRIPLET_PARAM, i) > 0.5f;
	s.modLfoDotted = mv(MARKER_MOD_LFO_DOTTED_PARAM, i) > 0.5f;
	s.modLfoShape = (int) std::round(mv(MARKER_MOD_LFO_SHAPE_PARAM, i));
	s.modLfoRate = mv(MARKER_MOD_LFO_RATE_PARAM, i);
	s.modLfoDest = (int) std::round(mv(MARKER_MOD_LFO_DEST_PARAM, i));
	s.modLfoDepth = mv(MARKER_MOD_LFO_DEPTH_PARAM, i);
	s.adPerc = mv(MARKER_MOD_AD_PERC_PARAM, i) > 0.5f;
	s.adAttack = mv(MARKER_MOD_AD_ATTACK_PARAM, i);
	s.adDecay = mv(MARKER_MOD_AD_DECAY_PARAM, i);
	s.adDest = (int) std::round(mv(MARKER_MOD_AD_DEST_PARAM, i));
	s.adDepth = mv(MARKER_MOD_AD_DEPTH_PARAM, i);
	// POLY: its ADSR
	s.adsr.attack = wa::adsrAttackSeconds(mv(MARKER_ATTACK_PARAM, i));
	s.adsr.decay = wa::adsrFallSeconds(mv(MARKER_DECAY_PARAM, i));
	s.adsr.sustain = mv(MARKER_SUSTAIN_PARAM, i);
	s.adsr.release = wa::adsrFallSeconds(mv(MARKER_RELEASE_PARAM, i));
	s.adsr.hold = wa::adsrHoldSeconds(mv(MARKER_HOLD_PARAM, i));
	// GRAIN / SAMPLE
	s.density = mv(MARKER_GRAIN_DENSITY_PARAM, i);
	s.size = mv(MARKER_GRAIN_SIZE_PARAM, i);
	s.spray = mv(MARKER_GRAIN_SPRAY_PARAM, i);
	s.window = clamp((int) std::round(mv(MARKER_GRAIN_WINDOW_PARAM, i)), 0, wa::NUM_GRAIN_WINDOWS - 1);
	s.loopMode = clamp((int) std::round(mv(MARKER_SAMPLE_LOOP_PARAM, i)), 0, wa::NUM_PLAYER_LOOPS - 1);
	s.loopPoint = mv(MARKER_SAMPLE_LOOP_POINT_PARAM, i);
	s.beatMode = clamp((int) std::round(mv(MARKER_SAMPLE_BEAT_PARAM, i)), 0, wa::NUM_SAMPLE_BEAT_MODES - 1);
	s.grainSync = mv(MARKER_GRAIN_SYNC_PARAM, i) > 0.5f;
	s.grainTriplets = mv(MARKER_GRAIN_TRIPLET_PARAM, i) > 0.5f;
	s.grainDotted = mv(MARKER_GRAIN_DOTTED_PARAM, i) > 0.5f;
	// BEAT: LENGTH as a note value, with the marker's T / D (right-click menu)
	float lengthKnob = mv(MARKER_GRAIN_SCAN_LENGTH_PARAM, i);
	s.beatLength = wa::SAMPLE_DIVISIONS[wa::sampleBeatDivision(lengthKnob, s.grainTriplets, s.grainDotted)].beats;
	s.hitSnap = mv(MARKER_SAMPLE_HIT_SNAP_PARAM, i) > 0.5f;
	s.pScatter = mv(MARKER_GRAIN_PSCATTER_PARAM, i);
	s.scan = mv(MARKER_GRAIN_SCAN_PARAM, i);
	s.spread = mv(MARKER_GRAIN_SPREAD_PARAM, i);
	s.direction = clamp((int) std::round(mv(MARKER_GRAIN_DIR_PARAM, i)), 0, wa::NUM_GRAIN_DIRECTIONS - 1);
	s.keepSpeed = mv(MARKER_GRAIN_KEEP_SPEED_PARAM, i) > 0.5f;
	s.scanPing = mv(MARKER_GRAIN_SCAN_MODE_PARAM, i) > 0.5f;
	s.scanFade = mv(MARKER_GRAIN_SCAN_FADE_PARAM, i);
	const wa::WavetableBank* bank = activeBank.load(std::memory_order_relaxed);
	s.scanLength = (bank && bank->sample.valid()) ? wa::grainScanLengthFraction(lengthKnob, (double) bank->sample.frames / bank->sample.sampleRate) : -1.f;
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
	int wantEngine = clamp((int) std::round(params[ENGINE_MODE_PARAM].getValue()), 0, 2);
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
		for (int i = 0; i < NUM_MARKERS; i++) {
			lfos[i].reset();
			modLfos[i].reset();
		}
		posLfo.reset();
		stepper.reset();
		seqResetCount++;
		seqClock.reset();
	}
	int ppqn = wa::PPQN_VALUES[clamp((int) std::round(params[CLOCK_PPQN_PARAM].getValue()), 0, wa::NUM_PPQN - 1)];
	// CLOCK patched: follow it. Unpatched: the internal tempo makes the same beats.
	if (inputs[CLOCK_INPUT].isConnected())
		clock.process(inputs[CLOCK_INPUT].getVoltage(), true, args.sampleRate, ppqn);
	else
		clock.processInternal(params[INTERNAL_BPM_PARAM].getValue(), args.sampleRate);
	seqClock.process(clock, args.sampleRate);
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
		out->granular = engineApplied == 1; // SAMPLE reads the CVs like WAVE: WAVE is START, FINE and DRIVE are themselves
		out->poly = params[PLAY_MODE_PARAM].getValue() > 0.5f;
		for (int i = 0; i < NUM_MARKERS; i++)
			out->modLfo[i] = modLfoOut[i];
		right->leftExpander.requestMessageFlip();
	}
	// The step sequencer on the left: which markers it struck this sample, and how hard.
	bool seqFired[NUM_MARKERS] = {};
	// Tied steps (long gates): a fire that goes on with the note, and notes kept on until their next tied step.
	bool seqLegato[NUM_MARKERS] = {}, seqHoldOpen[NUM_MARKERS] = {};
	bool seqMode = params[PLAY_MODE_PARAM].getValue() > 3.5f;
	bool seqLocksOn = false;
	{
		const wa::FromSequencerMessage* sq = (const wa::FromSequencerMessage*) leftExpander.consumerMessage;
		Module* leftModule = leftExpander.module;
		if (isWaveArtifactSequencer(leftModule)) {
			seqLocksOn = seqMode && sq->linked;
			wa::ToSequencerMessage* out = (wa::ToSequencerMessage*) leftModule->rightExpander.producerMessage;
			out->beatPos = seqClock.beatPos;
			out->resetCount = seqResetCount;
			for (int i = 0; i < NUM_MARKERS; i++)
				out->markerOn[i] = params[MARKER_ACTIVE_PARAM + i].getValue() > 0.5f;
			leftModule->rightExpander.requestMessageFlip();
			bool now = sq->linked;
			for (int i = 0; i < NUM_MARKERS; i++) {
				seqHoldOpen[i] = now && sq->holdOpen[i];
				if (now && !seqLinkedBefore) {
					seqSeen[i] = sq->fireCount[i]; // just snapped on: do not play what it played before
				}
				else if (sq->fireCount[i] != seqSeen[i]) {
					seqSeen[i] = sq->fireCount[i];
					seqFired[i] = true;
					// A tied step whose note is still on: same note, same level (the group's first trig's velocity).
					seqLegato[i] = sq->legato[i] && voices.gate[i];
					if (!seqLegato[i])
						seqVelocity[i] = sq->velocity[i];
					// From silence the new velocity is in place at once, so a soft step after a hard one does not
					// start with the hard one's level.
					if (voices.env[i] < 1e-3f)
						seqVelocityUsed[i] = seqVelocity[i];
					// The step's parameter locks, from now until the row plays another step.
					if (seqLocksOn) {
						float knobs[NUM_MARKER_SETTINGS];
						for (int k = 0; k < NUM_MARKER_SETTINGS; k++)
							knobs[k] = params[MARKER_SETTING_PARAMS[k] + i].getValue();
						seqLocks[i].apply(sq->lockValue[i], sq->lockMask[i], sq->slide[i], sq->stepBeats[i], knobs, lockJumpMask);
						settingsDirty[i] = true;
					}
				}
			}
			seqLinkedBefore = now;
		}
		else {
			seqLinkedBefore = false;
		}
	}
	// Locks only play in SEQ with the sequencer on: otherwise every marker plays its knobs.
	for (int i = 0; i < NUM_MARKERS; i++) {
		if (seqLocksOn)
			seqLocks[i].advance(seqClock.advance);
		else
			seqLocks[i].clear();
	}
	auto modOf = [&](int marker, int target) {
		return mod->mod[marker][target] / 10.f * wa::MOD_SCALE_PER_10V[target];
	};

	// The markers' settings (MarkerSettings): every SETTINGS_INTERVAL samples, and at once for a marker whose locks
	// just changed or glide, or when the bank changes.
	{
		const wa::WavetableBank* bankNow = activeBank.load(std::memory_order_relaxed);
		bool all = --settingsCountdown <= 0 || bankNow != settingsBank;
		if (all) {
			settingsCountdown = SETTINGS_INTERVAL;
			settingsBank = bankNow;
			posSmoothTime = wa::MIN_WAVE_SMOOTH * std::exp(params[POS_LFO_SMOOTH_PARAM].getValue() * LN_100);
		}
		for (int i = 0; i < NUM_MARKERS; i++) {
			bool gliding = seqLocks[i].gliding != 0;
			if (all || settingsDirty[i] || gliding || settingsGliding[i] || seqLocks[i].mask != settingsLockMask[i]) {
				refreshSettings(i);
				settingsDirty[i] = false;
				settingsGliding[i] = gliding;
				settingsLockMask[i] = seqLocks[i].mask;
			}
		}
	}

	wa::EngineInput in;
	// The expander modulation's DRIVE amount per marker, for GRAIN's P.SPRD below
	float modDrive[NUM_MARKERS] = {};
	bool polyMode = params[PLAY_MODE_PARAM].getValue() > 0.5f;
	const int lfoGrace = (int) args.sampleRate; // 1 s: far longer than any amount's fade in the engines
	for (int i = 0; i < NUM_MARKERS; i++) {
		const MarkerSettings& s = settings[i];
		wa::MarkerInput& m = in.markers[i];
		// The marker's expander LFO and AD (MarkerMod.hpp), each on the setting it is sent to. The AD starts on each
		// of the marker's notes (a POLY note, a sequencer trig: modAdNote, from the last sample) and on its expander's
		// GATE. In MORPH a patched GATE also plays the artifact through the AD (its level follows it).
		// The LFO only runs while it is heard: routed (DEPTH not 0), on a jack, or with the sequencer on in SEQ (a
		// trig may lock a DEPTH: the LFO is then where it would have been).
		wa::ModSum ms;
		float gateLevel = 1.f;
		{
			if (s.active && (s.modLfoDepth != 0.f || mod->lfoJack[i] || seqLocksOn))
				modLfoLive[i] = lfoGrace;
			if (modLfoLive[i] > 0) {
				modLfoLive[i]--;
				modLfoOut[i] = modLfos[i].process(s.modLfoShape, s.modLfoSync, s.modLfoRate, s.modLfoTriplets, s.modLfoDotted, clock, args.sampleTime);
			}
			else {
				modLfoOut[i] = 0.f;
			}
			bool gatePatched = mod->gatePatched[i];
			bool gateHigh = gatePatched && mod->gateHigh[i];
			bool gateEdge = gateHigh && !modGateWas[i];
			modGateWas[i] = gateHigh;
			float ad = modAds[i].process(modAdNote[i] || gateEdge, gateEdge, gateHigh, s.adPerc, s.adAttack, s.adDecay, args.sampleTime);
			modAdNote[i] = false;
			ms.put(s.modLfoDest, s.modLfoDepth * modLfoOut[i]);
			ms.put(s.adDest, s.adDepth * ad);
			if (gatePatched && !polyMode)
				gateLevel = ad;
			modDrive[i] = ms.add[wa::MOD_DEST_DRIVE];
		}
		m.active = s.active;
		m.pos = s.pos;
		m.wave = s.wave;
		// The engine adds the LFO after WAVE's glide (so SMOOTH does not dull it) and folds it back at the ends of the
		// sample. Only the wavetable engine uses it, and only while its amount is up.
		if (s.active && engineApplied == 0 && s.lfoAmount > 0.f)
			waveLfoLive[i] = lfoGrace;
		if (waveLfoLive[i] > 0) {
			waveLfoLive[i]--;
			m.lfo = lfos[i].process(s.lfoShape, s.lfoSync, s.lfoRate, s.lfoTriplets, s.lfoDotted, clock, args.sampleTime);
		}
		else {
			m.lfo = 0.f;
		}
		m.lfoAmount = s.lfoAmount;
		m.waveSmooth = s.waveSmooth;
		m.gain = s.gain;
		m.filterMode = s.filterMode;
		m.res = clamp(s.res + ms.add[wa::MOD_DEST_RES], 0.f, 1.f);
		// The expander modulation glides, in semitones and between.
		m.pitch = s.pitch + ms.add[wa::MOD_DEST_PITCH];

		// Expander CV, added in each param's own units (see MOD_SCALE_PER_10V). WAVE is folded at
		// the sample's ends by the engine, like the LFO. The filter is always on; CUTOFF CV sweeps it
		// across its whole range (20 Hz .. 20 kHz).
		m.wave += modOf(i, wa::MOD_WAVE) + ms.add[wa::MOD_DEST_WAVE];
		m.gain = clamp(m.gain * mod->gainMul[i] * ms.gainMul() * gateLevel, 0.f, 4.f); // expander VCA, modulation, GATE
		// Muted: no level at all, through the engines' 5 ms gain smoothing, so it fades instead of clicking.
		if (s.muted)
			m.gain = 0.f;
		// SEQ: velocity scales the voice's level (see voiceEnv below) and, here, opens the filter.
		if (seqMode)
			seqVelocityUsed[i] += (seqVelocity[i] - seqVelocityUsed[i]) * std::min(1.f, args.sampleTime / 0.005f);
		m.cutoff = clamp(s.cutoff + modOf(i, wa::MOD_CUTOFF) + ms.add[wa::MOD_DEST_CUTOFF] - (seqMode ? (1.f - seqVelocityUsed[i]) * 0.25f : 0.f), 0.f, 1.f);
		m.drive = clamp(s.drive + modOf(i, wa::MOD_DRIVE) + ms.add[wa::MOD_DEST_DRIVE], 0.f, 1.f);
		m.fine = clamp(s.fine + modOf(i, wa::MOD_FINE), -12.f, 12.f);
		// Expander V/OCT, added to the main V/OCT for this artifact only
		m.voct = mod->voct[i];
		m.pan = clamp(s.pan + modOf(i, wa::MOD_PAN) + ms.add[wa::MOD_DEST_PAN], -1.f, 1.f);
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
		lfo = posLfoSmoother.process(lfo, posSmoothTime, args.sampleTime);
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
	// STEP: the same voices, played in turn by the POS LFO's cycles instead of the playhead.
	bool stepWanted = params[PLAY_MODE_PARAM].getValue() > 1.5f;
	if (polyWanted) {
		if (!polyRunning || stepWanted != stepRunning) {
			voices.reset();
			stepper.reset();
		}
		float markerPos[NUM_MARKERS];
		bool markerOn[NUM_MARKERS];
		wa::AdsrSettings adsr[NUM_MARKERS];
		for (int i = 0; i < NUM_MARKERS; i++) {
			markerPos[i] = in.markers[i].pos;
			markerOn[i] = in.markers[i].active;
			adsr[i] = settings[i].adsr;
		}
		// Expander GATEs and a press on a slot play markers directly.
		wa::VoiceGates direct;
		int pressed = pressedMarker.load(std::memory_order_relaxed);
		for (int i = 0; i < NUM_MARKERS; i++) {
			direct.patched[i] = mod->gatePatched[i];
			direct.high[i] = mod->gateHigh[i] || pressed == i;
		}
		if (stepWanted) {
			bool playable[NUM_MARKERS];
			for (int i = 0; i < NUM_MARKERS; i++)
				playable[i] = markerOn[i] && !direct.patched[i] && !settings[i].muted;
			// ALL: every cycle plays every marker together.
			if (seqMode) {
				// SEQ: the step grid decides (muted and off markers are skipped by the playable test above).
				bool struck[NUM_MARKERS];
				for (int i = 0; i < NUM_MARKERS; i++)
					struck[i] = seqFired[i] && playable[i];
				voices.process(markerPos, markerOn, triggerPosition, adsr, args.sampleTime, &direct, struck, seqLegato, seqHoldOpen);
			}
			else {
				stepper.process(posLfo.getPhase(), markerPos, playable, params[PLAY_MODE_PARAM].getValue() > 2.5f);
				voices.process(markerPos, markerOn, triggerPosition, adsr, args.sampleTime, &direct, stepper.fired);
				// The playhead shows the marker playing.
				if (stepper.current >= 0)
					in.position = markerPos[stepper.current];
			}
		}
		else {
			voices.process(markerPos, markerOn, triggerPosition, adsr, args.sampleTime, &direct);
		}
	}
	polyRunning = polyWanted;
	stepRunning = polyWanted && stepWanted;
	in.poly = polyWanted;
	for (int i = 0; i < NUM_MARKERS; i++) {
		in.voiceEnv[i] = voices.env[i] * (seqMode ? seqVelocityUsed[i] : 1.f);
		in.voiceStart[i] = polyWanted && voices.start[i];
		// A new note starts its expander AD on the next sample
		if (in.voiceStart[i])
			modAdNote[i] = true;
	}
	in.voct = params[FREQ_PARAM].getValue() + inputs[VOCT_INPUT].getVoltage();
	in.bank = activeBank.load(std::memory_order_relaxed);
	// STOP uses the same click-free fade as a bank swap (main, artifact outputs and preview).
	in.holdSilent = swapWanted || switching || params[STOP_PARAM].getValue() > 0.5f;
	in.previewTable = previewTable.load(std::memory_order_relaxed);
	// SEND / RETURN per artifact: a patched RETURN replaces the artifact's filter (the engine crossfades 20 ms). It
	// reads the send of one sample before (Rack's cable delay). 2 channels are L and R, 1 feeds both sides. Volts
	// are clamped to +-12 V, NaN is silence.
	in.wantSend = false;
	for (int i = 0; i < NUM_MARKERS; i++) {
		Input& ret = inputs[RETURN_INPUT + i];
		Output& send = outputs[SEND_OUTPUT + i];
		in.wantSend = in.wantSend || send.isConnected();
		int ch = ret.isConnected() ? ret.getChannels() : 0;
		in.insert[i] = ch > 0;
		auto volts = [](float v) {
			return std::isfinite(v) ? clamp(v, -12.f, 12.f) / 5.f : 0.f;
		};
		in.retL[i] = ch > 0 ? volts(ret.getVoltage(0)) : 0.f;
		in.retR[i] = ch > 1 ? volts(ret.getVoltage(1)) : in.retL[i];
		// A mono module on the loop (1 channel back while SEND sends 2) gets L + R from now on
		if (ch == 0 || !send.isConnected()) {
			monoSend[i] = false;
			monoCount[i] = 0;
		}
		else if (!monoSend[i] && send.getChannels() == 2 && ch == 1) {
			if (++monoCount[i] >= MONO_SEND_SAMPLES)
				monoSend[i] = true;
		}
		else {
			monoCount[i] = 0;
		}
	}
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
	if (engineApplied >= 1) {
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
			const MarkerSettings& s = settings[i];
			g.density = clamp(s.density + 0.5f * modOf(i, wa::MOD_FINE), 0.f, 1.f);
			g.size = s.size;
			g.spray = s.spray;
			g.window = s.window;
			g.loopMode = s.loopMode;
			g.loopPoint = s.loopPoint;
			// SAMPLE keeps the wavetable engine's DRIVE and FINE (GRAIN has P.SPRD and LENGTH in their places).
			g.drive = wm.drive;
			g.beatMode = s.beatMode;
			g.beatLength = s.beatLength;
			g.hitSnap = s.hitSnap;
			g.sync = s.grainSync;
			g.triplet = s.grainTriplets;
			g.dotted = s.grainDotted;
			// The expander's DRIVE input drives P.SPRD while granular. PAN is PAN on both engines.
			g.pitchScatter = clamp(s.pScatter + modOf(i, wa::MOD_DRIVE) + modDrive[i], 0.f, 1.f);
			g.scan = s.scan;
			g.spread = s.spread;
			g.pan = wm.pan;
			g.direction = s.direction;
			g.keepSpeed = s.keepSpeed;
			g.scanPing = s.scanPing;
			g.scanFade = s.scanFade;
			g.scanLength = s.scanLength;
			g.cutoff = wm.cutoff;
			g.filterMode = wm.filterMode;
			g.res = wm.res;
			g.gain = wm.gain;
			g.pitch = wm.pitch + (engineApplied == 2 ? wm.fine : 0.f);
			g.voct = wm.voct;
		}
		gin.position = in.position;
		gin.voct = in.voct;
		gin.bank = in.bank;
		gin.clock = &clock;
		gin.reset = reset;
		gin.seeded = params[GRAIN_SEEDED_PARAM].getValue() > 0.5f;
		gin.noteRestart = params[GRAIN_NOTE_RESTART_PARAM].getValue() > 0.5f;
		gin.player = engineApplied == 2;
		gin.zeroSnap = params[SAMPLE_ZERO_SNAP_PARAM].getValue() > 0.5f;
		// HIT: take a new hit list from the UI (the old one goes back to be freed there)
		if (hitsPending.load(std::memory_order_acquire) && !hitsRetired.load(std::memory_order_acquire)) {
			std::vector<long>* fresh = hitsPending.exchange(nullptr);
			if (fresh) {
				hitsRetired.store(hitsActive);
				hitsActive = fresh;
			}
		}
		gin.hits = hitsActive;
		gin.holdSilent = in.holdSilent;
		gin.poly = in.poly;
		for (int i = 0; i < NUM_MARKERS; i++) {
			gin.insert[i] = in.insert[i];
			gin.retL[i] = in.retL[i];
			gin.retR[i] = in.retR[i];
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
	bool activeSilent = (engineApplied >= 1) ? grainEngine.isSilent() : engine.isSilent();
	if (switching && activeSilent)
		engineApplied = wantEngine;
	// Mono output (right-click): L + R on both sides, after the master section, so every widening (grain SPREAD, the
	// Smear window's diffuser, master SPREAD) folds to the center. Switching crossfades over 10 ms.
	{
		float target = monoOutput.load(std::memory_order_relaxed) ? 1.f : 0.f;
		if (monoMix != target) {
			float step = args.sampleTime / 0.01f;
			monoMix = target > monoMix ? std::min(target, monoMix + step) : std::max(target, monoMix - step);
		}
		if (monoMix > 0.f) {
			float c = 0.5f * (l + r);
			l += (c - l) * monoMix;
			r += (c - r) * monoMix;
		}
	}
	outputs[LEFT_OUTPUT].setVoltage(5.f * l);
	outputs[RIGHT_OUTPUT].setVoltage(5.f * r);
	// SEND: 2 channels (L, R), or L + R on one for a mono module on the return
	for (int i = 0; i < NUM_MARKERS; i++) {
		float sl, sr;
		if (engineApplied >= 1)
			grainEngine.getSend(i, &sl, &sr);
		else
			engine.getSend(i, &sl, &sr);
		Output& send = outputs[SEND_OUTPUT + i];
		if (monoSend[i]) {
			send.setChannels(1);
			send.setVoltage(2.5f * (sl + sr));
		}
		else {
			send.setChannels(2);
			send.setVoltage(5.f * sl, 0);
			send.setVoltage(5.f * sr, 1);
		}
	}

	// Display values, a few hundred times a second is plenty for a 60 Hz screen.
	if (++displayCounter >= 64) {
		displayCounter = 0;
		if (engineApplied >= 1) {
			const wa::WavetableBank* bank = in.bank;
			float tables = bank ? (float) std::max(bank->numTables - 1, 0) : 0.f;
			for (int i = 0; i < NUM_MARKERS; i++) {
				// The old displays think in tables: the table under the place each cloud reads
				shownIndex[i].store(grainEngine.getReadPosition(i) * tables, std::memory_order_relaxed);
				shownHeard[i].store(grainEngine.getHeard(i), std::memory_order_relaxed);
				shownGrainRead[i].store(grainEngine.getReadPosition(i), std::memory_order_relaxed);
				float sliceStart, sliceEnd;
				grainEngine.getScanSlice(i, &sliceStart, &sliceEnd);
				shownScanStart[i].store(sliceStart, std::memory_order_relaxed);
				shownScanEnd[i].store(sliceEnd, std::memory_order_relaxed);
				shownBeats[i].store((float) grainEngine.getBeats(i), std::memory_order_relaxed);
			}
			shownPosition.store(grainEngine.getPosition(), std::memory_order_relaxed);
			shownPreview.store(0.f, std::memory_order_relaxed);
		}
		else {
			for (int i = 0; i < NUM_MARKERS; i++) {
				shownScanEnd[i].store(-1.f, std::memory_order_relaxed);
				shownBeats[i].store(0.f, std::memory_order_relaxed);
				shownIndex[i].store(engine.getTableIndex(i), std::memory_order_relaxed);
				shownHeard[i].store(engine.getHeard(i), std::memory_order_relaxed);
			}
			shownPosition.store(engine.getPosition(), std::memory_order_relaxed);
			shownPreview.store(engine.getPreviewLevel(), std::memory_order_relaxed);
		}
		for (int i = 0; i < NUM_MARKERS; i++) {
			shownCv[i].store(mod->modulated[i], std::memory_order_relaxed);
			shownReturn[i].store(in.insert[i], std::memory_order_relaxed);
		}
	}
	if (engineApplied >= 1) {
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
	wavHash = wavHashOf(wavBytes);
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
	// A patch or preset just loaded: its WAV
	if (savedSamplePending)
		loadSavedSample();
	updateTransients();
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

void WaveArtifact::updateTransients() {
	delete hitsRetired.exchange(nullptr);
	// SAMPLE and GRAIN use them (HIT); switching to either finds them.
	if ((int) std::round(params[ENGINE_MODE_PARAM].getValue()) < 1)
		return;
	float sens = clamp(params[SAMPLE_HIT_SENSITIVITY_PARAM].getValue(), 0.f, 1.f);
	int serial = bankSerial.load();
	if (serial == transientsSerial && sens == transientsSens)
		return;
	// While the slider is dragged: at most every 30 ms (60 s of sample takes about 5 ms)
	double now = system::getTime();
	if (serial == transientsSerial && now - transientsTime < 0.03)
		return;
	transientsSerial = serial;
	transientsSens = sens;
	transientsTime = now;
	transients.clear();
	transientFrames = 0;
	// The UI frees retired banks itself (uiStep), so the active one stays valid while this runs.
	const wa::WavetableBank* bank = activeBank.load();
	if (bank && bank->sample.valid()) {
		transients = wa::findTransients(bank->sample, sens);
		transientFrames = (long) bank->sample.frames;
	}
	delete hitsPending.exchange(new std::vector<long>(transients));
}

bool WaveArtifact::stepStartToTransient(int marker, int dir) {
	if (marker < 0 || marker >= NUM_MARKERS)
		return false;
	updateTransients();
	if (transients.empty() || transientFrames <= 0)
		return false;
	double at = (double) wa::fold01(params[MARKER_WAVE_PARAM + marker].getValue()) * (double) transientFrames;
	long to = -1;
	if (dir > 0) {
		// The first one clearly after START (more than a frame: START sits on one after a step)
		auto it = std::upper_bound(transients.begin(), transients.end(), (long) std::floor(at + 1.5));
		if (it != transients.end())
			to = *it;
	}
	else {
		auto it = std::lower_bound(transients.begin(), transients.end(), (long) std::ceil(at - 1.5));
		if (it != transients.begin())
			to = *(it - 1);
	}
	if (to < 0)
		return false;
	params[MARKER_WAVE_PARAM + marker].setValue((float) ((double) to / (double) transientFrames));
	return true;
}

bool WaveArtifact::spreadMarkersOnTransients() {
	updateTransients();
	int active[NUM_MARKERS], k = 0;
	for (int i = 0; i < NUM_MARKERS; i++)
		if (isMarkerActive(i))
			active[k++] = i;
	if (transients.empty() || transientFrames <= 0 || k == 0)
		return false;
	// Marker j of k takes the hit j / k of the way through the list: the first one always, then evenly.
	size_t n = transients.size();
	for (int j = 0; j < k; j++) {
		size_t t = std::min((size_t) ((double) j * (double) n / (double) k), n - 1);
		params[MARKER_WAVE_PARAM + active[j]].setValue((float) ((double) transients[t] / (double) transientFrames));
	}
	return true;
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
	MARKER_GRAIN_KEEP_SPEED_PARAM, MARKER_GRAIN_SCAN_MODE_PARAM, MARKER_GRAIN_SCAN_FADE_PARAM, MARKER_GRAIN_SCAN_LENGTH_PARAM,
	MARKER_SAMPLE_LOOP_PARAM, MARKER_SAMPLE_LOOP_POINT_PARAM, MARKER_SAMPLE_BEAT_PARAM, MARKER_SAMPLE_HIT_SNAP_PARAM,
	MARKER_MOD_LFO_RATE_PARAM, MARKER_MOD_LFO_SHAPE_PARAM, MARKER_MOD_LFO_SYNC_PARAM, MARKER_MOD_LFO_TRIPLET_PARAM,
	MARKER_MOD_LFO_DOTTED_PARAM, MARKER_MOD_LFO_DEST_PARAM, MARKER_MOD_LFO_DEPTH_PARAM,
	MARKER_MOD_AD_ATTACK_PARAM, MARKER_MOD_AD_DECAY_PARAM, MARKER_MOD_AD_PERC_PARAM, MARKER_MOD_AD_DEST_PARAM, MARKER_MOD_AD_DEPTH_PARAM,
};

const int WaveArtifact::MARKER_ENVELOPE_PARAMS[WaveArtifact::NUM_ENVELOPE_PARAMS] = {
	MARKER_ATTACK_PARAM, MARKER_DECAY_PARAM, MARKER_SUSTAIN_PARAM, MARKER_HOLD_PARAM, MARKER_RELEASE_PARAM,
};

/** Settings that are a choice, not an amount: moving "by the same amount" means nothing for these. */
static bool isChoiceSetting(int id) {
	for (int choice : {WaveArtifact::MARKER_FILTER_MODE_PARAM, WaveArtifact::MARKER_LFO_SHAPE_PARAM, WaveArtifact::MARKER_LFO_SYNC_PARAM,
			WaveArtifact::MARKER_LFO_TRIPLET_PARAM, WaveArtifact::MARKER_LFO_DOTTED_PARAM, WaveArtifact::MARKER_PITCH_FREE_PARAM,
			WaveArtifact::MARKER_GRAIN_WINDOW_PARAM, WaveArtifact::MARKER_GRAIN_SYNC_PARAM, WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM,
			WaveArtifact::MARKER_GRAIN_DOTTED_PARAM, WaveArtifact::MARKER_GRAIN_DIR_PARAM, WaveArtifact::MARKER_GRAIN_KEEP_SPEED_PARAM,
			WaveArtifact::MARKER_GRAIN_SCAN_MODE_PARAM, WaveArtifact::MARKER_SAMPLE_LOOP_PARAM, WaveArtifact::MARKER_SAMPLE_BEAT_PARAM, WaveArtifact::MARKER_SAMPLE_HIT_SNAP_PARAM,
			WaveArtifact::MARKER_MOD_LFO_SHAPE_PARAM, WaveArtifact::MARKER_MOD_LFO_SYNC_PARAM, WaveArtifact::MARKER_MOD_LFO_TRIPLET_PARAM,
			WaveArtifact::MARKER_MOD_LFO_DOTTED_PARAM, WaveArtifact::MARKER_MOD_LFO_DEST_PARAM, WaveArtifact::MARKER_MOD_AD_PERC_PARAM,
			WaveArtifact::MARKER_MOD_AD_DEST_PARAM}) {
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

bool WaveArtifact::resetSettingOnAllMarkers(int paramId) {
	int sel = selectedMarker;
	int base = -1;
	for (int s = 0; s < NUM_MARKER_SETTINGS; s++)
		if (MARKER_SETTING_PARAMS[s] + sel == paramId)
			base = MARKER_SETTING_PARAMS[s];
	if (base < 0)
		return false;
	history::ComplexAction* action = new history::ComplexAction;
	action->name = "reset on all markers";
	for (int i = 0; i < NUM_MARKERS; i++) {
		if (i != sel && !isMarkerActive(i))
			continue;
		ParamQuantity* q = getParamQuantity(base + i);
		float before = params[base + i].getValue(), def = q->getDefaultValue();
		if (before == def)
			continue;
		history::ParamChange* change = new history::ParamChange;
		change->moduleId = id;
		change->paramId = base + i;
		change->oldValue = before;
		change->newValue = def;
		action->push(change);
		params[base + i].setValue(def);
	}
	if (action->isEmpty())
		delete action;
	else if (APP && APP->history)
		APP->history->push(action);
	// Not a drag: the Shift move logic must not treat the selected marker's change as something to spread.
	for (int s = 0; s < NUM_MARKER_SETTINGS; s++)
		linkLast[s] = params[MARKER_SETTING_PARAMS[s] + sel].getValue();
	return true;
}

void WaveArtifact::linkFinish() {
	linkSetting = -1;
	linkBeyond = 0.f;
}

WaveArtifactSequencer* WaveArtifact::sequencerOnLeft() {
	Module* left = leftExpander.module;
	return isWaveArtifactSequencer(left) ? (WaveArtifactSequencer*) left : nullptr;
}

bool WaveArtifact::lockTarget(int* row, int* step) {
	WaveArtifactSequencer* seq = sequencerOnLeft();
	if (!seq || !seq->hasSelection() || seq->selRow != selectedMarker || seq->velocity[seq->selRow][seq->selStep] <= 0.f)
		return false;
	*row = seq->selRow;
	*step = seq->selStep;
	return true;
}

int WaveArtifact::settingIndexOf(int paramId, int marker) {
	int base = paramId - marker;
	if (marker < 0 || base < 0 || base >= PARAMS_LEN)
		return -1;
	return lockIndex[base];
}

bool WaveArtifact::stepValueFor(int paramId, float* value, bool* locked, int* step) {
	int r, s;
	if (!lockTarget(&r, &s))
		return false;
	int k = settingIndexOf(paramId, r);
	if (k < 0)
		return false;
	// Live record: the step being recorded into (the one playing, or the last one played while the playhead is on an
	// empty step), so the bars and the held knob follow the recording as it happens.
	int rec = recordStep();
	if (rec >= 0)
		lastRecordStep = rec;
	if (rec == -2)
		lastRecordStep = -1;
	else if (lastRecordStep >= 0 && sequencerOnLeft()->velocity[r][lastRecordStep] > 0.f)
		s = lastRecordStep;
	const wa::StepData& d = sequencerOnLeft()->stepData[r][s];
	*locked = d.locked(k);
	// Mid-capture the control already shows the edit: lockSnap is the knob's own value.
	float own = (lockSnapMarker == r) ? lockSnap[k] : params[paramId].getValue();
	*value = *locked ? d.value[k] : own;
	if (step)
		*step = s;
	return true;
}

std::string WaveArtifact::valueText(int paramId, float value) {
	ParamQuantity* q = getParamQuantity(paramId);
	if (!q)
		return "";
	ValueShown* shown = dynamic_cast<ValueShown*>(q);
	if (!shown)
		return string::f("%g", value);
	shown->showing = true;
	shown->shown = value;
	std::string text = q->getDisplayValueString() + q->getUnit();
	shown->showing = false;
	return text;
}

std::string WaveArtifact::lockTooltip(int paramId) {
	WaveArtifactSequencer* seq = sequencerOnLeft();
	if (!seq)
		return "";
	int marker = -1, k = -1;
	for (int i = 0; i < NUM_MARKER_SETTINGS; i++) {
		int m = paramId - MARKER_SETTING_PARAMS[i];
		if (m >= 0 && m < NUM_MARKERS) {
			marker = m;
			k = i;
		}
	}
	if (marker < 0 || !wa::isLockable(k))
		return "";
	std::string text;
	// The selected trig (while recording: the one being recorded into), as the bar under the control shows it
	int r, s, step;
	float value;
	bool locked;
	if (lockTarget(&r, &s) && r == marker && stepValueFor(paramId, &value, &locked, &step)) {
		// A lock with its own condition: when it applies
		char cond[16];
		wa::conditionText(seq->stepData[r][step].lockCondition(k), cond, sizeof(cond));
		std::string note = !locked ? " (not locked)" : (cond[0] ? string::f(" (if %s)", cond) : "");
		text += string::f("\nTrig %d: %s%s", step + 1, valueText(paramId, value).c_str(), note.c_str());
	}
	return text;
}

void WaveArtifact::showInLane(int paramId) {
	WaveArtifactSequencer* seq = sequencerOnLeft();
	int k = settingIndexOf(paramId, selectedMarker);
	if (!seq || k < 0)
		return;
	seq->showLock(k);
	seq->laneRow = selectedMarker;
}

int WaveArtifact::stepsLockingInRow(int paramId) {
	WaveArtifactSequencer* seq = sequencerOnLeft();
	int k = settingIndexOf(paramId, selectedMarker);
	if (!seq || k < 0 || selectedMarker < 0 || selectedMarker >= NUM_MARKERS)
		return 0;
	int n = 0;
	for (int s = 0; s < WaveArtifactSequencer::STEPS; s++)
		if (seq->velocity[selectedMarker][s] > 0.f && seq->stepData[selectedMarker][s].locked(k))
			n++;
	return n;
}

bool WaveArtifact::stepWaveShown(int marker, float* wave, bool* locked) {
	int r, s;
	if (!lockTarget(&r, &s) || r != marker)
		return false;
	float v;
	bool l;
	if (!stepValueFor(MARKER_WAVE_PARAM + marker, &v, &l))
		return false;
	*wave = clamp(v, 0.f, 1.f);
	if (locked)
		*locked = l;
	return true;
}

bool WaveArtifact::stepWaveEditing(int marker, float* wave) {
	if (!stepWaveShown(marker, wave))
		return false;
	if (waveEditMarker == marker)
		return true;
	// The WAVE / START knob held while it shows the step (LinkedKnob)
	widget::Widget* dragged = APP->event->getDraggedWidget();
	ParamWidget* pw = dynamic_cast<ParamWidget*>(dragged);
	StepShownValue* shown = dynamic_cast<StepShownValue*>(dragged);
	return pw && shown && pw->module == this && pw->paramId == MARKER_WAVE_PARAM + marker && shown->stepShowing;
}

int WaveArtifact::rowLocks(int marker, int k, bool clear) {
	WaveArtifactSequencer* seq = sequencerOnLeft();
	if (!seq || marker < 0 || marker >= NUM_MARKERS || !wa::isLockable(k))
		return 0;
	int n = 0;
	for (int s = 0; s < WaveArtifactSequencer::STEPS; s++) {
		wa::StepData& d = seq->stepData[marker][s];
		if (!d.locked(k))
			continue;
		n++;
		if (clear)
			d.clearLock(k);
	}
	return n;
}

void WaveArtifact::appendLockMenu(ui::Menu* menu, int paramId) {
	WaveArtifactSequencer* seq = sequencerOnLeft();
	if (!seq)
		return;
	// Which marker and setting this control is (each setting's params are one block of NUM_MARKERS IDs)
	int marker = -1, k = -1;
	for (int s = 0; s < NUM_MARKER_SETTINGS; s++) {
		int i = paramId - MARKER_SETTING_PARAMS[s];
		if (i >= 0 && i < NUM_MARKERS) {
			marker = i;
			k = s;
		}
	}
	if (marker < 0 || !wa::isLockable(k))
		return;
	int n = rowLocks(marker, k);
	menu->addChild(new MenuSeparator);
	// The selected trig locks this setting: its lock's own condition
	int tr, ts;
	if (lockTarget(&tr, &ts) && tr == marker && seq->stepData[tr][ts].locked(k)) {
		char text[16];
		wa::conditionText(seq->stepData[tr][ts].lockCondition(k), text, sizeof(text));
		menu->addChild(createSubmenuItem(string::f("Trig %d lock condition", ts + 1), text[0] ? text : "always", [=](ui::Menu* sub) {
			WaveArtifactSequencer* sq = sequencerOnLeft();
			if (sq)
				appendLockConditionItems(sub, sq, tr, ts, k);
		}));
	}
	menu->addChild(createMenuItem(string::f("Clear %s trig locks (row %d)", wa::LOCK_LABELS[k], marker + 1), n == 1 ? "1 step" : string::f("%d steps", n),
		[=]() {
			WaveArtifactSequencer* sq = sequencerOnLeft();
			if (!sq)
				return;
			// Undo: the sequencer's whole state before and after
			history::ModuleChange* h = new history::ModuleChange;
			h->name = "clear trig locks";
			h->moduleId = sq->id;
			h->oldModuleJ = sq->toJson();
			rowLocks(marker, k, true);
			h->newModuleJ = sq->toJson();
			APP->history->push(h);
			// The lane shows what was cleared
			sq->laneRow = marker;
			sq->showLock(k);
		},
		n == 0));
}

int WaveArtifact::recordStep() {
	int r, s;
	WaveArtifactSequencer* seq = sequencerOnLeft();
	if (!lockTarget(&r, &s) || seq->params[WaveArtifactSequencer::REC_PARAM].getValue() <= 0.5f)
		return -2;
	int p = seq->playing[r].load();
	return (p >= 0 && p < WaveArtifactSequencer::STEPS && seq->velocity[r][p] > 0.f) ? p : -1;
}

wa::StepData* WaveArtifact::lockWriteStep(int* row) {
	int r, s;
	if (!lockTarget(&r, &s))
		return nullptr;
	int rec = recordStep();
	// Recording: the step playing now, or nothing while the playhead is on an empty step.
	if (rec == -1)
		return nullptr;
	*row = r;
	return &sequencerOnLeft()->stepData[r][rec >= 0 ? rec : s];
}

bool WaveArtifact::setStepLock(int paramId, float value) {
	int r, s;
	if (!lockTarget(&r, &s))
		return false;
	int k = settingIndexOf(paramId, r);
	if (k < 0)
		return false;
	wa::StepData* d = lockWriteStep(&r);
	if (d) {
		ParamQuantity* q = getParamQuantity(paramId);
		d->setLock(k, clamp(value, q->getMinValue(), q->getMaxValue()));
	}
	// The lane shows the setting being locked (unipolar or bipolar as that setting), and while recording the steps filling in.
	sequencerOnLeft()->showLock(k);
	return true;
}

bool WaveArtifact::clearStepLock(int paramId) {
	int r, s;
	if (!lockTarget(&r, &s))
		return false;
	int k = settingIndexOf(paramId, r);
	if (k < 0)
		return false;
	sequencerOnLeft()->stepData[r][s].clearLock(k);
	sequencerOnLeft()->showLock(k);
	return true;
}

void WaveArtifact::lockCaptureStep() {
	int r, s;
	bool target = lockTarget(&r, &s);
	bool resync = lockResync.exchange(false);
	if (!target || r != lockSnapMarker || resync) {
		// Not capturing (or a new target): remember the controls as they are, to see changes from the next frame.
		int m = target ? r : selectedMarker;
		for (int k = 0; k < NUM_MARKER_SETTINGS; k++)
			lockSnap[k] = params[MARKER_SETTING_PARAMS[k] + m].getValue();
		lockSnapMarker = target ? r : -1;
		return;
	}
	// Recording: into the step playing now (nothing while the playhead is on an empty step); else the selected one.
	int writeRow = r;
	wa::StepData* dp = lockWriteStep(&writeRow);
	wa::StepData none;
	wa::StepData& d = dp ? *dp : none;
	for (int k = 0; k < NUM_MARKER_SETTINGS; k++) {
		int id = MARKER_SETTING_PARAMS[k] + r;
		float v = params[id].getValue();
		if (v == lockSnap[k])
			continue;
		// Not lockable: the control changes the marker itself, as without a selected step.
		if (!wa::isLockable(k)) {
			lockSnap[k] = v;
			continue;
		}
		// A knob moves the lock by as much as the control moved. A switch or list stepped by one (a toggle, a mode
		// button's next / previous) steps the lock's choice the same way, so clicks keep cycling from what the step
		// plays; a choice picked further away (a menu) is taken as it is.
		ParamQuantity* q = getParamQuantity(id);
		float from = d.locked(k) ? d.value[k] : lockSnap[k];
		float lock;
		if (isChoiceSetting(MARKER_SETTING_PARAMS[k])) {
			int lo = (int) std::round(q->getMinValue()), n = (int) std::round(q->getMaxValue()) - lo + 1;
			int delta = (int) std::round(v - lockSnap[k]);
			if (n > 1 && (std::abs(delta) == 1 || std::abs(delta) == n - 1)) {
				int step = (std::abs(delta) == 1) ? delta : (delta > 0 ? -1 : 1);
				lock = (float) (lo + ((((int) std::round(from) - lo + step) % n) + n) % n);
			}
			else {
				lock = v;
			}
		}
		else {
			lock = clamp(from + v - lockSnap[k], q->getMinValue(), q->getMaxValue());
		}
		d.setLock(k, lock);
		sequencerOnLeft()->showLock(k); // the lane shows what is being locked
		params[id].setValue(lockSnap[k]);
	}
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

template <class T = ModeButton>
static T* createModeButton(Vec mmCenter, Module* module, int paramId, std::vector<std::string> labels, NVGcolor color) {
	T* b = createParam<T>(Vec(0, 0), module, paramId);
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

/** SAMPLE: steps the selected marker's START to the previous / next transient of the sample (the first press finds
them, Normal, when none were). One pair for the selected marker; undoable like a knob move. */
struct TransientArrow : widget::OpaqueWidget {
	WaveArtifact* module = nullptr;
	int dir = 1;
	ui::Tooltip* tooltip = nullptr;

	void draw(const DrawArgs& args) override {
		int sel = module ? module->selectedMarker : 0;
		NVGcolor c = layout::MARKER_COLORS[clamp(sel, 0, WaveArtifact::NUM_MARKERS - 1)];
		float w = box.size.x, h = box.size.y;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.5f, 0.5f, w - 1.f, h - 1.f, 1.2f);
		nvgStrokeColor(args.vg, nvgTransRGBAf(c, 0.8f));
		nvgStrokeWidth(args.vg, 0.8f);
		nvgStroke(args.vg);
		float cx = w * 0.5f, cy = h * 0.5f, s = std::min(w, h) * 0.28f;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, cx + dir * s, cy);
		nvgLineTo(args.vg, cx - dir * s, cy - s);
		nvgLineTo(args.vg, cx - dir * s, cy + s);
		nvgClosePath(args.vg);
		nvgFillColor(args.vg, c);
		nvgFill(args.vg);
	}

	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && module) {
			int m = module->selectedMarker;
			float before = module->params[WaveArtifact::MARKER_WAVE_PARAM + m].getValue();
			if (module->stepStartToTransient(m, dir)) {
				history::ParamChange* h = new history::ParamChange;
				h->name = dir > 0 ? "START to next transient" : "START to previous transient";
				h->moduleId = module->id;
				h->paramId = WaveArtifact::MARKER_WAVE_PARAM + m;
				h->oldValue = before;
				h->newValue = module->params[WaveArtifact::MARKER_WAVE_PARAM + m].getValue();
				APP->history->push(h);
			}
			e.consume(this);
			return;
		}
		OpaqueWidget::onButton(e);
	}

	void onEnter(const EnterEvent& e) override {
		if (!tooltip && settings::tooltips) {
			tooltip = new ui::Tooltip;
			tooltip->text = std::string(dir > 0 ? "START to the next transient" : "START to the previous transient")
				+ "\n(the HITS slider below: fewer or more of them; HIT: START snaps to them)";
			APP->scene->addChild(tooltip);
		}
	}
	void onLeave(const LeaveEvent& e) override {
		hideTooltip();
	}
	void onHide(const HideEvent& e) override {
		hideTooltip();
		OpaqueWidget::onHide(e);
	}
	void hideTooltip() {
		if (!tooltip)
			return;
		APP->scene->removeChild(tooltip);
		delete tooltip;
		tooltip = nullptr;
	}
	~TransientArrow() {
		hideTooltip();
	}
};

struct WaveArtifactWidget : ModuleWidget {
	/** Per-marker controls at the same panel spots: shared ones, the wavetable engine's and the granular engine's */
	static const int EDIT_KNOBS = 43;
	ParamWidget* editKnobs[WaveArtifact::NUM_MARKERS][EDIT_KNOBS] = {};
	/** The engines a control belongs to: bit 0 wavetable, bit 1 granular, bit 2 sample (0 = every engine) */
	int editKnobEngine[EDIT_KNOBS] = {};
	static const int W = 1, G = 2, S = 4;
	/** SAMPLE: the transient arrows under START (one pair, for the selected marker) */
	TransientArrow* transientArrows[2] = {};
	/** SAMPLE: the HITS slider (how many hits are found) */
	ParamWidget* hitsSlider = nullptr;
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
		// Marker sections, row 1. addModal: the granular engine's title for the same knob (SAMPLE too); addModal3: its own on SAMPLE.
		labels->addModal(Vec(WAVE_KNOB.x, ROW1_LABEL_Y), "WAVE", "START");
		labels->add(Vec(GAIN_KNOB.x, ROW1_LABEL_Y), "GAIN");
		labels->add(Vec(CUTOFF_KNOB.x, ROW1_LABEL_Y), "CUTOFF");
		labels->addModal3(Vec(DRIVE_KNOB.x, ROW1_LABEL_Y), "DRIVE", " ", "DRIVE");
		labels->add(Vec(PITCH_KNOB.x, ROW1_LABEL_Y), "PITCH");
		labels->add(Vec(PAN_KNOB.x, ROW2_LABEL_Y), "PAN", 6.f);
		labels->addModal3(Vec(SPREAD_KNOB.x, ROW2_LABEL_Y), "", "SPREAD", " ", 6.f);
		// GRAIN's extra columns: LENGTH and P.SPRD on top (the WINDOW screen under P.SPRD has no title)
		labels->addModal3(Vec(GRAIN_LENGTH_KNOB.x, ROW1_LABEL_Y), "", "LENGTH", " ", 5.f);
		labels->addModal3(Vec(GRAIN_PSCATTER_KNOB.x, ROW1_LABEL_Y), "", "P.SPRD", " ", 5.f);
		// Row 2
		labels->addModal(Vec(LFO_SHAPE_SCREEN.x, ROW1_LABEL_Y), "LFO SHAPE", " ", 6.f);
		labels->addModal(Vec(LFO_RATE_KNOB.x, ROW1_LABEL_Y), "RATE", " ", 6.f);
		labels->addModal(Vec(LFO_AMT_KNOB.x, ROW2_LABEL_Y), "AMT", " ", 6.f);
		labels->addModal(Vec(SMOOTH_KNOB.x, ROW2_LABEL_Y), "SMOOTH", " ", 6.f);
		// The grain group (GRAIN only): row 1 knobs, row 2 the window, SYNC / T / D under DENSITY, SPRAY, the loop
		labels->addModal3(Vec(GRAIN_DENSITY_KNOB.x, ROW1_LABEL_Y), "", "DENSITY", " ", 5.f);
		labels->addModal3(Vec(GRAIN_SIZE_KNOB.x, ROW1_LABEL_Y), "", "SIZE", " ", 6.f);
		labels->addModal3(Vec(GRAIN_SCAN_KNOB.x, ROW1_LABEL_Y), "", "SCAN", " ", 6.f);
		labels->addModal3(Vec(GRAIN_FADE_KNOB.x, ROW1_LABEL_Y), "", "FADE", " ", 6.f);
		// The SAMPLE group (SAMPLE only): LENGTH, LOOP, FADE after START
		labels->addModal3(Vec(SAMPLE_LENGTH_KNOB.x, ROW1_LABEL_Y), "", " ", "LENGTH", 5.f);
		labels->addModal3(Vec(SAMPLE_LOOP_KNOB.x, ROW1_LABEL_Y), "", " ", "LOOP", 6.f);
		labels->addModal3(Vec(SAMPLE_FADE_KNOB.x, ROW1_LABEL_Y), "", " ", "FADE", 6.f);
		labels->addModal3(Vec(GRAIN_SPRAY_KNOB.x, ROW2_LABEL_Y), "", "SPRAY", " ", 6.f);
		labels->add(Vec(RES_KNOB.x, ROW2_LABEL_Y), "RES", 6.f);
		labels->add(Vec(FILTER_MODE_BUTTON.x, ROW2_LABEL_Y), "TYPE", 6.f);
		labels->addModal3(Vec(FINE_KNOB.x, ROW2_LABEL_Y), "FINE", " ", "FINE", 6.f);
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
		labels->add(Vec(ARTOUT_BOX_POS.x + ARTOUT_BOX_SIZE.x / 2.f, ARTOUT_HEADER_Y), "SEND / RETURN", 5.f, true);
		labels->add(Vec(ARTOUT_L_X, ARTOUT_LR_LABEL_Y), "SEND", 5.f, true);
		labels->add(Vec(ARTOUT_R_X, ARTOUT_LR_LABEL_Y), "RET", 5.f, true);
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
			// engine: the engines it shows on (W, G, S bits; 0 = all). Hidden widgets take no clicks.
			auto add = [&](ParamWidget* w, int engine = 0) {
				editKnobEngine[k] = engine;
				editKnobs[i][k++] = w;
				addParam(w);
				w->setVisible(i == 0 && (engine == 0 || (engine & W)) && !editKnobPoly[k - 1]);
			};
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(WAVE_KNOB), module, WaveArtifact::MARKER_WAVE_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createLfoRateKnob<LinkedKnob<LfoRateKnobT<Trimpot>>>(LFO_RATE_KNOB, module, WaveArtifact::MARKER_LFO_RATE_PARAM + i,
				WaveArtifact::MARKER_LFO_SYNC_PARAM + i, WaveArtifact::MARKER_LFO_TRIPLET_PARAM + i,
				WaveArtifact::MARKER_LFO_DOTTED_PARAM + i, WaveArtifact::MARKER_LFO_AMT_PARAM + i), MARKER_COLORS[i]), W);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(LFO_AMT_KNOB), module, WaveArtifact::MARKER_LFO_AMT_PARAM + i), MARKER_COLORS[i]), W);
			add(createLfoShapeScreen<AboveTooltip<LfoShapeScreen>>(LFO_SHAPE_SCREEN, LFO_SHAPE_SIZE, module, WaveArtifact::MARKER_LFO_SHAPE_PARAM + i, MARKER_COLORS[i]), W);
			add(createLitToggle<AboveTooltip<LitToggle>>(LFO_SYNC_BUTTON, module, WaveArtifact::MARKER_LFO_SYNC_PARAM + i, "SYNC", MARKER_COLORS[i]), W);
			add(createLitToggle<AboveTooltip<LitToggle>>(LFO_TRIP_BUTTON, module, WaveArtifact::MARKER_LFO_TRIPLET_PARAM + i, "T", MARKER_COLORS[i], LFO_TOGGLE_SMALL), W);
			add(createLitToggle<AboveTooltip<LitToggle>>(LFO_DOT_BUTTON, module, WaveArtifact::MARKER_LFO_DOTTED_PARAM + i, "D", MARKER_COLORS[i], LFO_TOGGLE_SMALL), W);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(SMOOTH_KNOB), module, WaveArtifact::MARKER_SMOOTH_PARAM + i), MARKER_COLORS[i]), W);
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(GAIN_KNOB), module, WaveArtifact::MARKER_GAIN_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(CUTOFF_KNOB), module, WaveArtifact::MARKER_CUTOFF_PARAM + i), MARKER_COLORS[i]));
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(DRIVE_KNOB), module, WaveArtifact::MARKER_DRIVE_PARAM + i), MARKER_COLORS[i]), W | S);
			add(withArc(createParamCentered<LinkedKnob<RoundSmallBlackKnob>>(mm2px(RES_KNOB), module, WaveArtifact::MARKER_RES_PARAM + i), MARKER_COLORS[i]));
			add(createModeButton<AboveTooltip<ModeButton>>(FILTER_MODE_BUTTON, module, WaveArtifact::MARKER_FILTER_MODE_PARAM + i, {"LP", "BP", "HP", "NOTCH"}, MARKER_COLORS[i]));
			add(withArc(createParamCentered<LinkedKnob<RoundBlackKnob>>(mm2px(PITCH_KNOB), module, WaveArtifact::MARKER_PITCH_PARAM + i), MARKER_COLORS[i], true));
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(FINE_KNOB), module, WaveArtifact::MARKER_FINE_PARAM + i), MARKER_COLORS[i], true), W | S);
			add(createLitToggle<AboveTooltip<LitToggle>>(PITCH_FREE_BUTTON, module, WaveArtifact::MARKER_PITCH_FREE_PARAM + i, "FREE", MARKER_COLORS[i]));
			add(withArc(createParamCentered<LinkedKnob<RoundSmallBlackKnob>>(mm2px(PAN_KNOB), module, WaveArtifact::MARKER_PAN_PARAM + i), MARKER_COLORS[i], true));

			// Granular engine: its own grain group in the WAVE / LFO section (see Layout.hpp), with LENGTH, P.SPRD,
			// SPREAD and WINDOW in its extra columns, and KEEP in the strip under the knobs.
			add(withArc(createLfoRateKnob<LinkedKnob<LfoRateKnobT<Trimpot>>>(GRAIN_DENSITY_KNOB, module, WaveArtifact::MARKER_GRAIN_DENSITY_PARAM + i,
				WaveArtifact::MARKER_GRAIN_SYNC_PARAM + i, WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM + i,
				WaveArtifact::MARKER_GRAIN_DOTTED_PARAM + i), MARKER_COLORS[i]), G);
			add(withArc(createLfoRateKnob<LinkedKnob<LfoRateKnobT<Trimpot>>>(GRAIN_SIZE_KNOB, module, WaveArtifact::MARKER_GRAIN_SIZE_PARAM + i,
				WaveArtifact::MARKER_GRAIN_SYNC_PARAM + i, WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM + i,
				WaveArtifact::MARKER_GRAIN_DOTTED_PARAM + i), MARKER_COLORS[i]), G);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(GRAIN_SPRAY_KNOB), module, WaveArtifact::MARKER_GRAIN_SPRAY_PARAM + i), MARKER_COLORS[i]), G);
			add(createGrainWindowScreen<AboveTooltip<GrainWindowScreen>>(GRAIN_WINDOW_SCREEN, GRAIN_WINDOW_SIZE, module, WaveArtifact::MARKER_GRAIN_WINDOW_PARAM + i, MARKER_COLORS[i]), G);
			add(createLitToggle<AboveTooltip<LitToggle>>(GRAIN_SYNC_BUTTON, module, WaveArtifact::MARKER_GRAIN_SYNC_PARAM + i, "SYNC", MARKER_COLORS[i]), G);
			add(createLitToggle<AboveTooltip<LitToggle>>(GRAIN_TRIP_BUTTON, module, WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM + i, "T", MARKER_COLORS[i], LFO_TOGGLE_SMALL), G);
			add(createLitToggle<AboveTooltip<LitToggle>>(GRAIN_DOT_BUTTON, module, WaveArtifact::MARKER_GRAIN_DOTTED_PARAM + i, "D", MARKER_COLORS[i], LFO_TOGGLE_SMALL), G);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(GRAIN_PSCATTER_KNOB), module, WaveArtifact::MARKER_GRAIN_PSCATTER_PARAM + i), MARKER_COLORS[i]), G);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(GRAIN_SCAN_KNOB), module, WaveArtifact::MARKER_GRAIN_SCAN_PARAM + i), MARKER_COLORS[i], true), G);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(GRAIN_FADE_KNOB), module, WaveArtifact::MARKER_GRAIN_SCAN_FADE_PARAM + i), MARKER_COLORS[i]), G);
			// The scan loop's length, in the grain group's extra column: also dragged from the map's loop handle.
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(GRAIN_LENGTH_KNOB), module, WaveArtifact::MARKER_GRAIN_SCAN_LENGTH_PARAM + i), MARKER_COLORS[i]), G);
			{
				ModeButton* loop = createModeButton<AboveTooltip<ModeButton>>(GRAIN_SCAN_MODE_BUTTON, module, WaveArtifact::MARKER_GRAIN_SCAN_MODE_PARAM + i, {"LOOP", "PING"}, MARKER_COLORS[i]);
				loop->box.size = mm2px(GRAIN_SCAN_MODE_SIZE);
				loop->box.pos = mm2px(GRAIN_SCAN_MODE_BUTTON).minus(loop->box.size.div(2.f));
				add(loop, G);
			}
			// SAMPLE, in playing order (see Layout.hpp): LENGTH, LOOP, FADE after START (the same LENGTH and FADE params
			// as GRAIN's), and under them BEAT, ONE | LOOP | PING and the direction. The transient arrows under START
			// are one pair for the selected marker (added after this loop).
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(SAMPLE_LENGTH_KNOB), module, WaveArtifact::MARKER_GRAIN_SCAN_LENGTH_PARAM + i), MARKER_COLORS[i]), S);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(SAMPLE_LOOP_KNOB), module, WaveArtifact::MARKER_SAMPLE_LOOP_POINT_PARAM + i), MARKER_COLORS[i]), S);
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(SAMPLE_FADE_KNOB), module, WaveArtifact::MARKER_GRAIN_SCAN_FADE_PARAM + i), MARKER_COLORS[i]), S);
			{
				// FREE / BEAT / FIT: LENGTH and LOOP in time, in note values, or fitted by speed
				ModeButton* beat = createModeButton<AboveTooltip<ModeButton>>(SAMPLE_BEAT_BUTTON, module, WaveArtifact::MARKER_SAMPLE_BEAT_PARAM + i, {"FREE", "BEAT", "FIT"}, MARKER_COLORS[i]);
				beat->box.size = mm2px(SAMPLE_BEAT_SIZE);
				beat->box.pos = mm2px(SAMPLE_BEAT_BUTTON).minus(beat->box.size.div(2.f));
				add(beat, S);
			}
			add(createLitToggle<AboveTooltip<LitToggle>>(SAMPLE_HIT_BUTTON, module, WaveArtifact::MARKER_SAMPLE_HIT_SNAP_PARAM + i, "HIT", MARKER_COLORS[i], SAMPLE_HIT_SIZE), G | S);
			for (int b = 0; b < 2; b++) {
				ModeButton* mb = b == 0
					? createModeButton<AboveTooltip<ModeButton>>(SAMPLE_LOOP_MODE_BUTTON, module, WaveArtifact::MARKER_SAMPLE_LOOP_PARAM + i, {"ONE", "LOOP", "PING"}, MARKER_COLORS[i])
					: createModeButton<AboveTooltip<ModeButton>>(SAMPLE_DIR_BUTTON, module, WaveArtifact::MARKER_GRAIN_DIR_PARAM + i, {"FWD", "REV", "RND"}, MARKER_COLORS[i]);
				Vec at = b == 0 ? SAMPLE_LOOP_MODE_BUTTON : SAMPLE_DIR_BUTTON;
				mb->box.size = mm2px(SAMPLE_BUTTON_SIZE);
				mb->box.pos = mm2px(at).minus(mb->box.size.div(2.f));
				add(mb, S);
			}
			add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(SPREAD_KNOB), module, WaveArtifact::MARKER_GRAIN_SPREAD_PARAM + i), MARKER_COLORS[i]), G);
			add(createLitToggle<AboveTooltip<LitToggle>>(KEEP_SPEED_BUTTON, module, WaveArtifact::MARKER_GRAIN_KEEP_SPEED_PARAM + i, "KEEP", MARKER_COLORS[i], KEEP_SPEED_BUTTON_SIZE), G);

			// POLY: the marker's envelope, on both engines.
			for (int a = 0; a < WaveArtifact::NUM_ENVELOPE_PARAMS; a++) {
				editKnobPoly[k] = true;
				add(withArc(createParamCentered<LinkedKnob<Trimpot>>(mm2px(Vec(ADSR_KNOB_X[a], ADSR_Y)), module, WaveArtifact::MARKER_ENVELOPE_PARAMS[a] + i), MARKER_COLORS[i]));
			}
		}
		// SAMPLE: the transient arrows under START
		for (int d = 0; d < 2; d++) {
			TransientArrow* a = createWidget<TransientArrow>(Vec(0, 0));
			a->module = module;
			a->dir = d == 0 ? -1 : 1;
			a->box.size = mm2px(SAMPLE_ARROW_SIZE);
			a->box.pos = mm2px(d == 0 ? SAMPLE_PREV_BUTTON : SAMPLE_NEXT_BUTTON).minus(a->box.size.div(2.f));
			a->setVisible(false);
			addChild(a);
			transientArrows[d] = a;
		}
		// SAMPLE: the HITS slider under them (how many hits are found)
		{
			MiniSlider* s = createParam<MiniSlider>(mm2px(SAMPLE_HITS_SLIDER_POS), module, WaveArtifact::SAMPLE_HIT_SENSITIVITY_PARAM);
			s->box.size = mm2px(SAMPLE_HITS_SLIDER_SIZE);
			s->setVisible(false);
			addParam(s);
			hitsSlider = s;
		}
		// A step selected on the sequencer: its values under the knobs.
		{
			StepValueBars* bars = createWidget<StepValueBars>(Vec(0, 0));
			bars->box.size = box.size;
			bars->module = module;
			for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++)
				for (int k = 0; k < EDIT_KNOBS; k++)
					if (editKnobs[i][k])
						bars->controls.push_back(editKnobs[i][k]);
			addChild(bars);
		}
		// MORPH (one voice crossfading between markers) or POLY (each marker its own note).
		{
			ModeButton* b = createModeButton(PLAY_MODE_BUTTON, module, WaveArtifact::PLAY_MODE_PARAM, {"MORPH", "POLY", "STEP", "ALL", "SEQ"}, SCREEN_TEXT);
			b->box.size = mm2px(PLAY_MODE_BUTTON_SIZE);
			b->box.pos = mm2px(PLAY_MODE_BUTTON).minus(b->box.size.div(2.f));
			b->colors = {SCREEN_TEXT, nvgRGB(0xff, 0xc8, 0x6e), nvgRGB(0x8f, 0xe3, 0xb0), nvgRGB(0xff, 0x9a, 0x9e), nvgRGB(0x8f, 0xb8, 0xff)};
			addParam(b);
			AdsrScreen* screen = createWidget<AdsrScreen>(mm2px(ADSR_SCREEN_POS));
			screen->box.size = mm2px(ADSR_SCREEN_SIZE);
			screen->module = module;
			screen->setVisible(false);
			addChild(screen);
			envelopeScreen = screen;
		}
		// Engine switch in the header: WAVE (wavetable), GRAIN (granular) or SAMPLE (player). The knobs keep their places
		// and change meaning (labels, tooltips and values follow).
		{
			ModeButton* b = createModeButton(Vec(21.f, 5.8f), module, WaveArtifact::ENGINE_MODE_PARAM, {"WAVE", "GRAIN", "SAMPLE"}, SCREEN_TEXT);
			b->box.size = mm2px(Vec(14.f, 5.4f));
			b->box.pos = mm2px(Vec(21.f, 5.8f)).minus(b->box.size.div(2.f));
			b->colors = {SCREEN_TEXT, nvgRGB(0xff, 0xc8, 0x6e), nvgRGB(0x8f, 0xe3, 0xb0)};
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
			addOutput(createOutputCentered<ThemedPJ301MPort>(mm2px(Vec(ARTOUT_L_X, y)), module, WaveArtifact::SEND_OUTPUT + i));
			addInput(createInputCentered<ThemedPJ301MPort>(mm2px(Vec(ARTOUT_R_X, y)), module, WaveArtifact::RETURN_INPUT + i));
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
			// A step selected on the sequencer: the selected marker's controls edit that step's locks (no Shift moves then).
			m->lockCaptureStep();
			int lockRow, lockStep;
			if (m->lockTarget(&lockRow, &lockStep))
				held = nullptr;
			// Live record: a knob held down (moving or not) writes its value into each step of the row as it plays.
			if (m->recordStep() >= 0) {
				widget::Widget* grabbed = APP->event->getDraggedWidget();
				StepShownValue* shown = dynamic_cast<StepShownValue*>(grabbed);
				ParamWidget* pw = dynamic_cast<ParamWidget*>(grabbed);
				if (shown && pw && pw->module == m && shown->stepShowing)
					m->setStepLock(pw->paramId, shown->stepShownValue);
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
			int engineBit = 1 << clamp((int) std::round(m->params[WaveArtifact::ENGINE_MODE_PARAM].getValue()), 0, 2);
			bool poly = m->params[WaveArtifact::PLAY_MODE_PARAM].getValue() > 0.5f;
			for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
				for (int k = 0; k < EDIT_KNOBS; k++) {
					int e = editKnobEngine[k];
					editKnobs[i][k]->setVisible(i == m->selectedMarker && (e == 0 || (e & engineBit)) && (poly || !editKnobPoly[k]));
				}
			}
			for (TransientArrow* a : transientArrows)
				if (a)
					a->setVisible(engineBit & (G | S));
			if (hitsSlider)
				hitsSlider->setVisible(engineBit & (G | S));
			envelopeLabels->setVisible(poly);
			envelopeScreen->setVisible(poly);
		}
		ModuleWidget::step();
	}

	void onButton(const ButtonEvent& e) override {
		ModuleWidget::onButton(e);
		// A left click on the panel itself (no control took it): leave the sequencer step being locked, as a click
		// outside the steps on the sequencer does. (With "Lock module positions" Rack takes the click with no target.)
		bool onPanel = e.getTarget() == this || (e.getTarget() == nullptr && e.isConsumed());
		WaveArtifact* m = getModule<WaveArtifact>();
		if (m && onPanel && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			WaveArtifactSequencer* seq = m->sequencerOnLeft();
			if (seq)
				seq->select(-1, -1);
		}
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
			{"Wavetable (WAVE)", "Granular (GRAIN)", "Sample player (SAMPLE)"},
			[=]() {
				return (size_t) clamp((int) std::round(m->params[WaveArtifact::ENGINE_MODE_PARAM].getValue()), 0, 2);
			},
			[=](size_t i) {
				m->params[WaveArtifact::ENGINE_MODE_PARAM].setValue((float) i);
			}
		));
		// Granular settings that have no knob
		menu->addChild(createIndexSubmenuItem("Direction (selected marker, GRAIN / SAMPLE)",
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
		menu->addChild(createCheckMenuItem("Every note restarts the grains (POLY / SEQ)", "",
			[=]() {
				return m->params[WaveArtifact::GRAIN_NOTE_RESTART_PARAM].getValue() > 0.5f;
			},
			[=]() {
				Param& p = m->params[WaveArtifact::GRAIN_NOTE_RESTART_PARAM];
				p.setValue(p.getValue() > 0.5f ? 0.f : 1.f);
			}
		));
		// SAMPLE engine
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Sample player (SAMPLE)"));
		menu->addChild(createCheckMenuItem("Snap START and loops to zero crossings", "",
			[=]() {
				return m->params[WaveArtifact::SAMPLE_ZERO_SNAP_PARAM].getValue() > 0.5f;
			},
			[=]() {
				Param& p = m->params[WaveArtifact::SAMPLE_ZERO_SNAP_PARAM];
				p.setValue(p.getValue() > 0.5f ? 0.f : 1.f);
			}
		));
		// BEAT note values for LENGTH: straight always; triplets / dotted per marker (the same T / D as GRAIN's)
		for (int td = 0; td < 2; td++) {
			int id = td == 0 ? WaveArtifact::MARKER_GRAIN_TRIPLET_PARAM : WaveArtifact::MARKER_GRAIN_DOTTED_PARAM;
			menu->addChild(createCheckMenuItem(td == 0 ? "BEAT lengths: triplets too (selected marker)" : "BEAT lengths: dotted too (selected marker)", "",
				[=]() {
					return m->params[id + m->selectedMarker].getValue() > 0.5f;
				},
				[=]() {
					Param& p = m->params[id + m->selectedMarker];
					p.setValue(p.getValue() > 0.5f ? 0.f : 1.f);
				}
			));
		}
		menu->addChild(createMenuItem("Spread the markers over the transients", "", [=]() {
			history::ModuleChange* h = new history::ModuleChange;
			h->name = "spread markers over transients";
			h->moduleId = m->id;
			h->oldModuleJ = m->toJson();
			if (m->spreadMarkersOnTransients()) {
				h->newModuleJ = m->toJson();
				APP->history->push(h);
			}
			else {
				delete h;
			}
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
		menu->addChild(createCheckMenuItem("Mono output (L + R on both)", "", [=]() {
			return m->monoOutput.load();
		}, [=]() {
			m->monoOutput = !m->monoOutput.load();
		}));
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
		bool hasSequencer = isWaveArtifactSequencer(m->leftExpander.module);
		menu->addChild(createMenuItem("Add step sequencer (SEQ mode)", hasSequencer ? "added" : "", [=]() {
			addSequencer();
		}, hasSequencer));
	}

	/** Creates the step sequencer snapped to the LEFT of this module, pushing whatever is there further left.
	One undo step, which also moves the pushed modules back. */
	void addSequencer() {
		WaveArtifact* m = getModule<WaveArtifact>();
		if (!m || isWaveArtifactSequencer(m->leftExpander.module))
			return;
		std::map<int64_t, Vec> oldPos;
		for (ModuleWidget* w : APP->scene->rack->getModules())
			oldPos[w->module ? w->module->id : -1] = w->box.pos;

		Model* model = modelWaveArtifactSequencer;
		Module* sm = model->createModule();
		APP->engine->addModule(sm);
		ModuleWidget* smw = model->createModuleWidget(sm);
		APP->scene->rack->addModule(smw);
		APP->scene->rack->setModulePosForce(smw, Vec(box.pos.x - smw->box.size.x, box.pos.y));

		history::ComplexAction* complex = new history::ComplexAction;
		complex->name = "add Wave Artifacts sequencer";
		for (ModuleWidget* w : APP->scene->rack->getModules()) {
			if (w == smw || !w->module)
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
		h->setModule(smw);
		complex->push(h);
		APP->history->push(complex);
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
