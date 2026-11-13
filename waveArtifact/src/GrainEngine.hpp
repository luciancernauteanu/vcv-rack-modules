#pragma once
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Filters.hpp"
#include "Lfo.hpp"
#include "Limiter.hpp"
#include "MorphEngine.hpp" // NUM_MARKERS
#include "SamplePlayer.hpp"
#include "Transients.hpp"
#include "WavetableBank.hpp"

// Granular engine: the second engine of Wave Artifacts. No Rack dependencies (tested in tests/).
//
// Every active marker is its own grain cloud. A marker's START is a place in the loaded sample (the same
// 0..1 value the wavetable engine calls WAVE). The POSITION playhead crossfades the two neighboring
// clouds with equal-power weights, exactly like the wavetable morph, so clouds keep their own SIZE,
// DENSITY, pitch, filter and gain.
//
// Per cloud: grain scheduler -> grains (windowed, cubic reads of the sample, SPREAD's random pan, pitch) -> stereo
// bus -> stereo multimode filter -> GAIN -> position weight -> PAN (equal power, unity at center, the same as the
// wavetable engine; on a stereo sample it works as a balance). The sum goes through the master filter and the
// safety limiter (same blocks as the wavetable engine); each cloud can also have its own output.
//
// Timing (the part that makes it musical):
// - SYNC on: grains start on a grid of note values locked to the module's clock (beat position pulled in
//   over 20 ms like the LFOs, RESET = beat 0), with sub-sample timing.
// - The SCAN knob moves the read position through the marker's slice (START up to the next marker's START
//   in the sample, or the end). Free: speed in x real-time, 0 = frozen. Synced: the slice is fitted to a
//   number of beats and its phase is locked to the beat grid (a tempo-locked time stretch, no pitch change).
//   At the end of the slice it jumps back to START (LOOP) or turns round (PING: forwards, then backwards, each
//   way taking one loop). FADE lowers the cloud's level while the scan is near either end of its loop (0..50% of
//   it each side), so the turn or the jump back is a dip instead of an edge. It follows where the scan is, not
//   where each grain starts: synced grains that all land on the loop's start would otherwise all be silent.
//   At FADE 0 in LOOP, as before.
// - Random scatter (SPRAY, pitch, pan, direction) restarts from the same seed at the start of every loop
//   cycle while synced, so the random pattern repeats like a loop instead of drifting (SEEDED).
// - Pitch belongs to each cloud: V/OCT (0 V = original speed at C4's default), PITCH, FINE, scatter.
//
// KEEP SPEED (per marker): pitch no longer slows down or speeds up the sample. A pitched grain plays its piece of
// the sample faster or slower than real time, so it drifts from where an unpitched grain would be by |1 - rate| of
// its length. Each grain is centered on the unpitched grain's place and kept short enough for the drift to stay
// within +-KEEP_DRIFT_SECONDS / 2; that many more grains fire (whole subdivisions of the grid when synced), so the
// overlap and level stay as set. At rate 1 nothing changes.
//
// POLY play mode (GrainInput::poly): a cloud's weight is its marker's ADSR level instead of its position share
// (see PolyVoices.hpp). A note from silence starts the cloud as if it had been running (all its overlapping
// grains at once, each part way through), so a short attack is heard at once and the level does not dip.
// With RESTART (GrainInput::noteRestart) every note starts its cloud the same way: the grain grid, a synced scan and
// the scatter count from the note's start, not from the beat, so equal trigs on different steps sound the same.
//
// Click prevention: every grain has a >= 1.3 ms smooth edge and is placed so it fits inside the sample (a
// live pitch change that still reaches an end fades out there); clouds fade in and out with their weight
// (5 ms); a bank swap or STOP fades everything out first (5 ms) and clears the grains while silent.
//
// The Smear window turns a cloud into a continuous sound: SIZE sets how much of the sample each grain plays (at least
// SMEAR_MIN_SECONDS) and grains fire often enough for SMEAR_OVERLAP of them to overlap (DENSITY only counts above
// that rate, so a low DENSITY cannot turn the cloud into playback of the sample), each one starts where it best matches the grain it fades in over, and the cloud's
// bus runs through a short allpass diffuser that spreads the attacks repeated by every grain.

namespace wa {

enum GrainWindow {
	GRAIN_HANN,
	/** Flat top with soft edges: more of the source at full level */
	GRAIN_TUKEY,
	GRAIN_GAUSS,
	GRAIN_TRIANGLE,
	/** Fast attack, exponential decay: plucky, rhythmic */
	GRAIN_PERCUSSIVE,
	/** Slow rise, short release: grains that swell into the next */
	GRAIN_SWELL,
	GRAIN_BOX,
	/** Hann, plus the cloud's smear behavior: forced overlap, matched grain starts, diffusion */
	GRAIN_SMEAR,
	NUM_GRAIN_WINDOWS
};

static const char* const GRAIN_WINDOW_NAMES[NUM_GRAIN_WINDOWS] = {"Hann", "Tukey", "Gauss", "Triangle", "Percussive", "Swell", "Box", "Smear"};

enum GrainDirection {
	GRAIN_FORWARD,
	GRAIN_REVERSE,
	GRAIN_RANDOM_DIR,
	NUM_GRAIN_DIRECTIONS
};

static const char* const GRAIN_DIRECTION_NAMES[NUM_GRAIN_DIRECTIONS] = {"Forward", "Reverse", "Random (50 / 50)"};

/** Scan knob dead zone around the middle: no scanning (the cloud stays at START). */
static const float SCAN_DEADZONE = 0.03f;
static const int NUM_LOOP_BEATS = 8;
/** Slice lengths in beats offered by SCAN while synced */
static const float LOOP_BEATS[NUM_LOOP_BEATS] = {0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f, 16.f, 32.f};
/** Loop cycle (in beats) used for the seeded randomness while the scan is not moving */
static const float IDLE_CYCLE_BEATS = 4.f;

/** DENSITY knob, free: 0.5 .. 500 grains per second (log). */
inline float grainFreeHz(float k) {
	return 0.5f * std::exp(std::min(std::max(k, 0.f), 1.f) * 6.907755279f);
}
/** SIZE knob, free: 2 ms .. 1 s (log). */
inline float grainSizeFreeSeconds(float k) {
	return 0.002f * std::exp(std::min(std::max(k, 0.f), 1.f) * 6.214608098f);
}
/** SIZE knob, synced: 0.25 .. 8 times the grain interval (log). */
inline float grainSizeSyncRatio(float k) {
	return 0.25f * std::exp(std::min(std::max(k, 0.f), 1.f) * 3.465735903f);
}
/** SPRAY knob: random offset of the start, up to +-2 s (squared, fine control at the low end). */
inline float grainSpraySeconds(float k) {
	k = std::min(std::max(k, 0.f), 1.f);
	return 2.f * k * k;
}
/** LAY on GRAIN: grains that rebuild the sample as it is. Tukey grains (edges a quarter of the grain each) one every
3 / 4 of a grain: each fade out lies exactly over the next fade in, so they sum flat. 100 ms grains, 13.3 a second. */
static constexpr float LAY_GRAIN_SECONDS = 0.1f;
inline float layGrainSizeKnob() {
	return std::log(LAY_GRAIN_SECONDS / 0.002f) / 6.214608098f;
}
inline float layGrainDensityKnob() {
	return std::log(1.f / (0.75f * LAY_GRAIN_SECONDS) / 0.5f) / 6.907755279f;
}

/** P.SPRD knob: random pitch offset, up to +-12 semitones. */
inline float grainPitchScatterSemitones(float k) {
	return 12.f * std::min(std::max(k, 0.f), 1.f);
}
inline bool grainScanActive(float k) {
	return std::fabs(k) > SCAN_DEADZONE;
}
/** SCAN knob (-1..1), free: speed in x real-time (-2 .. +2). */
inline float grainScanSpeed(float k) {
	return grainScanActive(k) ? 2.f * std::min(std::max(k, -1.f), 1.f) : 0.f;
}
/** SCAN knob, synced: beats the slice lasts (|knob| picks the value, the sign the direction). */
inline float grainLoopBeats(float k) {
	float a = (std::min(std::fabs(k), 1.f) - SCAN_DEADZONE) / (1.f - SCAN_DEADZONE);
	int i = (int) std::lround(std::min(std::max(a, 0.f), 1.f) * (NUM_LOOP_BEATS - 1));
	return LOOP_BEATS[i];
}

/** SCAN LENGTH knob 0..1: the scan loop's length, 10 ms up to the whole sample (log), measured from START and cut at
the sample's end. At the very top: up to the next marker's START, as before (the default). */
static const float SCAN_LENGTH_TOP = 0.995f;
static const double SCAN_LENGTH_MIN_SECONDS = 0.01;

/** The loop length as a share of the sample, or -1 for "up to the next marker" */
inline float grainScanLengthFraction(float k, double sampleSeconds) {
	if (k >= SCAN_LENGTH_TOP)
		return -1.f;
	double shortest = std::min(1.0, SCAN_LENGTH_MIN_SECONDS / std::max(sampleSeconds, 1e-6));
	return (float) std::pow(shortest, 1.0 - std::max(k, 0.f) / SCAN_LENGTH_TOP);
}

/** The knob value for a loop length (share of the sample) */
inline float grainScanLengthKnob(double fraction, double sampleSeconds) {
	double shortest = std::min(1.0, SCAN_LENGTH_MIN_SECONDS / std::max(sampleSeconds, 1e-6));
	if (shortest >= 1.0)
		return 0.f;
	fraction = std::min(std::max(fraction, shortest), 1.0);
	return (float) (SCAN_LENGTH_TOP * (1.0 - std::log(fraction) / std::log(shortest)));
}

/** Window value at p (0..1 through the grain). All windows are 0 at both ends. */
inline float grainWindowValue(int shape, float p) {
	const float PI_F = 3.14159265f;
	p = std::min(std::max(p, 0.f), 1.f);
	switch (shape) {
		case GRAIN_TUKEY: {
			const float a = 0.5f; // fraction of the grain used by the two soft edges
			if (p < a * 0.5f)
				return 0.5f - 0.5f * std::cos(2.f * PI_F * p / a);
			if (p > 1.f - a * 0.5f)
				return 0.5f - 0.5f * std::cos(2.f * PI_F * (1.f - p) / a);
			return 1.f;
		}
		case GRAIN_GAUSS: {
			float x = (p - 0.5f) / 0.17f;
			return std::exp(-0.5f * x * x);
		}
		case GRAIN_TRIANGLE:
			return 1.f - std::fabs(2.f * p - 1.f);
		case GRAIN_PERCUSSIVE: {
			if (p < 0.02f)
				return std::sin(0.5f * PI_F * p / 0.02f);
			return std::exp(-5.f * (p - 0.02f) / 0.98f);
		}
		case GRAIN_SWELL:
			return grainWindowValue(GRAIN_PERCUSSIVE, 1.f - p);
		case GRAIN_BOX: {
			const float e = 0.08f;
			float a = std::min(p / e, 1.f), b = std::min((1.f - p) / e, 1.f);
			a = a * a * (3.f - 2.f * a);
			b = b * b * (3.f - 2.f * b);
			return a * b;
		}
		default:
			return 0.5f - 0.5f * std::cos(2.f * PI_F * p);
	}
}

/** Series allpass chain: flat magnitude, spreads transients over a few tens of ms. */
struct GrainDiffuser {
	static const int STAGES = 4;

