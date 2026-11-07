#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

// Parameter locks, trig conditions and slide for the step sequencer (Elektron style). No Rack dependency, so it is
// tested offline (tests/test_seq.cpp).
//
// A lock is a marker setting's own value on one step. Setting k is WaveArtifact::MARKER_SETTING_PARAMS[k] (the
// copy / paste list): the order of LOCK_NAMES must match it, and only ever grows at the end.

namespace wa {

static const int NUM_LOCKABLE = 37;

/** Saved in the patch (JSON keys): never rename one. Same order as WaveArtifact::MARKER_SETTING_PARAMS. */
static const char* const LOCK_NAMES[NUM_LOCKABLE] = {
	"wave", "smooth",
	"lfoRate", "lfoAmt", "lfoSync", "lfoTriplet", "lfoDotted", "lfoShape",
	"gain", "cutoff", "res", "filterMode", "drive",
	"pitch", "pitchFree", "fine", "pan",
	"grainDensity", "grainSize", "grainSpray", "grainWindow",
	"grainSync", "grainTriplet", "grainDotted",
	"grainSpread", "grainPScatter", "grainScan", "grainDir",
	"attack", "decay", "sustain", "hold", "release",
	"grainKeepSpeed", "grainScanMode", "grainScanFade", "grainScanLength",
};

/** Shown in the lane's setting menu and on the panel */
static const char* const LOCK_LABELS[NUM_LOCKABLE] = {
	"WAVE / START", "SMOOTH",
	"LFO RATE", "LFO AMT", "LFO SYNC", "LFO T", "LFO D", "LFO SHAPE",
	"GAIN", "CUTOFF", "RES", "FILTER TYPE", "DRIVE",
	"PITCH", "PITCH FREE", "FINE", "PAN",
	"DENSITY", "SIZE", "SPRAY", "WINDOW",
	"GRAIN SYNC", "GRAIN T", "GRAIN D",
	"SPREAD", "P.SPRD", "SCAN", "DIRECTION",
	"ATTACK", "DECAY", "SUSTAIN", "HOLD", "RELEASE",
	"KEEP", "LOOP / PING", "FADE", "LENGTH",
};

/** Settings a step cannot lock: switches that only change how another control is read (SYNC / T / D, PITCH FREE)
or the scan's mode (KEEP, LOOP / PING). While a step is selected they still act on the marker itself. The lockable
switches and lists are the filter type, the grain window and direction, and the LFO shape. */
static const uint64_t NOT_LOCKABLE =
	(1ull << 4) | (1ull << 5) | (1ull << 6) |    // LFO SYNC, T, D
	(1ull << 14) |                               // PITCH FREE
	(1ull << 21) | (1ull << 22) | (1ull << 23) | // GRAIN SYNC, T, D
	(1ull << 33) | (1ull << 34);                 // KEEP, LOOP / PING

inline bool isLockable(int k) {
	return k >= 0 && k < NUM_LOCKABLE && !((NOT_LOCKABLE >> k) & 1u);
}

enum TrigCondition {
	COND_ALWAYS,
	/** Plays with `chance` percent probability */
	COND_CHANCE,
	/** A:B: plays on pass A of every B passes of the row (pass 1 = the first after RESET) */
	COND_RATIO,
	COND_FILL,
	COND_NOT_FILL,
	NUM_CONDITIONS
};

/** The chances offered in the menu, in percent */
static const int CHANCES[5] = {10, 25, 50, 75, 90};

/** Everything a step carries besides its velocity */
struct StepData {
	/** The locked values, in each setting's own param units. Only the ones whose bit is set in `mask` count. */
	float value[NUM_LOCKABLE] = {};
	uint64_t mask = 0;
	uint8_t cond = COND_ALWAYS;
	uint8_t chance = 50;
	uint8_t a = 1, b = 2;
	bool slide = false;

