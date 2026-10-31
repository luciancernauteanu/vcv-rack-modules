#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// The LFO (six shapes) used by the markers, the position and the expander, and the clock tracker it syncs to. No Rack dependencies (tested in tests/).

namespace wa {

enum DivisionKind {
	STRAIGHT,
	DOTTED,
	TRIPLET
};

struct SyncDivision {
	const char* name;
	/** Length of one LFO cycle in quarter notes */
	float beats;
	DivisionKind kind;
};

/** Note values from slowest to fastest; D = dotted (x1.5), T = triplet (x2/3). */
static const SyncDivision SYNC_DIVISIONS[] = {
	{"4/1D", 24.f, DOTTED}, {"4/1", 16.f, STRAIGHT}, {"2/1D", 12.f, DOTTED}, {"4/1T", 32.f / 3.f, TRIPLET},
	{"2/1", 8.f, STRAIGHT}, {"1/1D", 6.f, DOTTED}, {"2/1T", 16.f / 3.f, TRIPLET}, {"1/1", 4.f, STRAIGHT},
	{"1/2D", 3.f, DOTTED}, {"1/1T", 8.f / 3.f, TRIPLET}, {"1/2", 2.f, STRAIGHT}, {"1/4D", 1.5f, DOTTED},
	{"1/2T", 4.f / 3.f, TRIPLET}, {"1/4", 1.f, STRAIGHT}, {"1/8D", 0.75f, DOTTED}, {"1/4T", 2.f / 3.f, TRIPLET},
	{"1/8", 0.5f, STRAIGHT}, {"1/16D", 0.375f, DOTTED}, {"1/8T", 1.f / 3.f, TRIPLET}, {"1/16", 0.25f, STRAIGHT},
	{"1/32D", 0.1875f, DOTTED}, {"1/16T", 1.f / 6.f, TRIPLET}, {"1/32", 0.125f, STRAIGHT}, {"1/32T", 1.f / 12.f, TRIPLET},
};
static const int NUM_SYNC_DIVISIONS = sizeof(SYNC_DIVISIONS) / sizeof(SYNC_DIVISIONS[0]);

/** Which note values the sync rate knob steps through: straight always, plus the enabled kinds. */
inline bool divisionEnabled(int i, bool triplets, bool dotted) {
	switch (SYNC_DIVISIONS[i].kind) {
		case DOTTED: return dotted;
		case TRIPLET: return triplets;
		default: return true;
	}
}

inline int lfoDivisionCount(bool triplets, bool dotted) {
	int n = 0;
	for (int i = 0; i < NUM_SYNC_DIVISIONS; i++)
		n += divisionEnabled(i, triplets, dotted);
	return n;
}

/** Free rate knob 0..1 -> 0.01..20 Hz on a log scale. */
inline float lfoFreeHz(float knob) {
	return 0.01f * std::exp(knob * 7.600902460f); // ln(2000)
}

/** Sync rate knob 0..1 -> index into SYNC_DIVISIONS, stepping only through enabled note values
(slow -> fast). Enabling triplets or dotted adds steps, so the same knob position can land on a
neighboring value. */
inline int lfoDivision(float knob, bool triplets, bool dotted) {
	int n = lfoDivisionCount(triplets, dotted);
	int k = (int) std::lround(knob * (n - 1));
	k = std::min(std::max(k, 0), n - 1);
	for (int i = 0; i < NUM_SYNC_DIVISIONS; i++) {
		if (divisionEnabled(i, triplets, dotted) && k-- == 0)
			return i;
	}
	return NUM_SYNC_DIVISIONS - 1;
}

/** Folds x into 0..1 by reflection, so modulation past an end bounces back instead of sticking. */
inline float fold01(float x) {
	x = std::fmod(std::fabs(x), 2.f);
	return (x > 1.f) ? 2.f - x : x;
}

/** Centered sweep for a 0..1 target such as the playhead: the LFO moves it by +-amount/2, so 100%
sweeps the whole range. The center (`base`) is held far enough from the ends that the whole swing
fits, so the LFO's shape is never cut off, rectified or folded (folding turns a symmetric LFO at
the ends into a stuck or double-speed signal). The result is always in 0..1. */
inline float lfoCentered(float base, float amount, float lfo) {
	float h = 0.5f * std::min(std::max(amount, 0.f), 1.f);
	float center = std::min(std::max(base, h), 1.f - h);
	return center + h * lfo;
}

/** Clock resolutions offered for the CLOCK input, in pulses per quarter note. */
static const int NUM_PPQN = 7;
static const int PPQN_VALUES[NUM_PPQN] = {1, 2, 4, 8, 12, 24, 48};

/** Measures the tempo from clock pulses and marks the beats (quarter notes).
`ppqn` pulses make one quarter note; the tempo is measured from every pulse (so it follows tempo
changes within one pulse), and a beat edge is reported on every `ppqn`-th pulse. */
class ClockTracker {
public:
	/** Call once per sample. */
	void process(float voltage, bool connected, float sampleRate, int ppqn = 1) {
		edge = false;
		this->sampleRate = sampleRate;
		ppqn = std::max(ppqn, 1);
		if (!connected) {
			running = false;
			havePrev = false;
			return;
		}
		// Schmitt trigger at 0.1 V / 1 V, per the VCV voltage standards.
		bool pulse = false;
		if (high) {
			if (voltage <= 0.1f)
				high = false;
		}
		else if (voltage >= 1.f) {
			high = true;
			pulse = true;
		}
		samplesSince++;
		if (pulse) {
			if (havePrev)
				periodSamples = samplesSince * ppqn;
			havePrev = true;
			samplesSince = 0;
			pulseCount++;
			if (pulseCount % ppqn == 0) {
				edge = true;
				beatCount++;
			}
		}
		// A clock that stops for 4 pulses counts as stopped; the LFO then free-runs at the last tempo.
		running = periodSamples > 0 && samplesSince * ppqn < 4 * periodSamples;
	}

