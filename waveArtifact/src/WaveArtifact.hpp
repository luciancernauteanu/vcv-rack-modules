#pragma once
#include <atomic>
#include <mutex>
#include <thread>

#include "plugin.hpp"
#include "GrainEngine.hpp"
#include "Lfo.hpp"
#include "MarkerMod.hpp"
#include "MorphEngine.hpp"
#include "PolyVoices.hpp"
#include "WavetableBank.hpp"
#include "StepLocks.hpp"
#include "ExpanderMessages.hpp"
#include "LoopLayout.hpp"

struct WaveArtifactSequencer;

/** Lets a param's quantity format a value other than its param's, the way its tooltip shows it (custom quantities
included): see WaveArtifact::valueText. UI thread only; the param itself is never touched. */
struct ValueShown {
	bool showing = false;
	float shown = 0.f;
};
template <class TQ>
struct ShowsValue : TQ, ValueShown {
	float getValue() override {
		return showing ? shown : TQ::getValue();
	}
};

/** One WAV and everything that follows from it: the main sample (WaveArtifact::slots[0]) or an artifact's own.
Bank handoff: the slot's worker publishes to pendingBank. process() moves pending -> active and the old active ->
retired once retired is empty and what plays the slot has faded to silence; the UI thread deletes retired. So only
the UI thread ever frees a bank, and never while process() can still read it. */
struct SampleSlot {
	std::atomic<wa::WavetableBank*> pendingBank{nullptr};
	std::atomic<wa::WavetableBank*> activeBank{nullptr};
	std::atomic<wa::WavetableBank*> retiredBank{nullptr};
	/** An artifact's slot: its file was dropped (the UI cleared it); process() retires its bank once it is silent */
	std::atomic<bool> clearPending{false};
	/** Changes whenever the active bank changes (load, re-analysis, Initialize), so displays that cache something per
	bank (the wavetable map) know when to rebuild. Never compare bank pointers: a new bank can reuse a freed one's
	address. */
	std::atomic<int> bankSerial{0};
	std::atomic<bool> analyzing{false};
	/** Bumped by Initialize and by dropping the file: an analysis started before it is discarded when it finishes */
	std::atomic<int> analysisGeneration{0};

	// UI thread
	std::thread worker;
	std::vector<uint8_t> wavBytes;
	std::string fileName;
	/** Full path of the last WAV asked for (even if it failed to load), for previous / next in its folder */
	std::string filePath;
	/** WAVs in filePath's folder, sorted; scanned on each load (not per frame) */
	std::vector<std::string> folderWavs;
	/** What the patch saved, waiting for loadSavedSample */
	bool savedPending = false;
	std::string savedFilePath;
	std::string savedFileName;
	uint32_t savedHash = 0;
	/** Checksum of wavBytes, and of the copy last written to the patch's storage folder (and where) */
	uint32_t wavHash = 0;
	uint32_t storedHash = 0;
	std::string storedPath;

	// Hits (transients) for HIT, UI thread. Found again whenever the sample or the HITS slider changes, while the
	// engine switch is on GRAIN or SAMPLE.
	/** Frames of the slot's sample, sorted */
	std::vector<long> transients;
	/** Frames of the sample they were found in (to turn them into 0..1) */
	long transientFrames = 0;
	int transientsSerial = -1;
	float transientsSens = -1.f;
	double transientsTime = 0.0;
	/** The hit list for HIT: the UI puts a new one in `hitsPending`, process() takes it into `hitsActive` and leaves
	the old one in `hitsRetired`, which the UI frees (the bank swap's pattern: nothing is freed on the audio thread) */
	std::atomic<std::vector<long>*> hitsPending{nullptr};
	std::atomic<std::vector<long>*> hitsRetired{nullptr};
	std::vector<long>* hitsActive = nullptr;
	/** The sample's onset strength (wa::onsetStrength, UI thread), for the tempo; the bank it was worked out for */
	std::vector<float> onset;
	int onsetSerial = -1;
	/** The file's tempo found in its audio (UI thread, see WaveArtifact::fileTempoOf), and what it was worked out for */
	wa::FoundTempo fileTempo;
	bool fileTempoFromName = false;
	int fileTempoSerial = -1;
	double fileTempoPrefer = 0.0;

	/** UI thread: a file is loaded (or loading) */
	bool hasFile() const {
		return !wavBytes.empty();
	}
};

struct WaveArtifact : Module {
	static const int NUM_MARKERS = wa::NUM_MARKERS;

	/** Module::configParam / configSwitch, with a quantity that can also format other values (ShowsValue), so the
	tooltips can show trig locks in the setting's own units. */
	template <class TQ = ParamQuantity>
	TQ* configParam(int paramId, float minValue, float maxValue, float defaultValue, std::string name = "", std::string unit = "",
		float displayBase = 0.f, float displayMultiplier = 1.f, float displayOffset = 0.f) {
		return Module::configParam<ShowsValue<TQ>>(paramId, minValue, maxValue, defaultValue, name, unit, displayBase, displayMultiplier, displayOffset);
	}
	template <class TQ = SwitchQuantity>
	TQ* configSwitch(int paramId, float minValue, float maxValue, float defaultValue, std::string name = "", std::vector<std::string> labels = {}) {
		return Module::configSwitch<ShowsValue<TQ>>(paramId, minValue, maxValue, defaultValue, name, labels);
	}

