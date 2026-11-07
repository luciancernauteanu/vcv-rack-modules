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
// - Random scatter (SPRAY, pitch, pan, direction) restarts from the same seed at the start of every loop
//   cycle while synced, so the random pattern repeats like a loop instead of drifting (SEEDED).
// - Pitch belongs to each cloud: V/OCT (0 V = original speed at C4's default), PITCH, FINE, scatter.
//
// POLY play mode (GrainInput::poly): a cloud's weight is its marker's ADSR level instead of its position share
// (see PolyVoices.hpp). A note from silence starts the cloud as if it had been running (all its overlapping
// grains at once, each part way through), so a short attack is heard at once and the level does not dip.
//
// Click prevention: every grain has a >= 1.3 ms smooth edge and is placed so it fits inside the sample (a
// live pitch change that still reaches an end fades out there); clouds fade in and out with their weight
// (5 ms); a bank swap or STOP fades everything out first (5 ms) and clears the grains while silent.
//
// The Smear window turns a cloud into a continuous sound: grains last at least SMEAR_OVERLAP grain intervals
// (SIZE becomes a minimum), each one starts where it best matches the grain it fades in over, and the cloud's
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
	const WavetableBank* bank = nullptr;
	/** Needed for SYNC; without it every cloud runs free */
	const ClockTracker* clock = nullptr;
	/** RESET happened this sample */
	bool reset = false;
	/** Random scatter repeats every loop cycle while synced */
	bool seeded = true;
	bool holdSilent = false;
	bool wantDirect[NUM_MARKERS] = {};
	/** POLY play mode: each cloud follows voiceEnv instead of the position crossfade */
	bool poly = false;
	/** Level of each marker's ADSR, 0..1 */
	float voiceEnv[NUM_MARKERS] = {};
	/** The marker started a new note on this sample */
	bool voiceStart[NUM_MARKERS] = {};

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
	/** Length in seconds */
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

	float getPosition() const {
		return posS;
	}

	/** The clock position the grain grids follow, in beats */
	double getBeatPos() const {
		return gclock.beatPos;
	}

	/** Share of the position crossfade of a cloud (0..1, sums to 1) */
	float getHeard(int m) const {
		return heard[m];
	}

	/** Where the cloud reads now: START plus scan offset, 0..1 of the sample */
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

	/** Grain window RMS^2, used for level compensation (tests) */
	float getWindowMeanSquare(int shape) const {
		return meanSquare[std::min(std::max(shape, 0), NUM_GRAIN_WINDOWS - 1)];
	}

	float getLimiterGain() const {
		return limiter.getGain();
	}

	void getDirect(int m, float* l, float* r) const {
		*l = directL[m];
		*r = directR[m];
	}

	void process(const GrainInput& in, float* outL, float* outR) {
		*outL = *outR = 0.f;
		const SampleBuffer* S = (in.bank && in.bank->sample.valid()) ? &in.bank->sample : nullptr;

		// Bank swap: fade out, clear while silent, apply.
		bool changed = !bankApplied || in.bank != appliedBank || in.poly != polyApplied;
		bool wantSilent = in.holdSilent || changed || !S;
		gain = wantSilent ? std::max(0.f, gain - fadeStep) : std::min(1.f, gain + fadeStep);
		if (changed && gain <= 0.f) {
			appliedBank = in.bank; // identity only: it may already be freed, never read it
			bankApplied = true;
			polyApplied = in.poly;
			clearGrains();
			for (int m = 0; m < NUM_MARKERS; m++)
				resetCloud(m);
		}
		if (in.reset) {
			gclock.reset();
			for (int m = 0; m < NUM_MARKERS; m++)
				clouds[m].lastCycle = LONG_MIN;
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
		if (S && gain > 0.f) {
			for (int m = 0; m < NUM_MARKERS; m++) {
				Cloud& c = clouds[m];
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
					}
					c.w = tw[m]; // the ADSR is already smooth, and a smoother would hide a short attack
				}
				else {
					c.w += (tw[m] - c.w) * smoothCoef;
				}
				float gT = std::min(std::max(in.markers[m].gain, 0.f), (float) MAX_GAIN);
				c.gainS += (gT - c.gainS) * smoothCoef;
				bool wanted = (tw[m] > 0.f || c.w > 1e-3f) && gT > 1e-4f;
				if (!wanted) {
					c.running = false;
					c.triggerFire = false;
					// Not scheduled (the playhead is elsewhere): the map still shows its bullet at START.
					c.shownRead = std::min(std::max(in.markers[m].start, 0.f), 1.f);
					continue;
				}
				schedule(m, in, *S, canSync);
			}
		}
		else {
			for (int m = 0; m < NUM_MARKERS; m++) {
				clouds[m].running = false;
				clouds[m].shownRead = std::min(std::max(in.markers[m].start, 0.f), 1.f);
			}
		}

		// Render all grains into the clouds' buses
		if (S) {
			for (int g = 0; g < MAX_GRAINS; g++) {
				Grain& gr = grains[g];
				if (!gr.active)
					continue;
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
				bufL[gr.marker] += l * k * gr.gl;
				bufR[gr.marker] += r * k * gr.gr;
				gr.pos += gr.inc * c.liveMul;
				gr.age++;
				if (gr.age >= gr.len || gr.pos < -4.0 || gr.pos > frames + 4.0) {
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
			// A silent cloud goes straight to its place, so a new note does not begin with a slide. This runs
			// even for a cloud idle long enough to skip the block below.
			float panT = std::min(std::max(M.pan, -1.f), 1.f);
			if (c.w < 1e-4f)
				c.panS = panT;
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
				modeWeights(true, M.filterMode, mixT);
				for (int k = 0; k < NUM_MIX; k++)
					c.mixS[k] += (mixT[k] - c.mixS[k]) * smoothCoef;
				c.cutS += (std::min(std::max(M.cutoff, 0.f), 1.f) - c.cutS) * smoothCoef;
				c.resS += (std::min(std::max(M.res, 0.f), 1.f) - c.resS) * smoothCoef;
				float fc = filterHz(c.cutS), q = filterQ(c.resS);
				float xl = bufL[m], xr = bufR[m];
				float wetT = (M.window == GRAIN_SMEAR) ? 1.f : 0.f;
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
				yl = c.svfL.process(xl, fc, q, sampleRate, c.mixS);
				yr = c.svfR.process(xr, fc, q, sampleRate, c.mixS);
				if (!std::isfinite(yl) || !std::isfinite(yr)) {
					c.svfL.reset();
					c.svfR.reset();
					yl = yr = 0.f;
				}
				yl *= c.gainS * c.w;
				yr *= c.gainS * c.w;
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
	static const int WINDOW_POINTS = 1024;
	static const int EDGE = 64;
	static const int MIN_LENGTH = 128;
	/** Most grains started at once when a note starts */
	static const int MAX_NOTE_GRAINS = 16;
	/** Frames (of the grain's level) over which a grain fades when it reaches an end of the sample */
	static constexpr float END_FADE = 32.f;
	/** Frames of level 0 kept free at the end when placing a grain (covers the sub-sample start delay) */
	static constexpr double FIT_MARGIN = 32.0;
	/** Smear: grains overlapping at any time. Hann grains sum flat at whole numbers. */
	static constexpr double SMEAR_OVERLAP = 3.0;
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
	};

	struct Cloud {
		bool running = false;
		bool directRunning = false;
		int active = 0;
		int idle = IDLE_RESET + 1;
		float w = 0.f;
		float gainS = 1.f;
		float panS = 0.f;
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
		/** POLY: a note started; schedule() takes it on this cloud's next turn */
		bool triggerFire = false;
		bool triggerFromSilence = false;
		/** The grain started last (it may have ended since) */
		int lastGrain = -1;
		float smearWet = 0.f;
		GrainDiffuser diffL, diffR;
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

	void resetCloud(int m) {
		Cloud& c = clouds[m];
		c.running = false;
		c.syncState = -1;
		c.gph = 1.0;
		c.scanAccum = 0.0;
		c.lastCycle = LONG_MIN;
		c.lastGrain = -1;
		c.triggerFire = false;
		c.idle = IDLE_RESET + 1;
		c.w = 0.f;
		c.svfL.reset();
		c.svfR.reset();
		c.smearWet = 0.f;
		c.diffL.clear();
		c.diffR.clear();
	}

	float random(Cloud& c) {
		uint32_t x = c.rng;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		c.rng = x;
		return (float) (x >> 8) * (1.f / 8388608.f) - 1.f;
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
		// POLY: a new note reads from START again (a synced scan stays locked to the beat).
		bool startNote = c.triggerFire && c.triggerFromSilence;
		if (c.triggerFire && !(sync && scanning))
			c.scanAccum = 0.0;
		c.triggerFire = false;

		if (!wasRunning) {
			c.syncState = -1;
			c.gph = 1.0; // a free cloud starts with a grain at once
		}

		if (c.rng == 1u)
			c.rng = 0x2545F491u * (uint32_t) (m + 1);

		// Scan offset into the slice (frames of level 0)
		double sliceLen = sliceFrames(in, m, S);
		double offset = 0.0;
		if (scanning) {
			if (sync) {
				double ph = gclock.beatPos / loopBeats;
				ph -= std::floor(ph);
				offset = (scan < 0.f ? 1.0 - ph : ph) * sliceLen;
			}
			else {
				c.scanAccum += grainScanSpeed(scan) * srcStep;
				if (c.scanAccum >= sliceLen || c.scanAccum < 0.0) {
					c.scanAccum = std::fmod(c.scanAccum, std::max(sliceLen, 1.0));
					if (c.scanAccum < 0.0)
						c.scanAccum += sliceLen;
				}
				offset = c.scanAccum;
			}
		}
		else {
			c.scanAccum = 0.0;
		}
		c.shownRead = (float) std::min(std::max((M.start * (double) S.frames + offset) / (double) S.frames, 0.0), 1.0);

		// Grain clock
		double delay = 0.0;
		bool fire = false;
		double grainsPerSecond;
		double sizeSeconds;
		double gridBeat = 0.0;
		if (sync) {
			float interval = SYNC_DIVISIONS[lfoDivision(M.density, M.triplet, M.dotted)].beats;
			// The epsilon (about 0.001 samples) keeps a position that lands on a grid point up to rounding
			// from missing it by one sample.
			double x = gclock.beatPos / interval;
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
				delay = gclock.snapped ? 0.0 : std::max(gclock.beatPos - nn * (double) interval, 0.0) / std::max(gclock.advance, 1e-12);
				delay = std::min(std::max(delay, 0.0), 2.0);
			}
			else if (nn < c.lastN) {
				c.lastN = nn;
			}
			grainsPerSecond = 1.0 / (interval * beatSec);
			sizeSeconds = grainSizeSyncRatio(M.size) * interval * beatSec;
		}
		else {
			if (c.syncState != 0) {
				c.syncState = 0;
				c.lastCycle = LONG_MIN;
			}
			double hz = grainFreeHz(M.density);
			c.gph += hz / sampleRate;
			if (c.gph >= 1.0) {
				fire = true;
				delay = std::min((c.gph - 1.0) / (hz / sampleRate), 2.0);
				c.gph -= 1.0;
				if (c.gph >= 1.0)
					c.gph = 0.0;
			}
			grainsPerSecond = hz;
			sizeSeconds = grainSizeFreeSeconds(M.size);
		}
		if (startNote) {
			// Now, not on the next grid point. The grid carries on as usual afterwards.
			fire = true;
			delay = 0.0;
			if (sync)
				gridBeat = gclock.beatPos;
			else
				c.gph = 0.0;
		}
		if (!fire || c.active >= MAX_PER_CLOUD)
			return;
		if (sync && scanning) {
			// A synced scan reads the slice at the grain's own grid time, not at the (rounded) beat position
			// of this sample: a grain on the loop point must always land at the start of the slice.
			double ph = (gridBeat + 1e-9) / loopBeats;
			ph -= std::floor(ph);
			offset = (scan < 0.f ? 1.0 - ph : ph) * sliceLen;
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
			long perCycle = std::max(1L, std::lround(cycleBeats / SYNC_DIVISIONS[lfoDivision(M.density, M.triplet, M.dotted)].beats));
			long j = ((c.lastN % perCycle) + perCycle) % perCycle;
			uint32_t h = (uint32_t) (j + 1) * 0x9E3779B1u ^ (uint32_t) (m + 1) * 0x85EBCA6Bu;
			h ^= h >> 15;
			h *= 0x2C1B3C6Du;
			h ^= h >> 12;
			c.rng = h ? h : 1u;
		}
		// Always draw four numbers, so the seeded sequence stays aligned whatever the knobs are.
		float rSpray = random(c), rPitch = random(c), rPan = random(c), rDir = random(c);
		double startF = (double) M.start * (double) S.frames + offset;
		startF += rSpray * grainSpraySeconds(M.spray) * (double) S.sampleRate;
		startF = std::min(std::max(startF, 0.0), (double) S.frames - 1.0);
		bool reverse = M.direction == GRAIN_REVERSE || (M.direction == GRAIN_RANDOM_DIR && rDir < 0.f);
		float scatter = std::exp2(rPitch * grainPitchScatterSemitones(M.pitchScatter) / 12.f);
		float rate = std::min(scatter * c.liveMul, (float) MAX_RATE);
		int level = rate <= 1.f ? 0 : rate <= 2.f ? 1 : rate <= 4.f ? 2 : 3;
		bool smear = M.window == GRAIN_SMEAR;
		if (smear)
			sizeSeconds = std::max(sizeSeconds, SMEAR_OVERLAP / grainsPerSecond);
		int len = (int) std::lround(std::min(std::max(sizeSeconds * sampleRate, (double) MIN_LENGTH), 4.0 * sampleRate));
		double stepLevel0 = scatter * srcStep; // per output sample, before the live rate
		double scale = 1.0 / (double) (1 << level);
		// Fit the grain inside the sample, so its window (not an end of the sample) shapes it.
		double usable = std::max((double) S.frames - 1.0 - FIT_MARGIN, 1.0);
		double stepNow = stepLevel0 * c.liveMul;
		if ((double) len * stepNow > usable)
			len = std::max((int) (usable / stepNow), 1);
		double maxStart = std::max(usable - (double) len * stepNow, 0.0);
		startF = std::min(startF, maxStart);
		double correlation = 0.0;
		if (smear && grainsPerSecond <= SMEAR_ALIGN_MAX_HZ && k == 0)
			startF = matchedStart(c, m, S, startF, maxStart, (double) len * stepNow, reverse, stepLevel0, &correlation);
		g->active = true;
		g->marker = m;
		g->level = level;
		g->len = len;
		g->age = 0;
		g->window = std::min(std::max(M.window, 0), NUM_GRAIN_WINDOWS - 1);
		double overlapNow = grainsPerSecond * (len / sampleRate);
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
		// Smear keeps every grain of the note on one read position, so they add up instead of cancelling.
		bool readAhead = startNote && overlapNow >= 1.0 && !smear;
		if (startNote && smear && grainCount > 1)
			correlation = 1.0;
		g->inc = (reverse ? -1.0 : 1.0) * stepLevel0 * scale;
		double span = (double) len * stepLevel0 * c.liveMul; // frames of level 0 the grain covers
		double p0 = reverse ? startF + span : startF;
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
		ev.seconds = (float) len / sampleRate;
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

	/** Frames of level 0 from this marker's START to the next START (in the sample), or to the end. */
	double sliceFrames(const GrainInput& in, int m, const SampleBuffer& S) const {
		float start = std::min(std::max(in.markers[m].start, 0.f), 1.f);
		float end = 1.f;
		for (int i = 0; i < NUM_MARKERS; i++) {
			if (i == m || !in.markers[i].active)
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
	GrainClock gclock;
	Grain grains[MAX_GRAINS];
	Cloud clouds[NUM_MARKERS];
	float window[NUM_GRAIN_WINDOWS][WINDOW_POINTS + 1] = {};
	float meanSquare[NUM_GRAIN_WINDOWS] = {};
	/** Where each window is highest, 0..1 through the grain */
	float windowPeak[NUM_GRAIN_WINDOWS] = {};
	bool polyApplied = false;
	float heard[NUM_MARKERS] = {1.f, 0.f, 0.f, 0.f, 0.f};
	float mainShare[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	float directL[NUM_MARKERS] = {};
	float directR[NUM_MARKERS] = {};
	MasterFilter master;
	SafetyLimiter limiter;
	GrainEvent eventRing[NUM_EVENTS];
	uint32_t eventsWritten = 0;
};

} // namespace wa