	/** RESET: the next pulse is beat 0 (the start of a bar), as on the sequencer. The tempo is kept. */
	void reset() {
		pulseCount = -1;
		beatCount = -1;
	}

	/** Seconds per quarter note: measured, or 120 BPM until two pulses have arrived. */
	double beatPeriod() const {
		return (periodSamples > 0) ? (double) periodSamples / sampleRate : 0.5;
	}
	bool isRunning() const {
		return running;
	}
	/** True on the sample of a rising edge. */
	bool isEdge() const {
		return edge;
	}
	/** Index of the latest beat (0 = first beat since the cable was connected or since RESET). */
	long getBeatCount() const {
		return beatCount;
	}

private:
	float sampleRate = 44100.f;
	bool high = false;
	bool edge = false;
	bool havePrev = false;
	bool running = false;
	long samplesSince = 0;
	long periodSamples = 0;
	long beatCount = -1;
	long pulseCount = -1;
};

enum LfoShape {
	LFO_SINE,
	LFO_TRIANGLE,
	LFO_SAW,
	/** Falling ramp: starts at +1 and drops to -1 */
	LFO_SAW_REVERSE,
	LFO_SQUARE,
	/** A new random target each cycle, reached along a smooth S-curve */
	LFO_SMOOTH_RANDOM,
	/** A random walk: each cycle's target is a bounded step from the last one */
	LFO_DRUNK,
	NUM_LFO_SHAPES
};

static const char* const LFO_SHAPE_NAMES[NUM_LFO_SHAPES] = {"Sine", "Triangle", "Saw", "Reverse saw", "Square", "Smooth random", "Drunk"};

inline bool lfoShapeIsRandom(int shape) {
	return shape == LFO_SMOOTH_RANDOM || shape == LFO_DRUNK;
}

/** The periodic shapes at `phase` (0..1), -1..1. All start at 0 except the saws and square, which start
at their cycle's beginning (-1, +1 and +1). Random shapes are not periodic: returns 0. */
inline float lfoShapeValue(int shape, double phase) {
	switch (shape) {
		case LFO_SINE:
			return (float) std::sin(2.0 * 3.14159265358979323846 * phase);
		case LFO_TRIANGLE:
			return (float) (phase < 0.25 ? 4.0 * phase : phase < 0.75 ? 2.0 - 4.0 * phase : 4.0 * phase - 4.0);
		case LFO_SAW:
			return (float) (2.0 * phase - 1.0);
		case LFO_SAW_REVERSE:
			return (float) (1.0 - 2.0 * phase);
		case LFO_SQUARE:
			return phase < 0.5 ? 1.f : -1.f;
		default:
			return 0.f;
	}
}

/** LFO: sine, triangle, saw, square, smooth random or drunk walk; free in Hz or synced to the clock
by note value. The random shapes pick a new target every cycle, so their rate is the rate of change
(and when synced they step on the clock grid). */
class Lfo {
public:
	/** Different seeds give different random sequences (see seed()). */
	void seed(uint32_t s) {
		rng = s ? s : 0x9E3779B9u;
	}

