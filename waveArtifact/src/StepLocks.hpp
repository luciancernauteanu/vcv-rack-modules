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

static const int NUM_LOCKABLE = 53;

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
	"sampleLoop", "sampleLoopPoint", "sampleBeat", "sampleHitSnap",
	"modLfoRate", "modLfoShape", "modLfoSync", "modLfoTriplet", "modLfoDotted", "modLfoDest", "modLfoDepth",
	"modAdAttack", "modAdDecay", "modAdPerc", "modAdDest", "modAdDepth",
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
	"ONE / LOOP / PING", "LOOP POINT", "BEAT", "HIT",
	"EXP LFO RATE", "EXP LFO SHAPE", "EXP LFO SYNC", "EXP LFO T", "EXP LFO D", "EXP LFO DEST", "EXP LFO DEPTH",
	"EXP AD ATTACK", "EXP AD DECAY", "EXP AD PERC", "EXP AD DEST", "EXP AD DEPTH",
};

/** Settings a step cannot lock: switches that only change how another control is read (SYNC / T / D, PITCH FREE)
or the scan's mode (KEEP, LOOP / PING). While a step is selected they still act on the marker itself. The lockable
switches and lists are the filter type, the grain window and direction, and the LFO shape. */
static const uint64_t NOT_LOCKABLE =
	(1ull << 4) | (1ull << 5) | (1ull << 6) |    // LFO SYNC, T, D
	(1ull << 14) |                               // PITCH FREE
	(1ull << 21) | (1ull << 22) | (1ull << 23) | // GRAIN SYNC, T, D
	(1ull << 33) | (1ull << 34) |                // KEEP, LOOP / PING
	(1ull << 37) | (1ull << 39) | (1ull << 40) | // SAMPLE ONE / LOOP / PING, BEAT, HIT
	(1ull << 43) | (1ull << 44) | (1ull << 45) | // EXP LFO SYNC, T, D
	(1ull << 50);                                // EXP AD PERC

inline bool isLockable(int k) {
	return k >= 0 && k < NUM_LOCKABLE && !((NOT_LOCKABLE >> k) & 1u);
}

enum TrigCondition {
	COND_ALWAYS,
	/** Plays with `chance` percent probability */
	COND_CHANCE,
	/** A:B: plays on pass A of every B passes of the row (pass 1 = the first after RESET) */
	COND_RATIO,
	NUM_CONDITIONS
};

/** The chances offered in the menu, in percent */
static const int CHANCES[5] = {10, 25, 50, 75, 90};

/** A condition: a trig's (whether it plays) or one lock's (whether that lock applies when its trig plays) */
struct Condition {
	uint8_t cond = COND_ALWAYS;
	uint8_t chance = 50;
	uint8_t a = 1, b = 2;
};

/** Whether a condition passes this time. `pass` counts the row's passes since RESET (0 = the first), `random01` is a
uniform number in [0, 1) drawn for it. */
inline bool conditionPasses(const Condition& c, long pass, float random01) {
	switch (c.cond) {
		case COND_CHANCE: return random01 * 100.f < (float) c.chance;
		case COND_RATIO: {
			int b = c.b < 1 ? 1 : c.b;
			int a = c.a < 1 ? 1 : (c.a > b ? b : c.a);
			return (pass % b) + 1 == a;
		}
		default: return true;
	}
}

/** Short text for a condition: "50%", "1:2" (empty for ALWAYS) */
inline void conditionText(const Condition& c, char* out, int size) {
	switch (c.cond) {
		case COND_CHANCE: std::snprintf(out, size, "%d%%", (int) c.chance); break;
		case COND_RATIO: std::snprintf(out, size, "%d:%d", (int) c.a, (int) c.b); break;
		default: if (size > 0) out[0] = 0; break;
	}
}

/** Everything a step carries besides its velocity */
struct StepData {
	/** The locked values, in each setting's own param units. Only the ones whose bit is set in `mask` count. */
	float value[NUM_LOCKABLE] = {};
	uint64_t mask = 0;
	uint8_t cond = COND_ALWAYS;
	uint8_t chance = 50;
	uint8_t a = 1, b = 2;
	/** Slide 0..1: 0 = off; otherwise the settings this step changes glide there over that fraction of one of the
	row's steps (1 = the whole step, 0.01 = almost a jump). */
	float slide = 0.f;
	/** Tied to the trig before it (a long gate): the note that trig started goes on through this step instead of
	starting again; only counts while the step before is lit (see isTied()). */
	bool tie = false;
	/** Each lock's own condition: when the trig plays, a lock whose condition fails this time does not apply (that
	setting plays the knob). Only the locks whose bit is set in `condMask` have one (always a subset of `mask`). */
	Condition lockCond[NUM_LOCKABLE];
	uint64_t condMask = 0;