	void setup(const float* delayMs, float sampleRate) {
		for (int s = 0; s < STAGES; s++) {
			buf[s].assign((size_t) std::max((int) std::lround(delayMs[s] * 0.001f * sampleRate), 1), 0.f);
			pos[s] = 0;
		}
	}

	void clear() {
		for (int s = 0; s < STAGES; s++)
			std::fill(buf[s].begin(), buf[s].end(), 0.f);
	}

	float process(float x) {
		for (int s = 0; s < STAGES; s++) {
			float d = buf[s][pos[s]];
			float v = x + COEF * d;
			buf[s][pos[s]] = v;
			x = d - COEF * v;
			if (++pos[s] >= (int) buf[s].size())
				pos[s] = 0;
		}
		return x;
	}

private:
	static constexpr float COEF = 0.6f;
	std::vector<float> buf[STAGES];
	int pos[STAGES] = {};
};

/** Beat position locked to the module's clock, in beats since RESET. */
struct GrainClock {
	double beatPos = 0.0;
	/** Beats per sample of the latest step */
	double advance = 0.0;
	/** The latest step was a hard snap to a beat (after RESET) */
	bool snapped = false;

	void reset() {
		needSnap = true;
	}

	void process(const ClockTracker& clock, float sampleRate) {
		snapped = false;
		double bps = 1.0 / (clock.beatPeriod() * sampleRate);
		// Where the position lands this sample: an edge means a beat starts on this very sample, so the
		// position after the update must equal the beat count.
		double next = beatPos + bps;
		bool internal = clock.isInternal();
		if (internal != wasInternal) {
			wasInternal = internal;
			needSnap = true; // another clock took over: line up with its next beat
		}
		if (clock.isEdge()) {
			double target = (double) clock.getBeatCount();
			if (needSnap) {
				next = target;
				correction = 0.0;
				needSnap = false;
				snapped = true;
			}
			else if (!internal) {
				// An external clock's beats arrive on whole samples while its tempo is a measurement:
				// pull the position toward each beat. The internal clock needs none of this: its tempo is
				// exact, so counting beats per sample in double precision is exact too (the beat edges
				// themselves are rounded to whole samples, which would only add jitter).
				double err = target - next;
				err -= std::round(err); // the shortest way round: -0.5 .. 0.5 beat
				correction = err;
			}
		}
		// Pulled in over ~20 ms, never faster than 90% of the tempo backwards (time stays monotonic).
		double pull = snapped ? 0.0 : correction * std::min(1.0, 1.0 / (CORRECTION_TIME * sampleRate));
		pull = std::max(pull, -0.9 * bps);
		correction -= pull;
		advance = bps + pull;
		beatPos = next + pull;
	}

private:
	static constexpr double CORRECTION_TIME = 0.02;
	double correction = 0.0;
	bool needSnap = false;
	bool wasInternal = true;
};

struct GrainMarkerInput {
	bool active = false;
	/** 0..1 along the bar */
	float pos = 0.f;
	/** START: 0..1 across the sample (already includes CV) */
	float start = 0.f;
	/** DENSITY knob 0..1 (Hz free, note value synced) */
	float density = 0.5f;
	/** SIZE knob 0..1 */
	float size = 0.4f;
	/** SPRAY knob 0..1 */
	float spray = 0.f;
	int window = GRAIN_HANN;
	/** Grains on the clock grid (and SCAN locked to the beat) instead of free-running */
	bool sync = false;
	bool triplet = false;
	bool dotted = false;
	/** SPREAD knob 0..1: random pan scatter */
	float spread = 0.f;
	/** Where the whole cloud sits, -1 (left) .. 1 (right) */
	float pan = 0.f;
	/** P.SPRD knob 0..1 */
	float pitchScatter = 0.f;
	/** SCAN knob -1..1 */
	float scan = 0.f;
	int direction = GRAIN_FORWARD;
	/** SAMPLE engine: wa::PlayerLoop at the slice end (ONE loops too in MORPH, where there are no notes) */
	int loopMode = PLAYER_LOOP;
	/** SAMPLE engine: where LOOP / PING repeat from, 0..1 of the slice (PlayerSettings::loopPoint) */
	float loopPoint = 0.f;
	/** SAMPLE engine: DRIVE before the filter, 0..1 (the wavetable engine's WarmDrive; 0 = untouched) */
	float drive = 0.f;
	/** SAMPLE engine BEAT button: wa::SampleBeatMode. CUT: LENGTH is `beatLength` beats at the tempo, cut at the
	sample's own speed; FIT: the speed changes so what repeats lasts a whole number of beats. Both put LOOP on a grid of the slice (sampleLoopSteps). */
	int beatMode = SAMPLE_BEAT_OFF;
	float beatLength = 1.f;
	/** HIT (SAMPLE and GRAIN): START plays from the nearest hit (GrainInput::hits) */
	bool hitSnap = false;
	/** KEEP SPEED: pitch changes the pitch but not the sample's timing */
	bool keepSpeed = false;
	/** SCAN: forwards then backwards (PING) instead of jumping back to START (LOOP) */
	bool scanPing = false;
	/** SCAN: share of the slice at each end over which the level fades (0..0.5) */
	float scanFade = 0.f;
	/** SCAN: the loop's length as a share of the sample, from START; negative = up to the next marker's START */
	float scanLength = -1.f;
	float cutoff = 1.f;
	int filterMode = MODE_LP;
	float res = 0.f;
	/** Linear gain 0..4 */
	float gain = 1.f;
	/** Semitones (PITCH + FINE) */
	float pitch = 0.f;
	/** Extra octaves for this cloud only (expander V/OCT) */
	float voct = 0.f;
};

struct GrainInput {
	GrainMarkerInput markers[NUM_MARKERS];
	/** 0..1 along the bar */
	float position = 0.f;
	/** Octaves; 0 V plays the sample at its original speed */
	float voct = 0.f;
	/** The main sample's bank: every cloud without its own plays it */
	const WavetableBank* bank = nullptr;
	/** A cloud's own sample (null: the main one). Changing it fades that cloud alone out, clears it and starts it on
	the new one. */
	const WavetableBank* markerBank[NUM_MARKERS] = {};
	/** Fade this cloud alone out and keep it silent (its own sample is about to be swapped) */
	bool markerHold[NUM_MARKERS] = {};
	/** Needed for SYNC; without it every cloud runs free */
	const ClockTracker* clock = nullptr;
	/** RESET happened this sample */
	bool reset = false;
	/** Random scatter repeats every loop cycle while synced */
	bool seeded = true;
	bool holdSilent = false;
	/** Measuring output (tests): each cloud's own output for getDirect. The module never sets it, so it costs nothing
	there. */
	bool wantDirect[NUM_MARKERS] = {};
	/** SEND / RETURN (an expander's jacks): the marker's filter is replaced by the returned signal (engine units, retR = retL when mono) */
	bool insert[NUM_MARKERS] = {};
	float retL[NUM_MARKERS] = {};
	float retR[NUM_MARKERS] = {};
	/** POLY play mode: each cloud follows voiceEnv instead of the position crossfade */
	bool poly = false;
	/** Level of each marker's ADSR, 0..1 */
	float voiceEnv[NUM_MARKERS] = {};
	/** The marker started a new note on this sample */
	bool voiceStart[NUM_MARKERS] = {};
	/** POLY: every note restarts its cloud from the same point (RESTART). The grain grid, a synced SCAN and the
	seeded scatter count from the note's start instead of the beat grid, the scatter's random sequence starts over,
	and a note over the last one's release fades the old grains out (DECLICK_TIME) and starts like one from
	silence. So two equal trigs sound the same wherever they land. */
	bool noteRestart = false;
	/** SAMPLE engine: each cloud is one read head playing the sample as it is (SamplePlayer.hpp) instead of grains.
	The rest of the cloud (filter, gain, pan, POLY level, own output) and the master section are the same. */
	bool player = false;
	/** SAMPLE engine SNAP: START, the slice end and the loop point move to the nearest zero crossing */
	bool zeroSnap = false;
	/** SAMPLE engine: seconds a note fades in over (PLAYER_ATTACK_SECONDS by default) */
	float noteFade = (float) PLAYER_ATTACK_SECONDS;
	/** HIT: each cloud's hits to snap to (frames of its sample, sorted; the HITS slider), or null */
	const std::vector<long>* hits[NUM_MARKERS] = {};

	// Master section: same as EngineInput
	int masterMode = -1;
	float masterCutoff = 0.7f;
	float masterRes = 0.f;
	float masterSpread = 0.f;
	float masterFdbk = 0.f;
	float masterDrive = 0.f;
	float masterGain = 1.f;
	float masterCutoffMod = 0.f;
	float masterResMod = 0.f;
	float masterSpreadMod = 0.f;
	float masterFdbkMod = 0.f;
	float masterDriveMod = 0.f;
};

/** A grain just started (for the display) */
struct GrainEvent {
	int marker = 0;
	/** Start in the sample, 0..1 */
	float pos = 0.f;
	/** How much of the sample the grain reads, in seconds of the sample (less than its length when pitched down) */
	float seconds = 0.f;
};

class GrainEngine {
public:
	static const int MAX_GRAINS = 128;
	static const int MAX_PER_CLOUD = 64;
	static const int NUM_EVENTS = 32;

	GrainEngine() {
		const int N = WINDOW_POINTS;
		for (int s = 0; s < NUM_GRAIN_WINDOWS; s++) {
			double sum = 0.0;
			windowPeak[s] = 0.f;
			float highest = -1.f;
			for (int i = 0; i <= N; i++) {
				window[s][i] = grainWindowValue(s, (float) i / N);
				sum += (double) window[s][i] * window[s][i];
				if (window[s][i] > highest) {
					highest = window[s][i];
					windowPeak[s] = (float) i / N;
				}
			}
			meanSquare[s] = (float) (sum / (N + 1));
		}
		setSampleRate(44100.f);
	}