	enum ParamId {
		POSITION_PARAM,
		POS_ATTEN_PARAM,
		FREQ_PARAM,
		LOAD_PARAM,
		// Marker state lives in params so it is saved automatically and the UI never writes
		// module fields that process() reads. Used from M4 on.
		ENUMS(MARKER_ACTIVE_PARAM, NUM_MARKERS),
		ENUMS(MARKER_POS_PARAM, NUM_MARKERS),
		/** Wavetable position 0..1 across the bank (start..end of the sample) */
		ENUMS(MARKER_WAVE_PARAM, NUM_MARKERS),
		/** Filter cutoff (was band-pass frequency; same ID). 20 Hz .. 20 kHz; the filter is always on. */
		ENUMS(MARKER_CUTOFF_PARAM, NUM_MARKERS),
		ENUMS(MARKER_PITCH_PARAM, NUM_MARKERS),
		ENUMS(MARKER_PAN_PARAM, NUM_MARKERS),
		ARRANGE_PARAM,
		// Appended (not grouped with the other marker params) so existing param IDs stay stable.
		ENUMS(MARKER_GAIN_PARAM, NUM_MARKERS),
		ENUMS(MARKER_RES_PARAM, NUM_MARKERS),
		/** WAVE glide time 0..1 -> 20 ms * 100^v (20 ms .. 2 s) */
		ENUMS(MARKER_SMOOTH_PARAM, NUM_MARKERS),
		/** WAVE LFO: rate knob 0..1 (Hz when free, note value when synced), amount, free/sync */
		ENUMS(MARKER_LFO_RATE_PARAM, NUM_MARKERS),
		ENUMS(MARKER_LFO_AMT_PARAM, NUM_MARKERS),
		ENUMS(MARKER_LFO_SYNC_PARAM, NUM_MARKERS),
		/** Note values the synced rate knob offers besides straight ones (set in RATE's right-click menu) */
		ENUMS(MARKER_LFO_TRIPLET_PARAM, NUM_MARKERS),
		ENUMS(MARKER_LFO_DOTTED_PARAM, NUM_MARKERS),
		/** wa::FilterMode: LP, BP, HP, notch */
		ENUMS(MARKER_FILTER_MODE_PARAM, NUM_MARKERS),
		ENUMS(MARKER_DRIVE_PARAM, NUM_MARKERS),
		/** Fine tune, semitones -1..1 (shown in cents) */
		ENUMS(MARKER_FINE_PARAM, NUM_MARKERS),
		/** Master filter: 0 = off, 1..4 = LP, BP, HP, notch */
		MASTER_MODE_PARAM,
		MASTER_CUTOFF_PARAM,
		MASTER_RES_PARAM,
		MASTER_SPREAD_PARAM,
		MASTER_FDBK_PARAM,
		MASTER_DRIVE_PARAM,
		/** CV amounts for the master knobs (-100%..100%, default 0) */
		MASTER_CUTOFF_AMT_PARAM,
		MASTER_RES_AMT_PARAM,
		MASTER_DRIVE_AMT_PARAM,
		MASTER_SPREAD_AMT_PARAM,
		MASTER_FDBK_AMT_PARAM,
		/** wa::LfoShape of each marker's WAVE LFO */
		ENUMS(MARKER_LFO_SHAPE_PARAM, NUM_MARKERS),
		/** The position LFO: same controls as a marker's. It moves POSITION, folding at the ends. */
		POS_LFO_SHAPE_PARAM,
		POS_LFO_RATE_PARAM,
		POS_LFO_AMT_PARAM,
		POS_LFO_SYNC_PARAM,
		POS_LFO_TRIPLET_PARAM,
		POS_LFO_DOTTED_PARAM,
		/** 0 = PITCH steps in whole semitones, 1 = free (continuous) */
		ENUMS(MARKER_PITCH_FREE_PARAM, NUM_MARKERS),
		/** Output gain 0..200% (100% = unity), before the output protection */
		MASTER_GAIN_PARAM,
		/** Index into wa::PPQN_VALUES: clock pulses per quarter note (right-click menu) */
		CLOCK_PPQN_PARAM,
		/** Internal tempo, used while CLOCK is unpatched (20..300 BPM) */
		INTERNAL_BPM_PARAM,
		/** Momentary: restart the beat grid and every LFO (same as a RESET gate) */
		RESTART_PARAM,
		/** Latching: silences every output (main and artifact outs) with a 5 ms fade */
		STOP_PARAM,
		/** Position LFO smoothing 0..1 -> 20 ms * 100^v (20 ms .. 2 s): a one-pole on the LFO alone, so the
		knob and CV stay fast and a saw / square edge becomes a ramp instead of a jump of the playhead */
		POS_LFO_SMOOTH_PARAM,
		/** Master GAIN CV amount -100%..100% (default 0): 10 V at 100% = the whole knob range (+200%) */
		MASTER_GAIN_AMT_PARAM,
		// ---- Granular engine (appended, so every earlier ID and saved patch stays valid) ----
		/** 0 = wavetable engine (default), 1 = granular engine, 2 = sample player: the knobs change meaning */
		ENGINE_MODE_PARAM,
		/** DENSITY: grains per second (free) or note value (SYNC), 0..1 */
		ENUMS(MARKER_GRAIN_DENSITY_PARAM, NUM_MARKERS),
		/** SIZE: ms (free) or x grain interval (SYNC), 0..1 */
		ENUMS(MARKER_GRAIN_SIZE_PARAM, NUM_MARKERS),
		/** SPRAY: random offset of the grain start, 0..1 */
		ENUMS(MARKER_GRAIN_SPRAY_PARAM, NUM_MARKERS),
		/** wa::GrainWindow */
		ENUMS(MARKER_GRAIN_WINDOW_PARAM, NUM_MARKERS),
		/** Grains on the clock grid / SCAN locked to the beat (default on), note kinds for DENSITY */
		ENUMS(MARKER_GRAIN_SYNC_PARAM, NUM_MARKERS),
		ENUMS(MARKER_GRAIN_TRIPLET_PARAM, NUM_MARKERS),
		ENUMS(MARKER_GRAIN_DOTTED_PARAM, NUM_MARKERS),
		/** SPREAD: random pan scatter 0..1 */
		ENUMS(MARKER_GRAIN_SPREAD_PARAM, NUM_MARKERS),
		/** P.SPRD: random pitch scatter 0..1 (up to +-12 st) */
		ENUMS(MARKER_GRAIN_PSCATTER_PARAM, NUM_MARKERS),
		/** SCAN -1..1: speed (free) or slice length in beats (SYNC); 0 = stays at START */
		ENUMS(MARKER_GRAIN_SCAN_PARAM, NUM_MARKERS),
		/** wa::GrainDirection (right-click menu) */
		ENUMS(MARKER_GRAIN_DIR_PARAM, NUM_MARKERS),
		/** 1 = the random scatter repeats every loop cycle while synced */
		GRAIN_SEEDED_PARAM,
		// ---- POLY play mode (appended) ----
		/** 0 = MORPH (the playhead crossfades neighboring markers, default), 1 = POLY (it starts their notes) */
		PLAY_MODE_PARAM,
		/** Each marker's ADSR in POLY: attack and decay / release 0..1 on a log scale (wa::adsrAttackSeconds,
		wa::adsrFallSeconds), sustain 0..1 */
		ENUMS(MARKER_ATTACK_PARAM, NUM_MARKERS),
		ENUMS(MARKER_DECAY_PARAM, NUM_MARKERS),
		ENUMS(MARKER_SUSTAIN_PARAM, NUM_MARKERS),
		ENUMS(MARKER_RELEASE_PARAM, NUM_MARKERS),
		/** How long a marker's note stays on (wa::adsrHoldSeconds); at the top, until another marker starts */
		ENUMS(MARKER_HOLD_PARAM, NUM_MARKERS),
		/** GRAIN: pitch changes the pitch but not the sample's timing (off = tape-like, the default) */
		ENUMS(MARKER_GRAIN_KEEP_SPEED_PARAM, NUM_MARKERS),
		/** GRAIN SCAN: 0 = LOOP (back to START), 1 = PING (forwards, then backwards) */
		ENUMS(MARKER_GRAIN_SCAN_MODE_PARAM, NUM_MARKERS),
		/** GRAIN SCAN: level fade at the slice ends, 0..0.5 of the slice each side */
		ENUMS(MARKER_GRAIN_SCAN_FADE_PARAM, NUM_MARKERS),
		/** GRAIN SCAN: loop length, wa::grainScanLengthFraction (top = up to the next marker, the default) */
		ENUMS(MARKER_GRAIN_SCAN_LENGTH_PARAM, NUM_MARKERS),
		/** 1 = muted: silent on both engines (its own output too), shown grey; it keeps its place and settings */
		ENUMS(MARKER_MUTE_PARAM, NUM_MARKERS),
		/** GRAIN, POLY / SEQ: 1 = every note restarts its cloud from the same point (grain grid, synced scan and
		scatter count from the note, not the beat), so equal trigs sound the same on any step */
		GRAIN_NOTE_RESTART_PARAM,
		/** SAMPLE engine: wa::PlayerLoop, what the slice end does (ONE / LOOP / PING) */
		ENUMS(MARKER_SAMPLE_LOOP_PARAM, NUM_MARKERS),
		/** SAMPLE engine: LOOP point, where LOOP / PING repeat from, 0..1 of the slice (0 = the whole slice) */
		ENUMS(MARKER_SAMPLE_LOOP_POINT_PARAM, NUM_MARKERS),
		/** SAMPLE engine: 1 = START, the slice end and the loop point snap to the nearest zero crossing (default) */
		SAMPLE_ZERO_SNAP_PARAM,
		/** SAMPLE engine HITS slider 0..1: how many hits (transients) are found, from only the strongest (0 %) to the
		softest too (100 %). Found again when it moves and when a sample loads (Transients.hpp). */
		SAMPLE_HIT_SENSITIVITY_PARAM,
		/** SAMPLE engine BEAT: 1 = the slice lasts a whole number of beats at the tempo */
		ENUMS(MARKER_SAMPLE_BEAT_PARAM, NUM_MARKERS),
		/** SAMPLE engine HIT: 1 = START plays from the nearest hit (transient) while the knob turns */
		ENUMS(MARKER_SAMPLE_HIT_SNAP_PARAM, NUM_MARKERS),
		/** Each marker's modulation (MarkerMod.hpp), set on its expander: an LFO and an AD, each with a destination
		(wa::ModDest) and a depth (-1..1). Marker settings like the others: lockable, copied with the marker. */
		ENUMS(MARKER_MOD_LFO_RATE_PARAM, NUM_MARKERS),
		ENUMS(MARKER_MOD_LFO_SHAPE_PARAM, NUM_MARKERS),
		ENUMS(MARKER_MOD_LFO_SYNC_PARAM, NUM_MARKERS),
		ENUMS(MARKER_MOD_LFO_TRIPLET_PARAM, NUM_MARKERS),
		ENUMS(MARKER_MOD_LFO_DOTTED_PARAM, NUM_MARKERS),
		ENUMS(MARKER_MOD_LFO_DEST_PARAM, NUM_MARKERS),
		ENUMS(MARKER_MOD_LFO_DEPTH_PARAM, NUM_MARKERS),
		/** AD times 0..1 (wa::modAdTime), PERC 0 = gate (GATE holds it) / 1 = percussive */
		ENUMS(MARKER_MOD_AD_ATTACK_PARAM, NUM_MARKERS),
		ENUMS(MARKER_MOD_AD_DECAY_PARAM, NUM_MARKERS),
		ENUMS(MARKER_MOD_AD_PERC_PARAM, NUM_MARKERS),
		ENUMS(MARKER_MOD_AD_DEST_PARAM, NUM_MARKERS),
		ENUMS(MARKER_MOD_AD_DEPTH_PARAM, NUM_MARKERS),
		/** SAMPLE engine: how long a note fades in, 0..3 = 1 / 3 / 10 / 30 ms (wa::SAMPLE_FADE_IN_SECONDS). Last, so
		older patches keep their IDs. */
		SAMPLE_FADE_IN_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		POS_INPUT,
		/** Clock for LFO sync; pulses per quarter note set by CLOCK_PPQN_PARAM */
		CLOCK_INPUT,
		MASTER_CUTOFF_INPUT,
		MASTER_RES_INPUT,
		MASTER_DRIVE_INPUT,
		MASTER_SPREAD_INPUT,
		MASTER_FDBK_INPUT,
		/** Restarts the beat count and every LFO (next clock pulse = start of a bar) */
		RESET_INPUT,
		/** Master GAIN CV (scaled by MASTER_GAIN_AMT_PARAM): a VCA on the main outputs */
		MASTER_GAIN_INPUT,
		/** Each artifact's RETURN: 2 channels (L, R) or 1 (both sides); replaces its filter while patched */
		ENUMS(RETURN_INPUT, NUM_MARKERS),
		INPUTS_LEN
	};
	enum OutputId {
		LEFT_OUTPUT,
		RIGHT_OUTPUT,
		/** The old per-artifact outputs (ARTIFACT OUT L / R), replaced by SEND / RETURN. The IDs stay reserved so
		later IDs keep their place; a cable to them is dropped when an old patch loads. */
		ENUMS(OLD_ARTIFACT_L_OUTPUT, NUM_MARKERS),
		ENUMS(OLD_ARTIFACT_R_OUTPUT, NUM_MARKERS),
		/** Beat clock: a 10 V, 5 ms trigger per quarter note from the clock in use (internal or CLOCK) */
		CLOCK_OUTPUT,
		/** Each artifact's SEND: what its filter gets, 2 channels (L, R), or 1 (L + R) once a mono module answers on
		its RETURN */
		ENUMS(SEND_OUTPUT, NUM_MARKERS),
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	/** The samples: slot 0 is the main one, slot 1 + i artifact i's own (empty: the artifact plays the main one) */
	static const int NUM_SLOTS = 1 + NUM_MARKERS;
	SampleSlot slots[NUM_SLOTS];
	/** Held while a worker publishes a bank and while Initialize clears them. */
	std::mutex publishMutex;
	/** Audio thread: the slot each marker plays (its own once its bank is in, else 0), as last applied */
	int markerSlot[NUM_MARKERS] = {};
	/** UI thread: the slot whose file artifact i plays or is loading (its own while it has a file, else 0) */
	int slotOf(int i) const {
		return (i >= 0 && i < NUM_MARKERS && slots[1 + i].hasFile()) ? 1 + i : 0;
	}
	/** UI thread: the slot the screen shows (the selected artifact's) */
	int shownSlot() const {
		return slotOf(selectedMarker);
	}
	/** The bank a marker plays, or null (UI thread: may be one being swapped out; read it only right away) */
	const wa::WavetableBank* bankOf(int i) const {
		const wa::WavetableBank* own = (i >= 0 && i < NUM_MARKERS) ? slots[1 + i].activeBank.load(std::memory_order_relaxed) : nullptr;
		return own ? own : slots[0].activeBank.load(std::memory_order_relaxed);
	}

