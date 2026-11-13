#pragma once
#include "Lfo.hpp"
#include "MorphEngine.hpp"
#include "StepLocks.hpp"

// Messages between waveArtifact and its expanders (chained to its right, up to MAX_EXPANDERS).
// Rack double-buffers them, so each hop adds one sample of latency, irrelevant for CV.
//
//   main --ToExpander--> exp 1 --ToExpander--> exp 2 ...      (clock, chain position)
//   main <--ToMain------ exp 1 <--ToMain------ exp 2 ...      (summed modulation per marker)

namespace wa {

static const int MAX_EXPANDERS = 5;

enum ModTarget {
	MOD_WAVE,
	MOD_GAIN,
	MOD_CUTOFF,
	MOD_DRIVE,
	MOD_FINE,
	MOD_PAN,
	NUM_MOD_TARGETS
};

static const char* const MOD_TARGET_NAMES[NUM_MOD_TARGETS] = {"WAVE", "GAIN", "CUTOFF", "DRIVE", "FINE", "PAN"};

/** What 10 V at AMT 100% does to each target, in the marker param's own units: WAVE the whole
sample, CUTOFF its full 10-octave range (so ~1 V/oct), DRIVE its full range, FINE 2 semitones, PAN
hard left to hard right. GAIN is not additive: it works as a VCA (see vcaLevel). */
static const float MOD_SCALE_PER_10V[NUM_MOD_TARGETS] = {1.f, 0.f, 1.f, 1.f, 2.f, 2.f};

/** GAIN as a VCA. AMT 0 leaves the marker's level alone; AMT 100% makes the level follow the CV,
silent at 0 V and full (the GAIN knob) at 10 V; in between is a blend. CV below 0 V or above 10 V is
clamped, so the level never inverts or exceeds the knob. */
inline float vcaLevel(float amt, float cvVolts) {
	float cv = std::min(std::max(cvVolts / 10.f, 0.f), 1.f);
	float a = std::min(std::max(amt, 0.f), 1.f);
	return 1.f - a + a * cv;
}

/** The expander's four CV inputs: each goes to one of the artifact's knobs, picked per engine (the name above the
jack is a menu). */
static const int NUM_CV_SLOTS = 4;
static const int NUM_CV_ENGINES = 3;

/** Where a CV input goes. Saved in patches by number: only append. */
enum CvDest {
	CV_WAVE,
	CV_GAIN,
	CV_CUTOFF,
	CV_RES,
	CV_DRIVE,
	CV_PITCH,
	CV_FINE,
	CV_PAN,
	CV_SMOOTH,
	CV_LFO_RATE,
	CV_LFO_AMT,
	CV_DENSITY,
	CV_SIZE,
	CV_SPRAY,
	CV_SPREAD,
	CV_PSCATTER,
	CV_SCAN,
	CV_FADE,
	CV_LENGTH,
	CV_LOOP_POINT,
	CV_ATTACK,
	CV_DECAY,
	CV_SUSTAIN,
	CV_HOLD,
	CV_RELEASE,
	CV_EXP_LFO_RATE,
	CV_EXP_LFO_DEPTH,
	CV_EXP_AD_DEPTH,
	NUM_CV_DESTS
};

struct CvDestInfo {
	/** The marker setting it moves (index in LOCK_NAMES, = WaveArtifact::MARKER_SETTING_PARAMS) */
	int setting;
	/** Its name on the wavetable engine, and on GRAIN / SAMPLE */
	const char* name;
	const char* grainName;
	/** The engines that offer it: bit 0 WAVE, bit 1 GRAIN, bit 2 SAMPLE */
	uint8_t engines;
	/** The audio-rate path (a ModTarget), or -1: added to the knob where the settings are read (every 32 samples) */
	int fast;
};

static const CvDestInfo CV_DESTS[NUM_CV_DESTS] = {
	{0, "WAVE", "START", 7, MOD_WAVE},
	{8, "GAIN", "GAIN", 7, MOD_GAIN},
	{9, "CUTOFF", "CUTOFF", 7, MOD_CUTOFF},
	{10, "RES", "RES", 7, -1},
	{12, "DRIVE", "DRIVE", 5, MOD_DRIVE},
	{13, "PITCH", "PITCH", 7, -1},
	{15, "FINE", "FINE", 5, MOD_FINE},
	{16, "PAN", "PAN", 7, MOD_PAN},
	{1, "SMOOTH", "SMOOTH", 1, -1},
	{2, "LFO RATE", "LFO RATE", 1, -1},
	{3, "LFO AMT", "LFO AMT", 1, -1},
	{17, "DENSITY", "DENSITY", 2, -1},
	{18, "SIZE", "SIZE", 2, -1},
	{19, "SPRAY", "SPRAY", 2, -1},
	{24, "SPREAD", "SPREAD", 2, -1},
	{25, "P.SPRD", "P.SPRD", 2, -1},
	{26, "SCAN", "SCAN", 2, -1},
	{35, "FADE", "FADE", 6, -1},
	{36, "LENGTH", "LENGTH", 6, -1},
	{38, "LOOP POINT", "LOOP POINT", 4, -1},
	{28, "ATTACK", "ATTACK", 7, -1},
	{29, "DECAY", "DECAY", 7, -1},
	{30, "SUSTAIN", "SUSTAIN", 7, -1},
	{31, "HOLD", "HOLD", 7, -1},
	{32, "RELEASE", "RELEASE", 7, -1},
	{41, "EXP LFO RATE", "EXP LFO RATE", 7, -1},
	{47, "EXP LFO DEPTH", "EXP LFO DEPTH", 7, -1},
	{52, "EXP AD DEPTH", "EXP AD DEPTH", 7, -1},
};

/** A destination's name on an engine (0 WAVE, 1 GRAIN, 2 SAMPLE) */
inline const char* cvDestName(int dest, int engine) {
	if (dest < 0 || dest >= NUM_CV_DESTS)
		return "";
	return engine == 0 ? CV_DESTS[dest].name : CV_DESTS[dest].grainName;
}

inline bool cvDestOffered(int dest, int engine) {
	return dest >= 0 && dest < NUM_CV_DESTS && ((CV_DESTS[dest].engines >> engine) & 1u);
}

/** Each engine's four inputs to start with: its main controls */
static const int CV_DEFAULTS[NUM_CV_ENGINES][NUM_CV_SLOTS] = {
	{CV_WAVE, CV_GAIN, CV_CUTOFF, CV_DRIVE},
	{CV_WAVE, CV_DENSITY, CV_SIZE, CV_SPRAY},
	{CV_WAVE, CV_LENGTH, CV_LOOP_POINT, CV_CUTOFF},
};

/** A CV on a knob that has no audio-rate path: `amount` is the share of the knob's range to add (10 V at 100% = the
whole range) */
struct CvKnobMod {
	int8_t marker;
	int8_t setting;
	float amount;
};
static const int MAX_CV_KNOB_MODS = MAX_EXPANDERS * NUM_CV_SLOTS;

struct ToExpanderMessage {
	/** The main module's clock, so expander LFOs can sync to CLOCK */
	ClockTracker clock;
	/** Chain position of the sender: 0 = main module, k = k-th expander */
	int index = 0;
	/** The sender is connected to a main module (possibly through other expanders) */
	bool linked = false;
	/** RESET arrived this sample: restart the LFOs */
	bool reset = false;
	/** The main module runs the granular engine: the CV inputs are labelled START / P.SPRD / DENSITY */
	bool granular = false;
	/** The main module runs the sample player: WAVE reads START and FINE is LENGTH */
	bool sample = false;
	/** The main module is in POLY: GATE plays the marker's own ADSR instead of scaling its level */
	bool poly = false;
	/** Each marker's expander LFO (-1..1, run by the main module: its settings are marker settings, lockable), for
	the expanders' LFO jacks */
	float modLfo[NUM_MARKERS] = {};
	/** Each artifact's SEND (what its filter gets, L and R, +-1 = 5 V), for the stereo SEND / RETURN module at the
	end of the chain */
	float sendL[NUM_MARKERS] = {}, sendR[NUM_MARKERS] = {};
};

/** Main -> sequencer (the sequencer sits on the LEFT of the main module). The beat position is a level and the
reset a counter, so a message seen twice or missed cannot play or skip a step. */
struct ToSequencerMessage {
	/** The module's clock (the CLOCK input, or the internal tempo), in beats (quarter notes) since RESET */
	double beatPos = 0.0;
	/** RESET happened: counts up, the sequencer restarts from step 1 */
	uint32_t resetCount = 0;
	/** Which markers are on: a row whose marker is off keeps time but plays nothing */
	bool markerOn[NUM_MARKERS] = {true, true, true, true, true};
};

/** Sequencer -> main: which marker was struck last, and how hard, with that step's parameter locks */
struct FromSequencerMessage {
	bool linked = false;
	uint32_t fireCount[NUM_MARKERS] = {};
	float velocity[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	/** The latest played step's locks (see StepLocks.hpp), its slide (0 = off, else the share of a step it glides
	over), and one step's length in beats */
	float lockValue[NUM_MARKERS][NUM_LOCKABLE] = {};
	uint64_t lockMask[NUM_MARKERS] = {};
	float slide[NUM_MARKERS] = {};
	double stepBeats[NUM_MARKERS] = {0.25, 0.25, 0.25, 0.25, 0.25};
	/** The latest played step is tied to the one before: its note goes on (see PolyVoices legato) */
	bool legato[NUM_MARKERS] = {};
	/** The row's next step is tied to the one playing: keep the note on until it comes (long gate). Live. */
	bool holdOpen[NUM_MARKERS] = {};
};

struct ToMainMessage {
	/** Volts x AMT, summed over every linked expander assigned to the marker (GAIN excluded) */
	float mod[NUM_MARKERS][NUM_MOD_TARGETS] = {};
	/** Product of every linked expander's VCA level and GATE (0 while a patched gate is low) for the marker (1 = untouched) */
	float gainMul[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	/** Volts on the expanders' V/OCT inputs, summed per artifact (added to the main V/OCT) */
	float voct[NUM_MARKERS] = {};
	/** POLY: an expander's GATE is patched to this artifact, and whether any of them is high */
	bool gatePatched[NUM_MARKERS] = {};
	bool gateHigh[NUM_MARKERS] = {};
	/** A linked expander is actively modulating this artifact (a CV with a non-zero amount, or V/OCT) */
	bool modulated[NUM_MARKERS] = {};
	/** An expander's LFO jack is patched for the artifact: its LFO must run even while it moves nothing */
	bool lfoJack[NUM_MARKERS] = {};
	/** The CVs on the other knobs (CvDestInfo::fast = -1), from every linked expander */
	CvKnobMod knobMods[MAX_CV_KNOB_MODS] = {};
	int numKnobMods = 0;
	/** The stereo SEND / RETURN module (end of the chain): an artifact's RET L is patched (its return replaces the
	artifact's filter, over the main panel's RETURN), with its volts (RET R unpatched: RET L on both sides) */
	bool stereoReturn[NUM_MARKERS] = {};
	float stereoRetL[NUM_MARKERS] = {}, stereoRetR[NUM_MARKERS] = {};
	/** One of its SEND jacks is patched (the engines work out the sends only while one is) */
	bool stereoSendWanted = false;
};

} // namespace wa