	void setSampleRate(float sr) {
		sampleRate = sr;
		smoothCoef = 1.f - std::exp(-1.f / (SMOOTH_TIME * sr));
		fadeStep = 1.f / (DECLICK_TIME * sr);
		limiter.setSampleRate(sr);
		master.setSampleRate(sr);
		// Different times per channel, so the two sides also decorrelate.
		static const float DIFFUSE_L[GrainDiffuser::STAGES] = {3.1f, 7.3f, 13.7f, 23.3f};
		static const float DIFFUSE_R[GrainDiffuser::STAGES] = {3.7f, 8.3f, 12.1f, 21.1f};
		for (int m = 0; m < NUM_MARKERS; m++) {
			clouds[m].directLimiter.setSampleRate(sr);
			clouds[m].diffL.setup(DIFFUSE_L, sr);
			clouds[m].diffR.setup(DIFFUSE_R, sr);
		}
		clearGrains();
	}

	float getSampleRate() const {
		return sampleRate;
	}

	bool isSilent() const {
		return gain <= 0.f;
	}

	/** True once a cloud's own fade (markerHold, or its sample changing) has reached silence */
	bool isMarkerSilent(int m) const {
		return gain <= 0.f || cloudGate[m] <= 0.f;
	}

	float getPosition() const {
		return posS;
	}

	/** Share of the position crossfade of a cloud (0..1, sums to 1) */
	float getHeard(int m) const {
		return heard[m];
	}

	/** Where the cloud reads now: START plus scan offset, 0..1 of the sample */
	/** Where a scanning cloud's loop starts and ends, 0..1 of the sample; end is -1 while it does not scan */
	void getScanSlice(int m, float* start, float* end) const {
		*start = clouds[m].shownSliceStart;
		*end = clouds[m].shownSliceEnd;
	}

	/** SAMPLE BEAT: the beats a marker's slice lasts now (0 = BEAT off) */
	double getBeats(int m) const {
		return clouds[m].beats;
	}

	float getReadPosition(int m) const {
		return clouds[m].shownRead;
	}

	int getActiveGrains() const {
		int n = 0;
		for (int m = 0; m < NUM_MARKERS; m++)
			n += clouds[m].active;
		return n;
	}

	int getActiveGrains(int m) const {
		return clouds[m].active;
	}

	/** Ring of the latest grain starts; eventCount() grows with every one. */
	const GrainEvent* events() const {
		return eventRing;
	}
	uint32_t eventCount() const {
		return eventsWritten;
	}

	static double syncedScanPlaceForTest(double beat, double loopBeats, float scan, bool ping) {
		return syncedScanPlace(beat, loopBeats, scan, ping);
	}

	static double keepLengthSecondsForTest(double size, double rate) {
		return keepLengthSeconds(size, rate);
	}

	float getLimiterGain() const {
		return limiter.getGain();
	}

	void getDirect(int m, float* l, float* r) const {
		*l = directL[m];
		*r = directR[m];
	}

	/** SEND: what this cloud's filter gets this sample (engine units, x5 V), before its level (position share or
	POLY envelope), GAIN and PAN; 0 while the cloud is idle */
	void getSend(int m, float* l, float* r) const {
		*l = sendL[m];
		*r = sendR[m];
	}