	/** Table auditioned instead of the morph, or -1. Written by the UI, read by process(). Not saved. */
	std::atomic<int> previewTable{-1};
	/** The slot (file) the previewed table is in: the one the screen showed when it started */
	std::atomic<int> previewSlot{0};
	/** POLY: the marker whose slot the mouse is holding down, which plays it, or -1. Written by the UI. */
	std::atomic<int> pressedMarker{-1};

	// Written by process(), read by the displays.
	std::atomic<float> shownIndex[NUM_MARKERS];
	std::atomic<float> shownHeard[NUM_MARKERS];
	std::atomic<float> shownPosition{0.f};
	std::atomic<float> shownPreview{0.f};
	/** An expander is modulating this artifact (shown as a CV tag on the screen) */
	std::atomic<bool> shownCv[NUM_MARKERS];
	/** The artifact's RETURN replaces its filter (shown as a RET tag on the screen) */
	std::atomic<bool> shownReturn[NUM_MARKERS];
	/** SEND / RETURN (audio thread): a mono module answers on this artifact's RETURN (1 channel back while SEND sends
	2, for MONO_SEND_SAMPLES in a row), so SEND carries L + R on one channel. Reset when either cable is unplugged. */
	bool monoSend[NUM_MARKERS] = {};
	int monoCount[NUM_MARKERS] = {};
	/** Each artifact's send (L, R) of the last sample, for the stereo SEND / RETURN module along the expander chain */
	float chainSendL[NUM_MARKERS] = {}, chainSendR[NUM_MARKERS] = {};
	/** A poly module needs a few samples to answer with 2 channels after its input became stereo: about 5 ms */
	static const int MONO_SEND_SAMPLES = 256;
	/** Granular engine: where each cloud reads (0..1 of the sample), and a counter of its grain starts
	with the latest start, so the display can flash a grain. */
	std::atomic<float> shownGrainRead[NUM_MARKERS];
	/** Each scanning cloud's loop on the sample, 0..1 (end -1 while it does not scan) */
	std::atomic<float> shownScanStart[NUM_MARKERS];
	std::atomic<float> shownScanEnd[NUM_MARKERS];
	std::atomic<float> shownGrainPos[NUM_MARKERS];
	std::atomic<float> shownGrainSeconds[NUM_MARKERS];
	std::atomic<uint32_t> shownGrainCount[NUM_MARKERS];
	/** SAMPLE BEAT: the beats each marker's slice lasts now (0 = off) */
	std::atomic<float> shownBeats[NUM_MARKERS];