	/** The trig's own condition */
	Condition trigCondition() const {
		Condition c;
		c.cond = cond;
		c.chance = chance;
		c.a = a;
		c.b = b;
		return c;
	}
	void setTrigCondition(const Condition& c) {
		cond = c.cond;
		chance = c.chance;
		a = c.a;
		b = c.b;
	}
	/** Lock k's condition (Always when it has none) */
	Condition lockCondition(int k) const {
		return ((condMask >> k) & 1u) ? lockCond[k] : Condition();
	}
	/** Gives lock k a condition (Always removes it). Nothing when k is not locked. */
	void setLockCondition(int k, const Condition& c) {
		if (!locked(k))
			return;
		uint64_t bit = (uint64_t) 1u << k;
		lockCond[k] = c;
		if (c.cond == COND_ALWAYS)
			condMask &= ~bit;
		else
			condMask |= bit;
	}

	bool locked(int k) const {
		return (mask >> k) & 1u;
	}
	void setLock(int k, float v) {
		if (!isLockable(k))
			return;
		value[k] = v;
		mask |= (uint64_t) 1u << k;
	}
	/** Removes lock k, with its condition */
	void clearLock(int k) {
		mask &= ~((uint64_t) 1u << k);
		condMask &= ~((uint64_t) 1u << k);
	}
	/** Removes every lock, with their conditions */
	void clearLocks() {
		mask = 0;
		condMask = 0;
	}
	bool isPlain() const {
		return mask == 0 && cond == COND_ALWAYS && slide <= 0.f && !tie;
	}
	void clear() {
		*this = StepData();
	}
};

/** What a tied step plays: the locks of the group so far with its own on top (a step without locks keeps the
group's), and its own slide. */
inline StepData mergeTied(const StepData& group, const StepData& step) {
	StepData out = group;
	for (int k = 0; k < NUM_LOCKABLE; k++) {
		if (step.locked(k)) {
			out.value[k] = step.value[k];
			out.mask |= (uint64_t) 1u << k;
		}
	}
	out.slide = step.slide;
	out.tie = true;
	return out;
}

/** Whether a step plays this time (its trig condition). `pass` counts the row's passes since RESET (0 = the first),
`random01` is a uniform number in [0, 1) drawn for this step. */
inline bool conditionPasses(const StepData& s, long pass, float random01) {
	return conditionPasses(s.trigCondition(), pass, random01);
}

/** A trig plays: the locks of `src` whose own condition fails this time come off `out` (those settings play the knob).
`rnd()` gives a uniform number in [0, 1), drawn once per lock with a chance. */
template <class Rnd>
inline void applyLockConditions(StepData& out, const StepData& src, long pass, Rnd rnd) {
	uint64_t m = src.mask & src.condMask;
	while (m) {
		int k = __builtin_ctzll(m);
		m &= m - 1;
		const Condition& c = src.lockCond[k];
		float u = (c.cond == COND_CHANCE) ? rnd() : 0.f;
		if (!conditionPasses(c, pass, u))
			out.mask &= ~((uint64_t) 1u << k);
	}
}

/** One value that jumps to each new target, or glides there with slide (over slide x one step, like the locks).
Used for a row's MOD output (its steps' velocities). */
struct GlideValue {
	float value = 0.f;
	float from = 0.f, to = 0.f;
	double progress = 1.0, glideBeats = 1.0;

	void set(float target, float slide, double stepBeats) {
		slide = slide > 1.f ? 1.f : slide;
		if (slide > 0.f && stepBeats > 0.0 && target != value) {
			from = value;
			to = target;
			progress = 0.0;
			glideBeats = stepBeats * slide;
		}
		else {
			value = to = target;
			progress = 1.0;
		}
	}
	/** Moves on by `beats` (the clock's beats since the last call) */
	void advance(double beats) {
		if (progress >= 1.0)
			return;
		progress += beats / glideBeats;
		if (progress >= 1.0) {
			progress = 1.0;
			value = to;
		}
		else {
			value = from + (to - from) * (float) progress;
		}
	}
};

/** Short text for a condition, drawn in the step, in the lane and in the menu: "50%", "1:2" (empty for ALWAYS) */
inline void conditionText(const StepData& s, char* out, int size) {
	conditionText(s.trigCondition(), out, size);
}

/** The locks one marker is playing, from the step that played last until its row plays another one. With slide, the
settings the new step changes glide there over SLIDE x one of the row's steps instead of jumping. Audio thread. */
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
	void apply(const float* newValue, uint64_t newMask, float slide, double stepBeats, const float* knobs, uint64_t jumpMask) {
		uint64_t glide = 0;
		float start[NUM_LOCKABLE];
		slide = slide > 1.f ? 1.f : slide;
		if (slide > 0.f && stepBeats > 0.0) {
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
		// SLIDE 100% glides over the whole step, 1% over a hundredth of it.
		glideBeats = (stepBeats > 0.0 ? stepBeats : 1.0) * (slide > 0.f ? slide : 1.f);
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