	void process(const GrainInput& in, float* outL, float* outR) {
		*outL = *outR = 0.f;
		// Each cloud's sample: its own, else the main one
		const SampleBuffer* cs[NUM_MARKERS];
		bool anySample = false;
		for (int m = 0; m < NUM_MARKERS; m++) {
			const WavetableBank* bk = bankFor(in, m);
			cs[m] = (bk && bk->sample.valid()) ? &bk->sample : nullptr;
			anySample = anySample || cs[m];
		}

		// Main bank swap: fade out, clear while silent, apply.
		bool changed = !bankApplied || in.bank != appliedBank || in.poly != polyApplied || in.player != playerApplied;
		bool wantSilent = in.holdSilent || changed || !anySample;
		gain = wantSilent ? std::max(0.f, gain - fadeStep) : std::min(1.f, gain + fadeStep);
		if (changed && gain <= 0.f) {
			appliedBank = in.bank; // identity only: it may already be freed, never read it
			bankApplied = true;
			polyApplied = in.poly;
			playerApplied = in.player;
			clearGrains();
			for (int m = 0; m < NUM_MARKERS; m++) {
				resetCloud(m);
				appliedCloudBank[m] = bankFor(in, m);
			}
		}
		// A cloud's own sample changing (or markerHold): that cloud alone fades out, is cleared, and starts on the new
		// one. While the whole engine is silent the change applies at once.
		for (int m = 0; m < NUM_MARKERS; m++) {
			const WavetableBank* want = bankFor(in, m);
			bool mChanged = want != appliedCloudBank[m];
			bool mSilent = in.markerHold[m] || mChanged || !cs[m];
			cloudGate[m] = mSilent ? std::max(0.f, cloudGate[m] - fadeStep) : std::min(1.f, cloudGate[m] + fadeStep);
			if (mChanged && (cloudGate[m] <= 0.f || gain <= 0.f)) {
				// appliedCloudBank is only an identity; it may already be freed. Never read it.
				appliedCloudBank[m] = want;
				clearCloudGrains(m);
				resetCloud(m);
			}
		}
		if (in.reset) {
			gclock.reset();
			for (int m = 0; m < NUM_MARKERS; m++) {
				clouds[m].lastCycle = LONG_MIN;
				clouds[m].anchored = false; // back on the beat grid until the next note
			}
		}
		if (in.clock)
			gclock.process(*in.clock, sampleRate);
		bool canSync = in.clock != nullptr;

		float target = std::min(std::max(in.position, 0.f), 1.f);
		posS += (target - posS) * smoothCoef;

		// Neighbors a, b and weight w from the live markers, sorted by bar position.
		int ord[NUM_MARKERS];
		int n = 0;
		for (int i = 0; i < NUM_MARKERS; i++) {
			if (in.markers[i].active)
				ord[n++] = i;
		}
		if (n == 0)
			ord[n++] = 0;
		for (int i = 1; i < n; i++) {
			int v = ord[i];
			int j = i - 1;
			while (j >= 0 && in.markers[ord[j]].pos > in.markers[v].pos) {
				ord[j + 1] = ord[j];
				j--;
			}
			ord[j + 1] = v;
		}
		int a = ord[0], b = ord[0];
		float w = 0.f;
		if (n > 1 && posS > in.markers[ord[0]].pos) {
			if (posS >= in.markers[ord[n - 1]].pos) {
				a = b = ord[n - 1];
			}
			else {
				for (int k = 0; k + 1 < n; k++) {
					float pa = in.markers[ord[k]].pos, pb = in.markers[ord[k + 1]].pos;
					if (posS <= pb) {
						a = ord[k];
						b = ord[k + 1];
						w = (pb - pa > 1e-6f) ? (posS - pa) / (pb - pa) : 0.f;
						break;
					}
				}
			}
		}
		float tw[NUM_MARKERS] = {};
		for (int m = 0; m < NUM_MARKERS; m++)
			heard[m] = 0.f;
		heard[a] += 1.f - w;
		heard[b] += w;
		// Equal power: two uncorrelated clouds crossfade without a dip.
		tw[a] += std::sqrt(1.f - w);
		tw[b] += std::sqrt(w);
		if (a == b)
			tw[a] = 1.f;
		if (polyApplied) {
			for (int m = 0; m < NUM_MARKERS; m++)
				heard[m] = tw[m] = std::min(std::max(in.voiceEnv[m], 0.f), 1.f);
		}

		for (int m = 0; m < NUM_MARKERS; m++) {
			float t = in.wantDirect[m] ? 0.f : 1.f;
			mainShare[m] = (t > mainShare[m]) ? std::min(t, mainShare[m] + fadeStep) : std::max(t, mainShare[m] - fadeStep);
		}

		// Schedulers
		float bufL[NUM_MARKERS] = {}, bufR[NUM_MARKERS] = {};
		if (anySample && gain > 0.f) {
			for (int m = 0; m < NUM_MARKERS; m++) {
				Cloud& c = clouds[m];
				const SampleBuffer* S = cs[m];
				if (polyApplied) {
					if (in.voiceStart[m]) {
						c.triggerFire = true;
						c.triggerFromSilence = c.w < 1e-4f;
						// Grains left over from the last note are silent now and must stay that way. Its place is
						// the one as of now: a CV can change on the very sample a gate starts the note.
						if (c.triggerFromSilence) {
							clearCloudGrains(m);
							c.panS = std::min(std::max(in.markers[m].pan, -1.f), 1.f);
						}
						else if (in.noteRestart) {
							fadeCloudGrains(m);
						}
						c.triggerRestart = in.noteRestart;
					}
					if (!in.noteRestart)
						c.anchored = false;
					c.w = tw[m]; // the ADSR is already smooth, and a smoother would hide a short attack
				}
				else {
					c.w += (tw[m] - c.w) * smoothCoef;
					c.anchored = false;
				}
				float gT = std::min(std::max(in.markers[m].gain, 0.f), (float) MAX_GAIN);
				c.gainS += (gT - c.gainS) * smoothCoef;
				// No sample for it: nothing to play (a cloud whose own is being swapped plays on through its fade)
				bool wanted = (tw[m] > 0.f || c.w > 1e-3f) && gT > 1e-4f && S;
				if (!wanted) {
					c.running = false;
					c.triggerFire = false;
					c.player.stop();
					// Not scheduled (the playhead is elsewhere): the map still shows its bullet at START, and its loop.
					c.shownRead = std::min(std::max(in.markers[m].start, 0.f), 1.f);
					if (S)
						updateShownSlice(m, in, *S);
					continue;
				}
				if (playerApplied)
					play(m, in, *S, &bufL[m], &bufR[m]);
				else
					schedule(m, in, *S, canSync);
			}
		}
		else {
			for (int m = 0; m < NUM_MARKERS; m++) {
				clouds[m].running = false;
				clouds[m].shownRead = std::min(std::max(in.markers[m].start, 0.f), 1.f);
				if (cs[m])
					updateShownSlice(m, in, *cs[m]);
			}
		}

		// Render all grains into the clouds' buses, each from its cloud's sample
		if (anySample) {
			for (int g = 0; g < MAX_GRAINS; g++) {
				Grain& gr = grains[g];
				if (!gr.active)
					continue;
				const SampleBuffer* S = cs[gr.marker];
				if (!S) {
					// Its sample is gone (the cloud is cleared on the swap): never read a freed one
					gr.active = false;
					clouds[gr.marker].active--;
					continue;
				}
				Cloud& c = clouds[gr.marker];
				const std::vector<float>& lv = S->levels[gr.level];
				long frames = (long) (lv.size() / (size_t) S->channels);
				float l, r;
				readCubic(lv.data(), frames, S->channels, gr.pos, &l, &r);
				float wv = windowAt(gr);
				float k = wv * gr.amp;
				// Only a live pitch change can bring a grain this close to an end: fade instead of a cut.
				double toEnd = std::min(gr.pos, (double) frames - 1.0 - gr.pos);
				if (toEnd < END_FADE) {
					float x = (float) std::max(toEnd, 0.0) / END_FADE;
					k *= x * x * (3.f - 2.f * x);
				}
				if (gr.fadeOut > 0.f) {
					k *= gr.fade * gr.fade * (3.f - 2.f * gr.fade);
					gr.fade -= gr.fadeOut;
				}
				if (gr.fadeIn > 0.f) {
					float x = gr.fadeInLevel;
					k *= x * x * (3.f - 2.f * x);
					gr.fadeInLevel += gr.fadeIn;
					if (gr.fadeInLevel >= 1.f) {
						gr.fadeInLevel = 1.f;
						gr.fadeIn = 0.f;
					}
				}
				bufL[gr.marker] += l * k * gr.gl;
				bufR[gr.marker] += r * k * gr.gr;
				gr.pos += gr.inc * c.liveMul;
				gr.age++;
				if (gr.age >= gr.len || gr.fade <= 0.f || gr.pos < -4.0 || gr.pos > frames + 4.0) {
					gr.active = false;
					c.active--;
				}
			}
		}

		// Clouds: filter, gain, position weight
		float l = 0.f, r = 0.f;
		for (int m = 0; m < NUM_MARKERS; m++) {
			Cloud& c = clouds[m];
			const GrainMarkerInput& M = in.markers[m];
			sendL[m] = sendR[m] = 0.f;
			float insertT = in.insert[m] ? 1.f : 0.f;
			c.insertS = (insertT > c.insertS) ? std::min(insertT, c.insertS + fadeStep) : std::max(insertT, c.insertS - fadeStep);
			// A silent cloud goes straight to its place, so a new note does not begin with a slide. This runs
			// even for a cloud idle long enough to skip the block below.
			float panT = std::min(std::max(M.pan, -1.f), 1.f);
			// SAMPLE: an LP fully open with no RES is no filter at all, so the sample passes untouched.
			bool filterOn = !(playerApplied && M.filterMode == MODE_LP && M.cutoff >= 0.999f && M.res <= 0.001f);
			if (c.w < 1e-4f) {
				c.panS = panT;
				// The filter too (a step's cutoff lock, a knob turned while it was silent): every note from silence
				// starts on its own settings, not with a glide from the last note's.
				modeWeights(filterOn, M.filterMode, c.mixS);
				c.cutS = std::min(std::max(M.cutoff, 0.f), 1.f);
				c.resS = std::min(std::max(M.res, 0.f), 1.f);
			}
			bool live = c.running || c.active > 0;
			if (live) {
				c.idle = 0;
			}
			else if (c.idle < IDLE_RESET) {
				c.idle++;
			}
			float yl = 0.f, yr = 0.f;
			if (c.idle < IDLE_RESET) {
				float mixT[NUM_MIX];
				modeWeights(filterOn, M.filterMode, mixT);
				for (int k = 0; k < NUM_MIX; k++)
					c.mixS[k] += (mixT[k] - c.mixS[k]) * smoothCoef;
				c.cutS += (std::min(std::max(M.cutoff, 0.f), 1.f) - c.cutS) * smoothCoef;
				c.resS += (std::min(std::max(M.res, 0.f), 1.f) - c.resS) * smoothCoef;
				float q;
				float fc = c.filterParams.get(c.cutS, c.resS, &q);
				// Its own fade (its sample being swapped); 1 the rest of the time
				float xl = bufL[m] * cloudGate[m], xr = bufR[m] * cloudGate[m];
				if (c.scanLevelTarget < 1.f || c.scanLevel < 1.f) {
					c.scanLevel += (c.scanLevelTarget - c.scanLevel) * smoothCoef;
					if (c.scanLevelTarget >= 1.f && c.scanLevel > 0.99999f)
						c.scanLevel = 1.f;
					xl *= c.scanLevel;
					xr *= c.scanLevel;
				}
				if (playerApplied)
					sampleDrive(c, M, &xl, &xr);
				float wetT = (!playerApplied && M.window == GRAIN_SMEAR) ? 1.f : 0.f;
				if (wetT > 0.f || c.smearWet > 0.f) {
					c.smearWet += (wetT - c.smearWet) * smoothCoef;
					if (wetT == 0.f && c.smearWet < 1e-4f) {
						c.smearWet = 0.f;
						c.diffL.clear();
						c.diffR.clear();
					}
					else {
						float dry = std::sqrt(std::max(1.f - c.smearWet, 0.f)), wet = std::sqrt(c.smearWet);
						xl = dry * xl + wet * c.diffL.process(xl);
						xr = dry * xr + wet * c.diffR.process(xr);
					}
				}
				// SEND: what the filter gets. RETURN (insert) takes the filter's place, faded over 20 ms both ways.
				sendL[m] = xl;
				sendR[m] = xr;
				float k = c.insertS;
				if (k < 1.f) {
					yl = c.svfL.process(xl, fc, q, sampleRate, c.mixS);
					yr = c.svfR.process(xr, fc, q, sampleRate, c.mixS);
					if (!std::isfinite(yl) || !std::isfinite(yr)) {
						c.svfL.reset();
						c.svfR.reset();
						yl = yr = 0.f;
					}
				}
				if (k > 0.f) {
					yl += (in.retL[m] - yl) * k;
					yr += (in.retR[m] - yr) * k;
				}
				// SAMPLE: the sound comes out of the drive's delay, so its envelope does too
				float w = playerApplied ? c.wDelayed : c.w;
				yl *= c.gainS * w;
				yr *= c.gainS * w;
				c.panS += (panT - c.panS) * smoothCoef;
				if (c.panS != 0.f) {
					float angle = (c.panS + 1.f) * M_PI_4_F;
					yl *= std::cos(angle) * SQRT2;
					yr *= std::sin(angle) * SQRT2;
				}
			}
			else if (c.idle == IDLE_RESET) {
				c.svfL.reset();
				c.svfR.reset();
				c.diffL.clear();
				c.diffR.clear();
				c.idle++;
			}
			l += yl * mainShare[m];
			r += yr * mainShare[m];
			if (in.wantDirect[m]) {
				c.directRunning = true;
				c.directLimiter.process(yl * gain, yr * gain, LIMIT, &directL[m], &directR[m]);
			}
			else {
				c.directRunning = false;
				directL[m] = directR[m] = 0.f;
			}
		}
		l *= gain;
		r *= gain;

		MasterParams mp;
		mp.mode = in.masterMode;
		mp.cutoff = in.masterCutoff;
		mp.res = in.masterRes;
		mp.spread = in.masterSpread;
		mp.fdbk = in.masterFdbk;
		mp.drive = in.masterDrive;
		mp.cutoffMod = in.masterCutoffMod;
		mp.resMod = in.masterResMod;
		mp.spreadMod = in.masterSpreadMod;
		mp.fdbkMod = in.masterFdbkMod;
		mp.driveMod = in.masterDriveMod;
		master.process(&l, &r, mp, smoothCoef, sampleRate);
		masterGainS += (std::min(std::max(in.masterGain, 0.f), (float) MAX_MASTER_GAIN) - masterGainS) * smoothCoef;
		limiter.process(l * masterGainS, r * masterGainS, LIMIT, outL, outR);
	}

private:
	static constexpr float SMOOTH_TIME = 0.005f;
	static constexpr float DECLICK_TIME = 0.005f;
	static constexpr float MAX_GAIN = 4.f;
	static constexpr float MAX_MASTER_GAIN = 2.f;
	static constexpr float LIMIT = 2.f;
	static constexpr float SQRT2 = 1.414213562f;
	static constexpr float M_PI_4_F = 0.785398163f;
	/** Samples a silent cloud keeps filtering (ring-out) before its filter memory is cleared */
	static const int IDLE_RESET = 24000;
	/** WarmDrive's constant latency is 30.5 samples (2x oversampling, two 63-tap half-band filters). The clean path is
	delayed 31, the nearest whole number: while DRIVE crosses in (5 ms) the two are half a sample apart, which dims the
	very top a little (measured: 12 kHz -0.7 dB at worst) and is gone once it has crossed. */
	static const int DRIVE_LATENCY = 31;
	static const int WINDOW_POINTS = 1024;
	static const int EDGE = 64;
	static const int MIN_LENGTH = 128;
	/** KEEP SPEED: the most a grain may drift from the unpitched timing over its length (centered: half each way) */
	static constexpr double KEEP_DRIFT_SECONDS = 0.04;
	/** KEEP SPEED: grains are never made shorter than this (at extreme pitches the drift then grows) */
	static constexpr double KEEP_MIN_SECONDS = 0.03;
	static const int KEEP_MAX_SUBDIVISIONS = 16;

	/** A grain's length under KEEP SPEED, at a playback rate relative to the source */
	static double keepLengthSeconds(double size, double rate) {
		double off = std::fabs(1.0 - rate);
		if (off < 1e-4)
			return size;
		return std::min(size, std::max(KEEP_MIN_SECONDS, KEEP_DRIFT_SECONDS / off));
	}
	/** Most grains started at once when a note starts */
	static const int MAX_NOTE_GRAINS = 16;
	/** Frames (of the grain's level) over which a grain fades when it reaches an end of the sample */
	static constexpr float END_FADE = 32.f;
	/** Frames of level 0 kept free at the end when placing a grain (covers the sub-sample start delay) */
	static constexpr double FIT_MARGIN = 32.0;
	/** Smear: grains overlapping at any time. Hann grains sum flat at whole numbers. */
	static constexpr double SMEAR_OVERLAP = 3.0;
	/** Smear: the shortest grain (SIZE's low end, 2 ms, would need hundreds of grains a second) */
	static constexpr double SMEAR_MIN_SECONDS = 0.02;
	/** Smear: above this grain rate the cloud is noise-like anyway and the search would cost too much */
	static constexpr double SMEAR_ALIGN_MAX_HZ = 50.0;
	/** Smear: how far a grain start may move to match the running grain (one period of 50 Hz) */
	static constexpr double SMEAR_SEARCH_SECONDS = 0.02;
	/** Highest playback rate relative to the source (3 octaves up: the last decimated level) */
	static constexpr float MAX_RATE = 8.f;
	/** Power a dense cloud keeps relative to the source (1 = the same loudness as the sample) */
	static constexpr float REF_MEAN_SQUARE = 1.0f;