	bool locked(int k) const {
		return (mask >> k) & 1u;
	}
	void setLock(int k, float v) {
		if (!isLockable(k))
			return;
		value[k] = v;
		mask |= (uint64_t) 1u << k;
	}
	void clearLock(int k) {
		mask &= ~((uint64_t) 1u << k);
	}
	bool isPlain() const {
		return mask == 0 && cond == COND_ALWAYS && !slide;
	}
	void clear() {
		*this = StepData();
	}
};

/** Whether a step plays this time. `pass` counts the row's passes since RESET (0 = the first), `random01` is a
uniform number in [0, 1) drawn for this step. */
inline bool conditionPasses(const StepData& s, long pass, bool fill, float random01) {
	switch (s.cond) {
		case COND_CHANCE: return random01 * 100.f < (float) s.chance;
		case COND_RATIO: {
			int b = s.b < 1 ? 1 : s.b;
			int a = s.a < 1 ? 1 : (s.a > b ? b : s.a);
			return (pass % b) + 1 == a;
		}
		case COND_FILL: return fill;
		case COND_NOT_FILL: return !fill;
		default: return true;
	}
}

/** Short text for a condition, drawn in the step and in the menu: "50%", "1:2", "FILL", "!FILL" (empty for ALWAYS) */
inline void conditionText(const StepData& s, char* out, int size) {
	switch (s.cond) {
		case COND_CHANCE: std::snprintf(out, size, "%d%%", (int) s.chance); break;
		case COND_RATIO: std::snprintf(out, size, "%d:%d", (int) s.a, (int) s.b); break;
		case COND_FILL: std::snprintf(out, size, "FILL"); break;
		case COND_NOT_FILL: std::snprintf(out, size, "!FILL"); break;
		default: if (size > 0) out[0] = 0; break;
	}
}

/** The locks one marker is playing, from the step that played last until its row plays another one. With slide, the
settings the new step changes glide there over one of the row's steps instead of jumping. Audio thread. */
struct MarkerLocks {
	float value[NUM_LOCKABLE] = {};
	uint64_t mask = 0;
	/** Settings still gliding, each from `from` toward its target (the lock, or the knob where unlocked) */
	uint64_t gliding = 0;
	float from[NUM_LOCKABLE] = {};
	double progress = 1.0;
	double glideBeats = 1.0;

	/** The value setting k plays now, given the knob's own value */
	float get(int k, float knob) const {
		uint64_t bit = (uint64_t) 1u << k;
		if (!((mask | gliding) & bit))
			return knob;
		float to = (mask & bit) ? value[k] : knob;
		if ((gliding & bit) && progress < 1.0)
			return from[k] + (to - from[k]) * (float) progress;
		return to;
	}

	/** A new step plays. `knobs` are the knob values now (to start glides from what is heard). `jumpMask`: settings
	that never glide (switches and lists). */
	void apply(const float* newValue, uint64_t newMask, bool slide, double stepBeats, const float* knobs, uint64_t jumpMask) {
		uint64_t glide = 0;
		float start[NUM_LOCKABLE];
		if (slide && stepBeats > 0.0) {
			for (int k = 0; k < NUM_LOCKABLE; k++) {
				uint64_t bit = (uint64_t) 1u << k;
				if (!((mask | newMask | gliding) & bit) || (jumpMask & bit))
					continue;
				float now = get(k, knobs[k]);
				float to = (newMask & bit) ? newValue[k] : knobs[k];
				if (now != to) {
					start[k] = now;
					glide |= bit;
				}
			}
		}
		std::memcpy(value, newValue, sizeof(value));
		mask = newMask;
		gliding = glide;
		for (int k = 0; k < NUM_LOCKABLE; k++)
			if ((glide >> k) & 1u)
				from[k] = start[k];
		progress = glide ? 0.0 : 1.0;
		glideBeats = stepBeats > 0.0 ? stepBeats : 1.0;
	}

	/** Moves the glides on by `beats` (the clock's beats per sample) */
	void advance(double beats) {
		if (!gliding)
			return;
		progress += beats / glideBeats;
		if (progress >= 1.0) {
			progress = 1.0;
			gliding = 0;
		}
	}

	void clear() {
		mask = 0;
		gliding = 0;
		progress = 1.0;
	}
};

} // namespace wa