	/** Sine. */
	float process(bool sync, float rateKnob, bool triplets, bool dotted, const ClockTracker& clock, float sampleTime) {
		return process(LFO_SINE, sync, rateKnob, triplets, dotted, clock, sampleTime);
	}

	/** Returns -1..1. */
	float process(int shape, bool sync, float rateKnob, bool triplets, bool dotted, const ClockTracker& clock, float sampleTime) {
		if (shape != lastShape) {
			// Random shapes start from wherever the output was, so switching does not jump.
			lastShape = shape;
			if (lfoShapeIsRandom(shape)) {
				from = lastOut;
				to = nextTarget(shape);
				walk = std::min(std::max(lastOut, -1.f), 1.f);
			}
		}
		double freq;
		if (sync) {
			double beats = SYNC_DIVISIONS[lfoDivision(rateKnob, triplets, dotted)].beats;
			freq = 1.0 / (beats * clock.beatPeriod());
			// Lock the phase to the clock on every pulse: beat k of an n-beat cycle is phase k/n.
			// The error is not snapped (that would jump WAVE) but pulled in over ~20 ms, so the
			// output stays continuous. Once locked, the error at each pulse is tiny.
			if (clock.isEdge() && clock.isRunning()) {
				double p = clock.getBeatCount() / beats;
				double err = (p - std::floor(p)) - phase;
				err -= std::floor(err + 0.5); // shortest way round: -0.5..0.5
				correction = err;
			}
		}
		else {
			freq = lfoFreeHz(rateKnob);
			correction = 0.0;
		}
		float out;
		if (lfoShapeIsRandom(shape)) {
			float t = (float) phase;
			t = t * t * (3.f - 2.f * t); // smoothstep between the two targets
			out = from + (to - from) * t;
		}
		else {
			out = lfoShapeValue(shape, phase);
		}
		double pull = correction * std::min(1.0, sampleTime / CORRECTION_TIME);
		correction -= pull;
		double before = phase;
		phase += freq * sampleTime + pull;
		phase -= std::floor(phase);
		// A forward wrap (not a small backward correction) starts the next random segment.
		if (phase < before - 0.5 && lfoShapeIsRandom(shape)) {
			from = to;
			to = nextTarget(shape);
		}
		lastOut = out;
		return out;
	}

	/** RESET: restart the cycle at phase 0 (random shapes start a new segment from where they are). */
	void reset() {
		phase = 0.0;
		correction = 0.0;
		if (lfoShapeIsRandom(lastShape)) {
			from = lastOut;
			to = nextTarget(lastShape);
		}
	}

	double getPhase() const {
		return phase;
	}

private:
	static constexpr double CORRECTION_TIME = 0.02;
	/** Largest step of the drunk walk per cycle (of the full -1..1 range) */
	static constexpr float DRUNK_STEP = 0.35f;

	/** -1..1 from an xorshift32 generator */
	float random() {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		return (float) (rng >> 8) * (1.f / 8388608.f) - 1.f;
	}

	float nextTarget(int shape) {
		if (shape == LFO_DRUNK) {
			walk += random() * DRUNK_STEP;
			// Reflect at the ends so it wanders without sticking to a wall.
			if (walk > 1.f)
				walk = 2.f - walk;
			if (walk < -1.f)
				walk = -2.f - walk;
			walk = std::min(std::max(walk, -1.f), 1.f);
			return walk;
		}
		return random();
	}

	double phase = 0.0;
	/** Remaining phase error still to pull in (cycles) */
	double correction = 0.0;
	uint32_t rng = 0x9E3779B9u;
	int lastShape = LFO_SINE;
	float lastOut = 0.f;
	float from = 0.f;
	float to = 0.f;
	float walk = 0.f;
};

} // namespace wa