	struct Grain {
		bool active = false;
		int marker = 0;
		int level = 0;
		/** Position and step in frames of `level` */
		double pos = 0.0;
		double inc = 0.0;
		int age = 0;
		int len = 0;
		int window = 0;
		float gl = 1.f, gr = 1.f;
		float amp = 1.f;
		/** RESTART: a grain of the last note fades out (fadeOut > 0, per sample) under the new note */
		float fade = 1.f;
		float fadeOut = 0.f;
		/** RESTART over a sounding note: the new note's grains fade in (fadeIn > 0, per sample) as the old ones fade
		out, so the restart is a crossfade. (They start part way through their windows, and without it each would
		begin with a step: a burst of noise on every trig.) */
		float fadeInLevel = 1.f;
		float fadeIn = 0.f;
	};

	struct Cloud {
		bool running = false;
		bool directRunning = false;
		int active = 0;
		int idle = IDLE_RESET + 1;
		float w = 0.f;
		float gainS = 1.f;
		float panS = 0.f;
		/** SCAN FADE: the level for where the scan is now, and its smoothed value */
		float scanLevelTarget = 1.f;
		float scanLevel = 1.f;
		float cutS = 1.f;
		float resS = 0.f;
		float mixS[NUM_MIX] = {0.f, 1.f, 0.f, 0.f, 0.f};
		Svf svfL, svfR;
		SafetyLimiter directLimiter;
		/** Playback rate from V/OCT, PITCH and the cloud's own voct (smoothed per sample) */
		float liveMul = 1.f;
		float liveKey = 1e9f;
		int syncState = -1;
		long lastN = 0;
		float lastInterval = -1.f;
		double gph = 1.0;
		double scanAccum = 0.0;
		long lastCycle = LONG_MIN;
		uint32_t rng = 1u;
		float shownRead = 0.f;
		float shownSliceStart = 0.f;
		float shownSliceEnd = -1.f;
		/** POLY: a note started; schedule() takes it on this cloud's next turn */
		bool triggerFire = false;
		bool triggerFromSilence = false;
		/** RESTART: the note restarts the cloud, and the beat it started on (the grid and a synced scan count from
		there while `anchored`) */
		bool triggerRestart = false;
		bool anchored = false;
		double anchor = 0.0;
		/** The grain started last (it may have ended since) */
		int lastGrain = -1;
		float smearWet = 0.f;
		GrainDiffuser diffL, diffR;
		/** SAMPLE engine: the cloud's read heads, and the direction of the note playing */
		PlayerVoice player;
		int playerDir = 1;
		/** SNAP cache: the last places asked for (START, slice end, loop point) and their zero crossings */
		long snapIn[3] = {-1, -1, -1};
		long snapOut[3] = {0, 0, 0};
		/** BEAT: the beats the slice lasts (0 = off) */
		double beats = 0.0;
		/** SAMPLE DRIVE: the drive, its smoothed amount, and how much of it is heard (0 = the clean path, delayed by
		the drive's latency so turning DRIVE never moves the sound in time) */
		WarmDrive driveL, driveR;
		float driveS = 0.f;
		float driveWet = 0.f;
		float cleanL[DRIVE_LATENCY + 1] = {};
		float cleanR[DRIVE_LATENCY + 1] = {};
		/** The note's level (w) delayed with the sound, so the envelope scales the very samples it was meant for (a
		retrigger's attack must not reach the old note's tail early) */
		float cleanW[DRIVE_LATENCY + 1] = {};
		float wDelayed = 0.f;
		/** RETURN: how far the returned sound has replaced the cloud's filter (0..1, 20 ms each way) */
		float insertS = 0.f;
		/** CUTOFF / RES as Hz and Q, worked out again only when they move */
		FilterParams filterParams;
		int cleanPos = 0;
		/** Samples the drive has run since it started (heard once past DRIVE_LATENCY) */
		int drivePrimed = 0;
	};

	void clearGrains() {
		for (int g = 0; g < MAX_GRAINS; g++)
			grains[g].active = false;
		for (int m = 0; m < NUM_MARKERS; m++)
			clouds[m].active = 0;
	}

	void clearCloudGrains(int m) {
		for (int g = 0; g < MAX_GRAINS; g++) {
			if (grains[g].marker == m)
				grains[g].active = false;
		}
		clouds[m].active = 0;
	}

	/** RESTART: the cloud's sounding grains fade out over DECLICK_TIME (they belong to the last note) */
	void fadeCloudGrains(int m) {
		for (int g = 0; g < MAX_GRAINS; g++) {
			if (grains[g].active && grains[g].marker == m && grains[g].fadeOut <= 0.f)
				grains[g].fadeOut = fadeStep;
		}
	}

	void resetCloud(int m) {
		Cloud& c = clouds[m];
		c.running = false;
		c.syncState = -1;
		c.gph = 1.0;
		c.scanAccum = 0.0;
		c.lastCycle = LONG_MIN;
		c.lastGrain = -1;
		c.triggerFire = false;
		c.triggerRestart = false;
		c.anchored = false;
		c.player.stop();
		c.driveL.reset();
		c.driveR.reset();
		c.driveS = c.driveWet = 0.f;
		c.drivePrimed = 0;
		c.snapIn[0] = c.snapIn[1] = c.snapIn[2] = -1;
		c.beats = 0.0;
		std::fill(c.cleanL, c.cleanL + DRIVE_LATENCY + 1, 0.f);
		std::fill(c.cleanR, c.cleanR + DRIVE_LATENCY + 1, 0.f);
		std::fill(c.cleanW, c.cleanW + DRIVE_LATENCY + 1, 0.f);
		c.wDelayed = 0.f;
		c.idle = IDLE_RESET + 1;
		c.w = 0.f;
		c.svfL.reset();
		c.svfR.reset();
		c.smearWet = 0.f;
		c.diffL.clear();
		c.diffR.clear();
	}

	/** RESTART: where a note's random sequence starts (the same for every note of a marker) */
	static uint32_t noteSeed(int m) {
		uint32_t h = (uint32_t) (m + 1) * 0x9E3779B1u ^ 0x5BD1E995u;
		h ^= h >> 15;
		h *= 0x2C1B3C6Du;
		h ^= h >> 12;
		return h ? h : 1u;
	}

	float random(Cloud& c) {
		uint32_t x = c.rng;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		c.rng = x;
		return (float) (x >> 8) * (1.f / 8388608.f) - 1.f;
	}

	/** SAMPLE engine: one sample of a cloud's read heads into its bus. A note (POLY / SEQ) or the cloud coming back
	(MORPH: the playhead reaches it again) starts it from START; a tied step goes on. */
	void play(int m, const GrainInput& in, const SampleBuffer& S, float* l, float* r) {
		Cloud& c = clouds[m];
		const GrainMarkerInput& M = in.markers[m];
		bool wasRunning = c.running;
		c.running = true;
		float key = in.voct + M.voct + M.pitch / 12.f;
		if (key != c.liveKey) {
			c.liveKey = key;
			c.liveMul = std::min(std::exp2(std::min(std::max(key, -6.f), 3.f)), (float) MAX_RATE);
		}
		double srcRate = (double) S.sampleRate;
		PlayerSettings p;
		double beatSec = in.clock ? std::max(in.clock->beatPeriod(), 1e-3) : 0.5;
		bool cut = M.beatMode == SAMPLE_BEAT_CUT && in.clock, fit = M.beatMode == SAMPLE_BEAT_FIT && in.clock;
		p.start = (double) std::min(std::max(M.start, 0.f), 1.f) * (double) S.frames;
		p.end = std::min(p.start + sliceFrames(in, m, S), (double) S.frames - 1.0);
		// HIT: START plays from the nearest hit (the knob turns smoothly, the sound jumps from hit to hit). The slice
		// keeps its end.
		bool onHit = false;
		if (M.hitSnap && in.hits[m]) {
			long h = nearestHit(*in.hits[m], p.start);
			if (h >= 0) {
				p.start = (double) h;
				onHit = true;
			}
		}
		p.loop = std::min(std::max(M.loopMode, 0), NUM_PLAYER_LOOPS - 1);
		if (!polyApplied && p.loop == PLAYER_ONE)
			p.loop = PLAYER_LOOP;
		p.xfade = std::max((double) std::min(std::max(M.scanFade, 0.f), 0.5f) * (p.end - p.start), PLAYER_MIN_XFADE_SECONDS * srcRate);
		// BEAT: LENGTH is a note value at the tempo, as heard: at another PITCH (or V/OCT) the slice holds more or less of
		// the sample, so it still lasts that note value. With LOOP the slice reads on by the crossfade, so one pass, up to
		// where the next one starts under it, lasts exactly that note value.
		double gridBeats = 0.0;
		double beatFrames = (double) M.beatLength * beatSec * srcRate * (double) c.liveMul;
		if (cut) {
			double len = std::max(beatFrames, 2.0);
			p.xfade = std::max((double) std::min(std::max(M.scanFade, 0.f), 0.5f) * len, PLAYER_MIN_XFADE_SECONDS * srcRate);
			p.xfade = std::min(p.xfade, 0.5 * len);
			p.end = std::min(p.start + len + (p.loop == PLAYER_LOOP ? p.xfade : 0.0), (double) S.frames - 1.0);
			gridBeats = M.beatLength;
		}
		else if (fit) {
			gridBeats = c.beats;
		}
		p.start = std::min(p.start, std::max(p.end - 1.0, 0.0));
		// LOOP point: free, or (BEAT, FIT) on a grid of the slice: 1/128 notes, 8 to 64 places (sampleLoopSteps)
		p.loopPoint = std::min(std::max(M.loopPoint, 0.f), 1.f);
		if (gridBeats > 0.0) {
			double steps = (double) sampleLoopSteps(gridBeats);
			p.loopPoint = std::min(std::round(p.loopPoint * steps), steps - 1.0) / steps;
		}
		// The repeated part: [loop point, end] forward, its mirror [start, loop point] reverse. In BEAT the loop point
		// is a share of the musical length (without the crossfade's read-on).
		{
			double len = cut ? std::min(beatFrames, p.end - p.start) : p.end - p.start;
			double lp = std::min(p.loopPoint, 0.99) * len;
			p.loopLo = c.playerDir > 0 ? p.start + lp : p.start;
			p.loopHi = c.playerDir > 0 ? p.end : p.start + len - lp;
		}
		// SNAP: every place a note or a loop starts from moves to the nearest zero crossing (up to 5 ms), so nothing
		// starts on a step. Cached: only a moved place is searched again. Not in BEAT: there the places are musical
		// times, and moving them would drift the loop (its crossfade and the note's attack keep it clean).
		if (in.zeroSnap && !cut) {
			// A hit is already placed exactly (on or just after a zero crossing): moving it could cut its attack.
			if (!onHit)
				p.start = snapped(c, 0, p.start, S);
			p.end = std::max(snapped(c, 1, p.end, S), p.start + 1.0);
			if (c.playerDir > 0)
				p.loopLo = std::min(std::max(snapped(c, 2, p.loopLo, S), p.start), p.end - 1.0);
			else
				p.loopHi = std::min(std::max(snapped(c, 2, p.loopHi, S), p.start + 1.0), p.end);
		}
		p.edge = PLAYER_EDGE_SECONDS * srcRate;
		p.rate = (double) c.liveMul * srcRate / sampleRate;
		// FIT: what repeats lasts a whole number of beats (a power of two, 1/4 to 64) at the clock's tempo: the speed
		// changes to fit, like tape. The count is the nearest to its own length, and stays until that is 1.5x away
		// (so a tempo drift does not make it jump).
		if (fit) {
			// What repeats: the slice (ONE), the loop up to where the next pass starts under its crossfade (LOOP),
			// or the loop there and back (PING).
			double part = std::max(p.loopHi - p.loopLo, 1.0), cycle = p.end - p.start;
			if (p.loop == PLAYER_LOOP)
				cycle = std::max(part - std::min(std::max(p.xfade, 1.0), 0.5 * part), 1.0);
			else if (p.loop == PLAYER_PING)
				cycle = 2.0 * part;
			double natural = cycle / srcRate / beatSec;
			double b = c.beats;
			if (b <= 0.0 || natural / b > 1.5 || natural / b < 1.0 / 1.5)
				b = std::min(std::max(std::exp2(std::round(std::log2(std::max(natural, 1e-6)))), 0.25), 64.0);
			c.beats = b;
			p.rate *= natural / b;
		}
		else {
			c.beats = cut ? (double) M.beatLength : 0.0;
		}


		bool note = c.triggerFire || !wasRunning;
		c.triggerFire = false;
		c.triggerRestart = false;
		if (note) {
			if (M.direction == GRAIN_REVERSE)
				c.playerDir = -1;
			else if (M.direction == GRAIN_RANDOM_DIR)
				c.playerDir = random(c) < 0.f ? -1 : 1;
			else
				c.playerDir = 1;
		}
		p.dir = c.playerDir;
		if (note)
			c.player.start(p, wasRunning && c.player.sounding() && c.w > 1e-4f, DECLICK_TIME * sampleRate,
				std::round(in.noteFade * sampleRate), c.w);

		float a, b;
		c.player.process(S, p, &a, &b, c.w);
		*l += a * S.normGain;
		*r += b * S.normGain;

		double at = c.player.position();
		c.shownRead = (float) std::min(std::max((at >= 0.0 ? at : p.start) / (double) S.frames, 0.0), 1.0);
		updateShownSlice(m, in, S);
		// The map shows the slice where it plays (after HIT and the zero-crossing snap)
		c.shownSliceStart = (float) (p.start / (double) S.frames);
		// BEAT: the musical length (the LOOP crossfade reads on past it, not shown)
		double shownEnd = cut ? std::min(p.start + beatFrames, p.end) : p.end;
		c.shownSliceEnd = (float) (shownEnd / (double) S.frames);
		c.scanLevelTarget = 1.f;
	}