	// Audio thread only
	wa::MorphEngine engine;
	wa::GrainEngine grainEngine;
	/** Engine in use (0 wavetable, 1 granular, 2 sample player: grainEngine with GrainInput::player); follows ENGINE_MODE_PARAM after a fade to silence */
	int engineApplied = 0;
	uint32_t grainEventsSeen = 0;
	wa::PolyVoices voices;
	wa::MarkerStepper stepper;
	bool stepRunning = false;
	// SEQ: the step sequencer on the left of this module
	uint32_t seqResetCount = 0, seqSeen[NUM_MARKERS] = {};
	bool seqLinkedBefore = false;
	/** The beat position the sequencer follows: this module's clock (CLOCK input or internal tempo), locked to it */
	wa::GrainClock seqClock;
	/** The velocity of each marker's latest step (scales its level and its cutoff) */
	float seqVelocity[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	/** The velocity in use: jumps to a new one while the voice is silent, slews (5 ms) over a sounding one */
	float seqVelocityUsed[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	/** SEQ: each marker's parameter locks, from its row's latest played step (see StepLocks.hpp) */
	wa::MarkerLocks seqLocks[NUM_MARKERS];
	/** For each param ID: its index in MARKER_SETTING_PARAMS when it is a setting's first ID, else -1 */
	int8_t lockIndex[PARAMS_LEN];
	/** Settings that never glide (switches and lists), bit k = MARKER_SETTING_PARAMS[k] */
	uint64_t lockJumpMask = 0;
	/** A marker setting as it plays now: the knob, or in SEQ the playing step's lock, plus an expander CV sent to it
	(the knobs without an audio-rate path; see cvAdd). `base` is the setting's first ID. */
	float mv(int base, int i) {
		float knob = params[base + i].getValue();
		int k = lockIndex[base];
		if (k < 0)
			return knob;
		float v = seqLocks[i].get(k, knob);
		if ((cvMask[i] >> k) & 1u)
			v = std::min(std::max(v + cvAdd[i][k], settingMin[k]), settingMax[k]);
		return v;
	}
	/** The expanders' CVs on the knobs without an audio-rate path, as last received (wa::CvKnobMod) */
	wa::CvKnobMod cvKnobMods[wa::MAX_CV_KNOB_MODS];
	int numCvKnobMods = 0;
	/** Per marker, for the settings it is read with (refreshSettings): what the CVs add to each (in the setting's
	units), bit k set for the settings that have one */
	float cvAdd[NUM_MARKERS][wa::NUM_LOCKABLE];
	uint64_t cvMask[NUM_MARKERS] = {};
	/** Each setting's range (its ParamQuantity's), for the CVs */
	float settingMin[wa::NUM_LOCKABLE] = {}, settingMax[wa::NUM_LOCKABLE] = {};
	bool polyRunning = false;
	wa::ClockTracker clock;
	wa::Lfo lfos[NUM_MARKERS];
	wa::Lfo posLfo;
	/** Each marker's modulation (MarkerMod.hpp): its expander LFO and AD, the LFO's last value (sent to the expanders'
	LFO jacks), a note that starts the AD on the next sample, and the GATE as last seen (for its edges) */
	/** A marker's settings as they play (its knobs, or in SEQ the playing step's locks), and what follows from them
	(times, note values, lengths). The knobs only move at the screen's rate, so these are read every
	SETTINGS_INTERVAL samples instead of every sample (the module's main CPU saving); a marker whose locks just
	changed or glide (SLIDE), or a new bank, reads them on that very sample. Audio-rate inputs (expander CV, the
	LFOs, the AD, velocity) still act on every sample. */
	struct MarkerSettings {
		bool active = false, muted = false;
		float pos = 0.f, wave = 0.f;
		bool lfoSync = false, lfoTriplets = false, lfoDotted = false;
		int lfoShape = 0;
		float lfoRate = 0.f, lfoAmount = 0.f, waveSmooth = 0.f;
		float gain = 1.f, res = 0.f, pitch = 0.f, cutoff = 1.f, drive = 0.f, fine = 0.f, pan = 0.f;
		int filterMode = 0;
		bool modLfoSync = false, modLfoTriplets = false, modLfoDotted = false, adPerc = false;
		int modLfoShape = 0, modLfoDest = 0, adDest = 0;
		float modLfoRate = 0.f, modLfoDepth = 0.f, adAttack = 0.f, adDecay = 0.f, adDepth = 0.f;
		wa::AdsrSettings adsr;
		// GRAIN / SAMPLE
		float density = 0.f, size = 0.f, spray = 0.f, loopPoint = 0.f, beatLength = 1.f, pScatter = 0.f, scan = 0.f;
		float spread = 0.f, scanFade = 0.f, scanLength = -1.f;
		int window = 0, loopMode = 0, beatMode = 0, direction = 0;
		bool hitSnap = false, grainSync = false, grainTriplets = false, grainDotted = false, keepSpeed = false, scanPing = false;
	};
	static const int SETTINGS_INTERVAL = 32;
	MarkerSettings settings[NUM_MARKERS];
	int settingsCountdown = 0;
	/** The position LFO's SMOOTH time, read with the settings */
	float posSmoothTime = 0.f;
	/** The slots' bank serials summed when the settings were last read (a new bank anywhere reads them again) */
	int settingsSerial = -1;
	/** Audio thread, checked every SETTINGS_INTERVAL samples (a swap only starts a 5 ms fade, so a few samples later is
	fine): which artifacts' own samples wait to be swapped or dropped, and the slots' bank serials summed */
	bool markerSwap[NUM_MARKERS] = {};
	int slotsSerial = 0;
	int slotCheckCountdown = 0;
	/** Per marker: the locks it read with, its locks were gliding then, a new step's locks arrived */
	uint64_t settingsLockMask[NUM_MARKERS] = {};
	bool settingsGliding[NUM_MARKERS] = {};
	bool settingsDirty[NUM_MARKERS] = {};
	void refreshSettings(int i);
	/** LFOs only run while they are heard: these count down (samples) after the last moment they were, so a fade of
	their amount in the engine still gets their values */
	int waveLfoLive[NUM_MARKERS] = {};
	int modLfoLive[NUM_MARKERS] = {};
	wa::Lfo modLfos[NUM_MARKERS];
	wa::ModEnvelope modAds[NUM_MARKERS];
	float modLfoOut[NUM_MARKERS] = {};
	bool modAdNote[NUM_MARKERS] = {};
	bool modGateWas[NUM_MARKERS] = {};
	/** The position LFO after its SMOOTH one-pole */
	wa::LfoSmoother posLfoSmoother;
	dsp::SchmittTrigger resetTrigger;
	dsp::PulseGenerator clockOutPulse;
	bool restartHeld = false;
	/** Set by the UI (menu) to restart the LFOs on the next sample */
	std::atomic<bool> restartRequest{false};
	int displayCounter = 0;

	// UI thread only
	wa::TableCountMode tableCountMode = wa::TABLES_AUTO;
	/** How soft the artifacts are (see wa::TableTone); re-analyzes when changed. */
	wa::TableTone tableTone = wa::TONE_SMOOTH;
	int selectedMarker = 0;
	/** First param ID of every setting a marker copy carries: everything per marker except on/off and bar position */
	static const int NUM_MARKER_SETTINGS = 53;
	static const int MARKER_SETTING_PARAMS[NUM_MARKER_SETTINGS];
	/** The settings copied last (not saved with the patch), and the marker they came from, or -1 */
	float markerClipboard[NUM_MARKER_SETTINGS] = {};
	int markerClipboardFrom = -1;
	/** The selected marker's settings at the last linkStep, to see what changed */
	float linkLast[NUM_MARKER_SETTINGS] = {};
	int linkSelected = -1;
	/** First param ID of each part of a marker's envelope */
	static const int NUM_ENVELOPE_PARAMS = 5;
	static const int MARKER_ENVELOPE_PARAMS[NUM_ENVELOPE_PARAMS];
	/** Shift + a per-marker control: the setting being moved on every marker, or -1 (see linkStep) */
	int linkSetting = -1;
	/** The marker the move is made on */
	int linkMarker = -1;
	/** How far a Shift drag has gone past the moved knob's own limit, in its units (see LinkedKnob) */
	float linkBeyond = 0.f;
	/** Each marker's value and the moved marker's own value when the move began */
	float linkBase[NUM_MARKERS] = {};
	float linkBaseValue = 0.f;


	WaveArtifact();
	~WaveArtifact();

	void process(const ProcessArgs& args) override;
	void onSampleRateChange(const SampleRateChangeEvent& e) override;
	void onReset(const ResetEvent& e) override;
	/** The loaded WAVs go into the patch (its own storage folder, so the project opens without the original files),
	with their original paths, checksums, the table count and tone. Loaded again by the UI (loadSavedSample). */
	json_t* dataToJson() override;
	void dataFromJson(json_t* root) override;
	void onSave(const SaveEvent& e) override;
	/** UI thread, once after a patch or preset is loaded: the patch's copy of a slot's WAV when the checksum
	matches, else the original file */
	void loadSavedSample(int slot);
	/** Right-click: L + R on both outputs (saved with the patch), and how far the output has faded to it */
	std::atomic<bool> monoOutput{false};
	float monoMix = 0.f;
	/** Randomize also randomizes the expanders snapped on to the right (each keeps its own
	randomizeEnabled flags: MARKER, LFO shape, SYNC, T, D and PERC stay as they are). */
	void onRandomize(const RandomizeEvent& e) override;

	// Marker editing, UI thread. All state is in params, so these are saved and undo-free like knobs.
	bool isMarkerActive(int i);
	bool isMarkerMuted(int i) {
		return params[MARKER_MUTE_PARAM + i].getValue() > 0.5f;
	}
	int activeMarkerCount();
	/** Turns on the lowest-numbered free marker at `pos`, with default settings and the wavetable
	under that bar position. Returns it, or -1 when all 5 are in use. */
	int addMarker(float pos);
	/** Turns a marker off. The last active marker cannot be deleted. */
	void deleteMarker(int i);
	/** Sets a marker's wavetable to table `t` of the loaded bank. Turns the marker on (in the
	widest gap of the bar) if it was off, and selects it. */
	void assignTable(int i, int t);
	/** Nearest table to a marker's WAVE value, or -1 without a bank. */
	int markerTable(int i);
	/** Remembers all of a marker's settings, as they are now, for pasteMarker. */
	void copyMarker(int i);
	/** Overwrites a marker's settings with the copied ones and selects it. Its on/off and bar position stay. */
	void pasteMarker(int i);
	/** Call every frame, with the param under a Shift-held mouse (-1 if none) and whether its button is down.
	When the selected marker's setting changes there, every other active marker moves by the same amount from
	where it was when the move began, so the spacing comes back if one hits a limit. A drag past the moved knob's
	own limit keeps moving the others (linkBeyond). Switches and lists take the same value instead. Returns true once when a move has ended: the UI makes its undo step, then calls
	linkFinish(). */
	bool linkStep(int paramId, bool dragging);
	/** Shift + double-click: the setting behind `paramId` (a control of the selected marker) back to its default on
	every active marker (the selected one too), as one undo step. False if it is not a per-marker setting. */
	bool resetSettingOnAllMarkers(int paramId);
	void linkFinish();

	// Parameter locks from the main module's knobs, UI thread (see WaveArtifactSequencer). While a lit step of the
	// selected marker's row is selected on the sequencer to the left, that marker's controls edit the step's locks
	// instead of themselves.
	/** The sequencer snapped on the left, or null */
	WaveArtifactSequencer* sequencerOnLeft();
	/** True while a step is the lock target: its row and step */
	bool lockTarget(int* row, int* step);
	/** The setting index (MARKER_SETTING_PARAMS) of a param ID of marker `marker`, or -1 */
	int settingIndexOf(int paramId, int marker);
	/** The value the target step plays for `paramId` (its lock, or the knob) and whether it is locked. False when there
	is no target or the param is not one of the selected marker's settings. */
	bool stepValueFor(int paramId, float* value, bool* locked, int* step = nullptr);
	/** `value` of `paramId` as its tooltip shows it, units included ("12 st", "1/16"). UI thread. */
	std::string valueText(int paramId, float value);
	/** Extra tooltip line for a per-marker control while a trig of its row is selected on the sequencer: the value that
	trig plays for it (its lock, or the knob). Empty otherwise. UI thread. */
	std::string lockTooltip(int paramId);
	/** Live record (REC on the sequencer, a step of the selected marker's row selected): the row's step playing now
	(lock edits go there), -1 while the playhead is on an empty step (edits go nowhere), -2 when not recording. */
	int recordStep();
	/** A press on one of the selected marker's controls: the sequencer's lane shows that setting for the marker's row
	(its locks as bars, the other steps at the knob's value). Nothing for a setting that cannot be locked. */
	void showInLane(int paramId);
	/** How many lit steps of the selected marker's row lock the setting behind `paramId` (0 without a sequencer) */
	int stepsLockingInRow(int paramId);
	/** A sequencer step of `marker` is selected: the WAVE / START it plays (its lock, or the knob where it has none),
	for the wavetable map to show and edit instead of the live position. UI thread. */
	bool stepWaveShown(int marker, float* wave, bool* locked = nullptr);
	/** The marker whose selected trig's WAVE / START the wavetable map is dragging, or -1. UI thread. */
	int waveEditMarker = -1;
	/** That trig's WAVE / START is being moved right now (dragged on the map, or its knob held): its value, for the
	middle screen's slot to follow. UI thread. */
	bool stepWaveEditing(int marker, float* wave);
	/** Right-click menu of a per-marker control: with a sequencer on the left and a lockable setting, an item clearing
	that setting's trig locks on every step of the control's marker row (undoable). UI thread. */
	void appendLockMenu(ui::Menu* menu, int paramId);
	/** The trig locks of setting k on every step of marker `marker`'s row: how many there are, or (clear) removes them */
	int rowLocks(int marker, int k, bool clear = false);
	/** The step the bars showed last while recording (they keep it while the playhead is on an empty step) */
	int lastRecordStep = -1;
	/** The step a lock edit goes into: the playing one while recording, else the selected one. Null when none. */
	wa::StepData* lockWriteStep(int* row);
	/** Writes a lock on the target step (while recording: on the step playing now). False when there is no target. */
	bool setStepLock(int paramId, float value);
	/** Removes the target step's lock for `paramId` (double-click on a knob). False when there is no target. */
	bool clearStepLock(int paramId);
	/** Call every frame: a change to the selected marker's settings while a step is the target becomes a lock, and the
	control goes back to its own value (switches, buttons, the scroll wheel; knob drags lock directly, see LinkedKnob). */
	void lockCaptureStep();
	float lockSnap[NUM_MARKER_SETTINGS] = {};
	int lockSnapMarker = -1;
	/** Set by Initialize / Randomize: the knobs moved on their own, so the next lockCaptureStep() takes them as they
	are instead of turning the change into locks on the selected step (and putting the knobs back). */
	std::atomic<bool> lockResync{false};

	/** UI thread. Reads the file into a slot (0 main, 1 + i artifact i's own) and starts its analysis. */
	void loadFile(const std::string& path, int slot = 0);
	/** UI thread. Re-runs a slot's analysis on its loaded bytes, e.g. after the table count changes. */
	void startAnalysis(int slot);
	/** UI thread. Every slot with a file analyzed again (the table count or tone changed) */
	void startAnalysisAll();
	/** UI thread. Artifact i goes back to the main sample: its own file is dropped (its bank fades out first). */
	void clearArtifactSample(int i);
	/** UI thread. Loads the previous (dir = -1) or next (dir = +1) WAV in a slot's folder, sorted by name, wrapping
	around. Does nothing without a path. */
	void loadSibling(int dir, int slot = 0);
	/** UI thread. True when a slot's folder holds another WAV to step to. */
	bool hasSiblings(int slot = 0);
	/** UI thread. Frees banks that process() has retired. */
	void uiStep();
	/** A short name for a slot's file in the status line and menus ("Artifact 2: kick.wav") */
	std::string slotLabel(int slot);

	/** Finds a slot's hits again when its sample or the HITS slider changed (at most every 30 ms while it is
	dragged) and hands the list to the audio thread */
	void updateTransients(int slot);
	/** Moves a marker's START to the next (dir 1) or previous (dir -1) hit of its file. False when there is none. */
	bool stepStartToTransient(int marker, int dir);
	/** Puts the active markers' STARTs on hits spread over their files (the markers on one file share its hits) */
	bool spreadMarkersOnTransients();
	/** "Lay out the loop on the row" (SAMPLE, sequencer on the left): the marker's loop and how it would be laid out */
	struct LoopLayoutInfo {
		/** Empty when it can be laid out, else why not (shown in the menu) */
		std::string why;
		/** The loop: the marker's slice (START up to its LENGTH or the next marker), frames of its sample */
		long from = 0, frames = 0;
		/** The whole sample: frames and seconds; the loop's seconds */
		long sampleFrames = 0;
		double sampleSeconds = 0.0, seconds = 0.0;
		/** Hits inside the loop (at the HITS setting) */
		int hits = 0;
		/** The tempo in the file name, or 0; the loop's beats and tempo, and where they came from (shown in the menu):
		found in the audio, the file name, or (no clear beat) the module's tempo */
		double nameBpm = 0.0;
		int beats = 1;
		double bpm = 0.0;
		std::string tempoFrom;
	};
	LoopLayoutInfo loopLayoutInfo(int marker);
	/** A slot's file tempo found in its audio (for Shift + drag on the map): worked out again when the sample or the
	module's tempo (which octave) changes. With a tempo in the file name that agrees up to octaves, its exact value. */
	wa::FoundTempo fileTempoOf(int slot, bool* fromName = nullptr);
	/** A slot's onset strength curve (one value per wa::ONSET_HOP frames), worked out once per sample */
	const std::vector<float>& onsetOf(int slot);
	/** Replaces the marker's sequencer row with its loop laid out over `beats` (on the hits, or `grid`: every step),
	and sets the marker to play each slice up to the next trig. False when it cannot (see LoopLayoutInfo::why). */
	bool layoutLoopOnRow(int marker, int beats, bool grid);
	/** layoutLoopOnRow with one undo entry for both modules; `beats` <= 0: the guessed length */
	bool layoutLoopWithUndo(int marker, int beats, bool grid);
	/** The layout's menu entries (the slice, on the hits / every step, other lengths), and them as a submenu */
	void appendLoopLayoutItems(ui::Menu* menu, int marker);
	void appendLoopLayoutMenu(ui::Menu* menu, int marker);
	/** RND on a row: each lit trig (only the `marked` steps, when given) gets a random slice of the marker's loop with
	probability `amount`: START on one of the slices LAY makes, LENGTH up to the row's next trig. Step 1 stays. The
	rhythm, velocities and conditions stay. False when nothing changed. */
	bool randomizeSlices(int marker, const bool* marked, float amount);
	/** Audio thread: each marker's hit list for HIT (its file's), or null */
	const std::vector<long>* markerHits(int i) const {
		int k = markerSlot[i];
		return slots[k].hitsActive;
	}

	/** The file line of a slot (its last message: analyzing, the artifact count, an error) */
	std::string getStatus(int slot = 0);
	void setStatus(const std::string& s, int slot = 0);

private:
	std::mutex statusMutex;
	std::string status[NUM_SLOTS];
};

/** UI thread. File dialog, then loadFile(). */
void openLoadDialog(WaveArtifact* module, int slot = 0);
