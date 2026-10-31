#pragma once
#include <algorithm>
#include <cmath>

// Per-marker WAVE LFO and the clock tracker it syncs to. No Rack dependencies (tested in tests/).

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

/** Measures the clock period from rising edges (1 pulse = 1 quarter note). */
class ClockTracker {
public:
	/** Call once per sample. */
	void process(float voltage, bool connected, float sampleRate) {
		edge = false;
		this->sampleRate = sampleRate;
		if (!connected) {
			running = false;
			havePrev = false;
			return;
		}
		// Schmitt trigger at 0.1 V / 1 V, per the VCV voltage standards.
		if (high) {
			if (voltage <= 0.1f)
				high = false;
		}
		else if (voltage >= 1.f) {
			high = true;
			edge = true;
		}
		samplesSince++;
		if (edge) {
			if (havePrev)
				periodSamples = samplesSince;
			havePrev = true;
			samplesSince = 0;
			beatCount++;
		}
		// A clock that stops for 4 periods counts as stopped; the LFO then free-runs at the last tempo.
		running = periodSamples > 0 && samplesSince < 4 * periodSamples;
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
	/** Index of the latest pulse (0 = first pulse since the cable was connected). */
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
};

/** Sine LFO, free in Hz or synced to the clock by note value. */
class Lfo {
public:
	/** Returns -1..1. */
	float process(bool sync, float rateKnob, bool triplets, bool dotted, const ClockTracker& clock, float sampleTime) {
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
		float out = (float) std::sin(2.0 * 3.14159265358979323846 * phase);
		double pull = correction * std::min(1.0, sampleTime / CORRECTION_TIME);
		correction -= pull;
		phase += freq * sampleTime + pull;
		phase -= std::floor(phase);
		return out;
	}

	double getPhase() const {
		return phase;
	}

private:
	static constexpr double CORRECTION_TIME = 0.02;
	double phase = 0.0;
	/** Remaining phase error still to pull in (cycles) */
	double correction = 0.0;
};

} // namespace wa