	/** A place (frames) moved to its nearest zero crossing, cached per cloud and place `k` */
	double snapped(Cloud& c, int k, double frame, const SampleBuffer& S) {
		long f = (long) std::lround(frame);
		if (f != c.snapIn[k]) {
			c.snapIn[k] = f;
			c.snapOut[k] = nearestZeroCrossing(S, f, (long) (ZERO_SNAP_SECONDS * S.sampleRate));
		}
		return (double) c.snapOut[k];
	}

	/** SAMPLE DRIVE on the cloud's bus, before the filter. The clean path is delayed by the drive's latency, so DRIVE
	fades between two aligned sounds (5 ms) and at 0 the sample is untouched; the drive itself only runs while it is
	turned up (once started, its first DRIVE_LATENCY samples are not heard). */
	void sampleDrive(Cloud& c, const GrainMarkerInput& M, float* l, float* r) {
		float dT = std::min(std::max(M.drive, 0.f), 1.f);
		bool want = dT > 1e-4f;
		c.cleanL[c.cleanPos] = *l;
		c.cleanR[c.cleanPos] = *r;
		c.cleanW[c.cleanPos] = c.w;
		c.cleanPos = (c.cleanPos + 1) % (DRIVE_LATENCY + 1);
		float cl = c.cleanL[c.cleanPos], cr = c.cleanR[c.cleanPos];
		c.wDelayed = c.cleanW[c.cleanPos];
		if (!want && c.driveWet <= 0.f) {
			c.drivePrimed = 0;
			c.driveS = 0.f;
			*l = cl;
			*r = cr;
			return;
		}
		if (c.drivePrimed == 0) {
			c.driveL.reset();
			c.driveR.reset();
			c.driveS = dT;
		}
		c.driveS += (dT - c.driveS) * smoothCoef;
		float dl = c.driveL.process(*l, c.driveS), dr = c.driveR.process(*r, c.driveS);
		bool primed = c.drivePrimed > DRIVE_LATENCY;
		if (!primed)
			c.drivePrimed++;
		float wetT = (want && primed) ? 1.f : 0.f;
		// A silent cloud goes straight to its setting (a step's DRIVE lock), like the filter.
		if (c.w < 1e-4f)
			c.driveWet = wetT;
		else
			c.driveWet = wetT > c.driveWet ? std::min(1.f, c.driveWet + fadeStep) : std::max(0.f, c.driveWet - fadeStep);
		*l = cl + (dl - cl) * c.driveWet;
		*r = cr + (dr - cr) * c.driveWet;
	}

	/** Decides, for this sample, whether a cloud starts a grain. */
	void schedule(int m, const GrainInput& in, const SampleBuffer& S, bool canSync) {
		Cloud& c = clouds[m];
		const GrainMarkerInput& M = in.markers[m];
		bool wasRunning = c.running;
		c.running = true;

		// Playback rate of the cloud (V/OCT, PITCH + FINE, expander V/OCT)
		float key = in.voct + M.voct + M.pitch / 12.f;
		if (key != c.liveKey) {
			c.liveKey = key;
			c.liveMul = std::min(std::exp2(std::min(std::max(key, -6.f), 3.f)), (float) MAX_RATE);
		}

		bool sync = M.sync && canSync;
		double beatSec = in.clock ? in.clock->beatPeriod() : 0.5;
		float scan = M.scan;
		bool scanning = grainScanActive(scan);
		float loopBeats = grainLoopBeats(scan);
		double srcStep = (double) S.sampleRate / sampleRate;
		// POLY: a new note reads from START again (a synced scan stays locked to the beat, unless RESTART).
		bool restart = c.triggerFire && c.triggerRestart;
		bool fromSilence = c.triggerFire && c.triggerFromSilence;
		bool startNote = fromSilence || restart;
		if (c.triggerFire && !(sync && scanning))
			c.scanAccum = 0.0;
		if (restart) {
			// The note's own time: its grid point 0 is now, the scan and the seeded scatter start over.
			c.anchored = true;
			c.anchor = gclock.beatPos;
			c.scanAccum = 0.0;
			c.lastN = 0;
			c.lastGrain = -1;
			c.rng = noteSeed(m);
		}
		c.triggerFire = false;
		c.triggerRestart = false;
		// Beats on the grid this cloud follows: since RESET, or since its note started (RESTART)
		double beat = gclock.beatPos - (c.anchored ? c.anchor : 0.0);

		if (!wasRunning) {
			c.syncState = -1;
			c.gph = 1.0; // a free cloud starts with a grain at once
		}

		if (c.rng == 1u)
			c.rng = 0x2545F491u * (uint32_t) (m + 1);

		// Scan offset into the slice (frames of level 0)
		double sliceLen = sliceFrames(in, m, S);
		updateShownSlice(m, in, S);
		// HIT: the grains start from the nearest hit to START, as SAMPLE's notes do (the knob turns smoothly, the
		// sound jumps from hit to hit); the scan reads on from there for the slice's length.
		double startFrames = (double) M.start * (double) S.frames;
		if (M.hitSnap && in.hits[m]) {
			long h = nearestHit(*in.hits[m], startFrames);
			if (h >= 0) {
				startFrames = (double) h;
				c.shownSliceStart = (float) (startFrames / (double) S.frames);
				if (c.shownSliceEnd >= 0.f)
					c.shownSliceEnd = (float) std::min((startFrames + sliceLen) / (double) S.frames, 1.0);
			}
		}
		double offset = 0.0;
		// Free SCAN: how far the read place moves per output sample (frames of level 0), for a note's first grains
		double scanStep = 0.0;
		if (scanning) {
			if (sync) {
				offset = syncedScanPlace(beat, loopBeats, scan, M.scanPing) * sliceLen;
			}
			else {
				// PING runs one cycle there and back: twice the slice.
				double cycle = std::max(M.scanPing ? 2.0 * sliceLen : sliceLen, 1.0);
				scanStep = grainScanSpeed(scan) * srcStep;
				if (M.scanPing && c.scanAccum > sliceLen)
					scanStep = -scanStep;
				c.scanAccum += grainScanSpeed(scan) * srcStep;
				if (c.scanAccum >= cycle || c.scanAccum < 0.0) {
					c.scanAccum = std::fmod(c.scanAccum, cycle);
					if (c.scanAccum < 0.0)
						c.scanAccum += cycle;
				}
				offset = (M.scanPing && c.scanAccum > sliceLen) ? 2.0 * sliceLen - c.scanAccum : c.scanAccum;
			}
		}
		else {
			c.scanAccum = 0.0;
		}
		c.shownRead = (float) std::min(std::max((startFrames + offset) / (double) S.frames, 0.0), 1.0);
		// FADE: the level follows where the scan is in its loop now (applied to the cloud's sound, smoothed).
		c.scanLevelTarget = 1.f;
		if (scanning && M.scanFade > 0.f && sliceLen > 1.0) {
			float q = (float) std::min(std::max(offset / sliceLen, 0.0), 1.0);
			float f = std::min(M.scanFade, 0.5f);
			auto ease = [](float x) {
				x = std::min(std::max(x, 0.f), 1.f);
				return x * x * (3.f - 2.f * x);
			};
			c.scanLevelTarget = ease(q / f) * ease((1.f - q) / f);
		}
		// RESTART from silence: the level starts where the note does, not where the last one left it.
		if (restart && fromSilence)
			c.scanLevel = c.scanLevelTarget;

		// Grain clock
		double delay = 0.0;
		bool fire = false;
		double grainsPerSecond;
		double sizeSeconds;
		double gridBeat = 0.0;
		// KEEP SPEED: how many grains fire for one, so the shortened grains overlap as much as the set ones
		auto keepFactor = [&](double size) {
			if (!M.keepSpeed)
				return 1;
			double kept = keepLengthSeconds(size, c.liveMul);
			return std::min(std::max((int) std::ceil(size / kept - 1e-6), 1), KEEP_MAX_SUBDIVISIONS);
		};
		// Smear: SIZE sets the grain, and the grain rate rises (free) or the grid subdivides (synced) to keep
		// SMEAR_OVERLAP grains overlapping
		bool smearRate = M.window == GRAIN_SMEAR;
		if (sync) {
			float division = SYNC_DIVISIONS[lfoDivision(M.density, M.triplet, M.dotted)].beats;
			float ratio = grainSizeSyncRatio(M.size);
			sizeSeconds = ratio * division * beatSec;
			int sub = keepFactor(sizeSeconds);
			if (smearRate && ratio * sub < SMEAR_OVERLAP) {
				// From the knob alone (not the tempo), so the grid stays put; the grain is rounded to exactly
				// SMEAR_OVERLAP subdivisions, where Hann grains sum flat
				sub = (int) std::ceil(SMEAR_OVERLAP / ratio - 1e-6);
				sizeSeconds = SMEAR_OVERLAP * division * beatSec / sub;
			}
			float interval = division / (float) sub;
			// The epsilon (about 0.001 samples) keeps a position that lands on a grid point up to rounding
			// from missing it by one sample.
			double x = beat / interval;
			long nn = (long) std::floor(x + 1e-7);
			if (c.syncState != 1 || interval != c.lastInterval) {
				c.syncState = 1;
				c.lastInterval = interval;
				c.lastN = nn; // entering sync or a new division: wait for the next grid point
			}
			if (gclock.snapped)
				c.lastN = nn - 1;
			if (nn > c.lastN) {
				fire = true;
				c.lastN = nn;
				gridBeat = nn * (double) interval;
				delay = gclock.snapped ? 0.0 : std::max(beat - nn * (double) interval, 0.0) / std::max(gclock.advance, 1e-12);
				delay = std::min(std::max(delay, 0.0), 2.0);
			}
			else if (nn < c.lastN) {
				c.lastN = nn;
			}
			grainsPerSecond = 1.0 / (interval * beatSec);
		}
		else {
			if (c.syncState != 0) {
				c.syncState = 0;
				c.lastCycle = LONG_MIN;
			}
			sizeSeconds = grainSizeFreeSeconds(M.size);
			if (smearRate)
				sizeSeconds = std::max(sizeSeconds, SMEAR_MIN_SECONDS);
			double hz = grainFreeHz(M.density) * keepFactor(sizeSeconds);
			if (smearRate)
				hz = std::max(hz, SMEAR_OVERLAP / sizeSeconds);
			c.gph += hz / sampleRate;
			if (c.gph >= 1.0) {
				fire = true;
				delay = std::min((c.gph - 1.0) / (hz / sampleRate), 2.0);
				c.gph -= 1.0;
				if (c.gph >= 1.0)
					c.gph = 0.0;
			}
			grainsPerSecond = hz;
		}
		if (startNote) {
			// Now, not on the next grid point. The grid carries on as usual afterwards.
			fire = true;
			delay = 0.0;
			if (sync)
				gridBeat = beat;
			else
				c.gph = 0.0;
		}
		if (!fire || c.active >= MAX_PER_CLOUD)
			return;
		if (sync && scanning) {
			// A synced scan reads the slice at the grain's own grid time, not at the (rounded) beat position
			// of this sample: a grain on the loop point must always land at the start of the slice.
			offset = syncedScanPlace(gridBeat + 1e-9, loopBeats, scan, M.scanPing) * sliceLen;
		}

		// One grain, or all the ones a running cloud would have sounding when a note starts.
		int grainCount = 1;
		double noteSpacing = 0.0;
		for (int k = 0; k < grainCount && c.active < MAX_PER_CLOUD; k++) {
		// Find a free voice
		Grain* g = nullptr;
		for (int i = 0; i < MAX_GRAINS; i++) {
			if (!grains[i].active) {
				g = &grains[i];
				break;
			}
		}
		if (!g)
			return;

		// Seeded: the random numbers depend only on the grain's place in the loop cycle (its index modulo
		// the grains per cycle), so the pattern repeats every cycle whatever the floating point rounding
		// of the beat position. (Not seeded, or not synced: the sequence just continues.)
		if (sync && in.seeded && k == 0) {
			double cycleBeats = scanning ? loopBeats : IDLE_CYCLE_BEATS;
			long perCycle = std::max(1L, std::lround(cycleBeats / c.lastInterval));
			long j = ((c.lastN % perCycle) + perCycle) % perCycle;
			uint32_t h = (uint32_t) (j + 1) * 0x9E3779B1u ^ (uint32_t) (m + 1) * 0x85EBCA6Bu;
			h ^= h >> 15;
			h *= 0x2C1B3C6Du;
			h ^= h >> 12;
			c.rng = h ? h : 1u;
		}
		// Always draw four numbers, so the seeded sequence stays aligned whatever the knobs are.
		float rSpray = random(c), rPitch = random(c), rPan = random(c), rDir = random(c);
		double startF = startFrames + offset;
		startF += rSpray * grainSpraySeconds(M.spray) * (double) S.sampleRate;
		startF = std::min(std::max(startF, 0.0), (double) S.frames - 1.0);
		bool reverse = M.direction == GRAIN_REVERSE || (M.direction == GRAIN_RANDOM_DIR && rDir < 0.f);
		float scatter = std::exp2(rPitch * grainPitchScatterSemitones(M.pitchScatter) / 12.f);
		float rate = std::min(scatter * c.liveMul, (float) MAX_RATE);
		int level = rate <= 1.f ? 0 : rate <= 2.f ? 1 : rate <= 4.f ? 2 : 3;
		bool smear = smearRate;
		int len = (int) std::lround(std::min(std::max(sizeSeconds * sampleRate, (double) MIN_LENGTH), 4.0 * sampleRate));
		double stepLevel0 = scatter * srcStep; // per output sample, before the live rate
		double scale = 1.0 / (double) (1 << level);
		// Fit the grain inside the sample, so its window (not an end of the sample) shapes it.
		double usable = std::max((double) S.frames - 1.0 - FIT_MARGIN, 1.0);
		double stepNow = stepLevel0 * c.liveMul;
		if (M.keepSpeed) {
			// Short enough to drift little, and centered on where the unpitched grain would be.
			double r = stepNow / srcStep;
			int kept = (int) std::lround(keepLengthSeconds(len / sampleRate, r) * sampleRate);
			len = std::max(std::min(len, kept), MIN_LENGTH);
			startF = std::min(std::max(startF + (1.0 - r) * srcStep * len * 0.5, 0.0), (double) S.frames - 1.0);
		}
		if ((double) len * stepNow > usable)
			len = std::max((int) (usable / stepNow), 1);
		double maxStart = std::max(usable - (double) len * stepNow, 0.0);
		startF = std::min(startF, maxStart);
		double correlation = 0.0;
		if (smear && grainsPerSecond <= SMEAR_ALIGN_MAX_HZ && k == 0)
			startF = matchedStart(c, m, S, startF, maxStart, (double) len * stepNow, reverse, stepLevel0, &correlation);
		g->active = true;
		g->marker = m;
		g->fade = 1.f;
		g->fadeOut = 0.f;
		// RESTART over a sounding note: fade in while the last note's grains fade out (fadeCloudGrains, the same time)
		bool crossfade = restart && !fromSilence;
		g->fadeInLevel = crossfade ? 0.f : 1.f;
		g->fadeIn = crossfade ? 1.f / (DECLICK_TIME * sampleRate) : 0.f;
		g->level = level;
		g->len = len;
		g->age = 0;
		g->window = std::min(std::max(M.window, 0), NUM_GRAIN_WINDOWS - 1);
		double overlapNow = grainsPerSecond * (len / sampleRate);
		// A note starts with the cloud a running one would have (all its overlapping grains at once).
		if (startNote && k == 0 && overlapNow >= 1.0) {
			grainCount = std::min((int) std::ceil(overlapNow - 1e-6), MAX_NOTE_GRAINS);
			noteSpacing = (double) len / grainCount;
			if (grainCount < MAX_NOTE_GRAINS)
				noteSpacing = sampleRate / grainsPerSecond;
		}
		// Sparse grains leave gaps anyway: the first one starts at its window's peak, or a long grain would
		// hide the attack. Overlapping ones start part way through, as they would be in a running cloud.
		int startAge = 0;
		if (startNote && overlapNow < 1.0)
			startAge = (int) (windowPeak[g->window] * len);
		else if (startNote)
			startAge = (int) (k * noteSpacing);
		g->age = std::min(startAge, std::max(len - 1, 0));
		// From silence, Smear keeps every grain of the note on one read position, so they add up instead of cancelling.
		// Over a sounding note (RESTART) they read on by their age, as in a running cloud: started on one place there, they
		// added up to a loud, phasey onset over the old note, heard as a burst of noise on every trig.
		bool readAhead = startNote && overlapNow >= 1.0 && (!smear || crossfade);
		if (startNote && smear && !crossfade && grainCount > 1)
			correlation = 1.0;
		g->inc = (reverse ? -1.0 : 1.0) * stepLevel0 * scale;
		double span = (double) len * stepLevel0 * c.liveMul; // frames of level 0 the grain covers
		double p0 = reverse ? startF + span : startF;
		// A grain the note starts part way through is where a running cloud's would be: it began `age` samples ago,
		// when a free SCAN was that much further back. (A scan at real time then reads exactly where the note's first
		// grain does, so the grains add up as the sample itself; a still cloud is unchanged.)
		if (readAhead)
			p0 = std::max(p0 - scanStep * (double) g->age, 0.0);
		g->pos = p0 * scale + g->inc * c.liveMul * (delay + (readAhead ? (double) g->age : 0.0));
		float pan = std::min(std::max(rPan * std::min(std::max(M.spread, 0.f), 1.f), -1.f), 1.f);
		float angle = (pan + 1.f) * M_PI_4_F;
		g->gl = std::cos(angle) * SQRT2;
		g->gr = std::sin(angle) * SQRT2;
		// Level compensation: grains that overlap add up as uncorrelated noise (power), so the amplitude
		// falls with the overlap and the cloud keeps one level. Sparse grains (less overlap than the
		// reference) are mostly compensated (exponent 0.35 of the 0.5 that would equalize them), up to +7 dB:
		// a few short grains stay lighter than a dense cloud, like pulses, but do not vanish.
		double x = grainsPerSecond * (len / sampleRate) * meanSquare[g->window] / REF_MEAN_SQUARE;
		double comp = (x >= 1.0) ? 1.0 / std::sqrt(x) : std::min(std::pow(std::max(x, 1e-6), -0.35), 2.2);
		// Matched grains add up as amplitude, not power: for Hann, overlap / 2 instead of sqrt(0.375 * overlap).
		// Scaled by how well this one matched, so tonal and noisy sources stay at one level.
		if (correlation > 0.0) {
			double overlap = grainsPerSecond * (len / sampleRate);
			double flat = std::min(1.2247 / std::sqrt(std::max(overlap, 1e-6)), 1.0);
			comp *= 1.0 - (1.0 - flat) * std::min(correlation, 1.0);
		}
		g->amp = S.normGain * (float) comp;
		c.active++;
		c.lastGrain = (int) (g - grains);

		GrainEvent& ev = eventRing[eventsWritten % NUM_EVENTS];
		ev.marker = m;
		ev.pos = (float) (startF / (double) S.frames);
		ev.seconds = (float) (span / (double) S.sampleRate);
		eventsWritten++;
		}
	}

	/** Mono value of a level at a frame (the caller keeps the frame in range) */
	static float monoAt(const std::vector<float>& lv, int channels, long i) {
		return channels == 2 ? 0.5f * (lv[(size_t) (2 * i)] + lv[(size_t) (2 * i + 1)]) : lv[(size_t) i];
	}

	/** Best normalized match of `count` frames of a level against the running grain, over a range of lags. */
	static long bestLag(const std::vector<float>& lv, int channels, long frames, long from, long to, int dir, long count, long lagMin, long lagMax, bool* found, double* correlation) {
		*found = false;
		*correlation = 0.0;
		long best = 0;
		double bestScore = 0.0;
		long fromEnd = from + dir * (count - 1);
		if (std::min(from, fromEnd) < 0 || std::max(from, fromEnd) >= frames)
			return 0;
		double xx = 0.0;
		for (long t = 0; t < count; t++) {
			float x = monoAt(lv, channels, from + dir * t);
			xx += (double) x * x;
		}
		for (long lag = lagMin; lag <= lagMax; lag++) {
			long a = to + lag, b = a + dir * (count - 1);
			if (std::min(a, b) < 0 || std::max(a, b) >= frames)
				continue;
			double xy = 0.0, yy = 0.0;
			for (long t = 0; t < count; t++) {
				float x = monoAt(lv, channels, from + dir * t);
				float y = monoAt(lv, channels, a + dir * t);
				xy += (double) x * y;
				yy += (double) y * y;
			}
			double score = xy / std::sqrt(yy + 1e-9);
			if (!*found || score > bestScore) {
				*found = true;
				bestScore = score;
				best = lag;
			}
		}
		*correlation = bestScore / std::sqrt(xx + 1e-9);
		return best;
	}

	/** Moves a grain's start (by at most SMEAR_SEARCH_SECONDS) to where the source lines up with what the
	cloud's running grain is reading now, so the two add up instead of cancelling while they crossfade.
	Coarse search on the 8x decimated level, then refined on level 0. Only meaningful for grains that
	read in the same direction at the same speed. `correlation` gets how well they match (0 = no match). */
	double matchedStart(const Cloud& c, int m, const SampleBuffer& S, double startF, double maxStart, double span, bool reverse, double stepLevel0, double* correlation) const {
		*correlation = 0.0;
		if (c.lastGrain < 0)
			return startF;
		const Grain& prev = grains[c.lastGrain];
		if (!prev.active || prev.marker != m || (prev.inc < 0.0) != reverse)
			return startF;
		double prevUnit = (double) (1 << prev.level);
		if (std::fabs(std::fabs(prev.inc) * prevUnit - stepLevel0) > 1e-3 * stepLevel0)
			return startF;
		const int COARSE = 3;
		const long COUNT_COARSE = 64, COUNT_FINE = 256;
		int dir = reverse ? -1 : 1;
		double prevPos = prev.pos * prevUnit;
		// A reversed grain reads down from the far end of its span.
		double readStart = reverse ? startF + span : startF;
		long search = (long) (SMEAR_SEARCH_SECONDS * S.sampleRate);
		// The moved grain must still fit: limit the lags to what the placement allows.
		long lagLo = -std::min(search, (long) startF), lagHi = std::min(search, (long) (maxStart - startF));
		bool found = false;
		long scale = 1L << COARSE;
		const std::vector<float>& lc = S.levels[COARSE];
		long lag = scale * bestLag(lc, S.channels, (long) S.levelFrames(COARSE), std::lround(prevPos / scale), std::lround(readStart / scale), dir,
			COUNT_COARSE, lagLo / scale, lagHi / scale, &found, correlation);
		if (!found)
			return startF;
		const std::vector<float>& l0 = S.levels[0];
		long fineLo = std::max(lag - scale, lagLo), fineHi = std::min(lag + scale, lagHi);
		double fineCorrelation = 0.0;
		long fine = bestLag(l0, S.channels, (long) S.frames, std::lround(prevPos), std::lround(readStart), dir, COUNT_FINE, fineLo, fineHi, &found, &fineCorrelation);
		if (!found)
			return startF + (double) lag;
		*correlation = fineCorrelation;
		return startF + (double) fine;
	}

	/** Where a synced scan is in its slice, 0..1. LOOP: 0 to 1 over loopBeats, again and again. PING: 0 to 1 over
	loopBeats, then back to 0 over the next loopBeats. A negative SCAN runs it the other way. */
	static double syncedScanPlace(double beat, double loopBeats, float scan, bool ping) {
		double ph;
		if (ping) {
			ph = beat / (2.0 * loopBeats);
			ph -= std::floor(ph);
			ph = (ph < 0.5) ? 2.0 * ph : 2.0 - 2.0 * ph;
		}
		else {
			ph = beat / loopBeats;
			ph -= std::floor(ph);
		}
		return scan < 0.f ? 1.0 - ph : ph;
	}

	/** For the map: where this cloud's loop is (end -1 while it does not scan). */
	void updateShownSlice(int m, const GrainInput& in, const SampleBuffer& S) {
		Cloud& c = clouds[m];
		float start = std::min(std::max(in.markers[m].start, 0.f), 1.f);
		c.shownSliceStart = start;
		bool loop = playerApplied || grainScanActive(in.markers[m].scan);
		c.shownSliceEnd = loop ? (float) std::min(start + sliceFrames(in, m, S) / (double) S.frames, 1.0) : -1.f;
	}

	/** Frames of level 0 from this marker's START to the next START (in the sample), or to the end. */
	double sliceFrames(const GrainInput& in, int m, const SampleBuffer& S) const {
		float start = std::min(std::max(in.markers[m].start, 0.f), 1.f);
		float end = 1.f;
		// A set LENGTH: from START, cut at the sample's end, whatever the other markers do.
		if (in.markers[m].scanLength > 0.f)
			return std::max((double) (std::min(start + in.markers[m].scanLength, 1.f) - start) * (double) S.frames, 1.0);
		// The next START of a marker on the same sample (another marker's own file says nothing about this one)
		const WavetableBank* mine = bankFor(in, m);
		for (int i = 0; i < NUM_MARKERS; i++) {
			if (i == m || !in.markers[i].active || bankFor(in, i) != mine)
				continue;
			float s = std::min(std::max(in.markers[i].start, 0.f), 1.f);
			if (s > start + 1e-4f && s < end)
				end = s;
		}
		return std::max((double) (end - start) * (double) S.frames, 1.0);
	}

	float windowAt(const Grain& g) const {
		float p = (float) g.age / (float) g.len * WINDOW_POINTS;
		int i = std::min((int) p, WINDOW_POINTS - 1);
		float f = p - i;
		float v = window[g.window][i] + (window[g.window][i + 1] - window[g.window][i]) * f;
		int toEnd = g.len - g.age;
		if (g.age < EDGE) {
			float x = (float) g.age / EDGE;
			v *= x * x * (3.f - 2.f * x);
		}
		else if (toEnd < EDGE) {
			float x = (float) toEnd / EDGE;
			v *= x * x * (3.f - 2.f * x);
		}
		return v;
	}

	/** Cubic (Catmull-Rom) read of interleaved frames; outside the sample reads silence. */
	static void readCubic(const float* d, long frames, int channels, double pos, float* l, float* r) {
		long i0 = (long) std::floor(pos);
		float f = (float) (pos - (double) i0);
		float yl[4], yr[4];
		for (int k = 0; k < 4; k++) {
			long i = i0 - 1 + k;
			if (i < 0 || i >= frames) {
				yl[k] = yr[k] = 0.f;
			}
			else if (channels == 2) {
				yl[k] = d[2 * i];
				yr[k] = d[2 * i + 1];
			}
			else {
				yl[k] = yr[k] = d[i];
			}
		}
		auto cubic = [&](const float* y) {
			float a = -0.5f * y[0] + 1.5f * y[1] - 1.5f * y[2] + 0.5f * y[3];
			float b = y[0] - 2.5f * y[1] + 2.f * y[2] - 0.5f * y[3];
			float c = -0.5f * y[0] + 0.5f * y[2];
			return ((a * f + b) * f + c) * f + y[1];
		};
		*l = cubic(yl);
		*r = (channels == 2) ? cubic(yr) : *l;
	}

	float sampleRate = 44100.f;
	float smoothCoef = 0.f;
	float fadeStep = 0.f;
	float gain = 0.f;
	float posS = 0.f;
	float masterGainS = 1.f;
	bool bankApplied = false;
	const WavetableBank* appliedBank = nullptr;
	/** Each cloud's bank as applied (identity only) and its own fade, 0..1 */
	const WavetableBank* appliedCloudBank[NUM_MARKERS] = {};
	float cloudGate[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};

	/** The bank a cloud plays: its own sample's, else the main one (null: none) */
	static const WavetableBank* bankFor(const GrainInput& in, int m) {
		return in.markerBank[m] ? in.markerBank[m] : in.bank;
	}
	GrainClock gclock;
	Grain grains[MAX_GRAINS];
	Cloud clouds[NUM_MARKERS];
	float window[NUM_GRAIN_WINDOWS][WINDOW_POINTS + 1] = {};
	float meanSquare[NUM_GRAIN_WINDOWS] = {};
	/** Where each window is highest, 0..1 through the grain */
	float windowPeak[NUM_GRAIN_WINDOWS] = {};
	bool polyApplied = false;
	/** SAMPLE engine in use (GrainInput::player, applied while silent like a bank swap) */
	bool playerApplied = false;
	float heard[NUM_MARKERS] = {1.f, 0.f, 0.f, 0.f, 0.f};
	float mainShare[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	float directL[NUM_MARKERS] = {};
	float directR[NUM_MARKERS] = {};
	/** SEND: each cloud's sound where its filter is, this sample */
	float sendL[NUM_MARKERS] = {};
	float sendR[NUM_MARKERS] = {};
	MasterFilter master;
	SafetyLimiter limiter;
	GrainEvent eventRing[NUM_EVENTS];
	uint32_t eventsWritten = 0;
};

} // namespace wa
