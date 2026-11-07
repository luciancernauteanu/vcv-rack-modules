#pragma once
#include <algorithm>
#include <cmath>

#include "MorphEngine.hpp" // NUM_MARKERS

// POLY play mode: every marker is a voice with its own ADSR, started by the POSITION playhead. No Rack
// dependencies (tested in tests/). The module owns one PolyVoices and hands its levels to whichever engine
// runs, so notes carry on through an engine switch.

namespace wa {

/** ATTACK knob 0..1: 1 ms .. 5 s (log). The minimum is also the declick. */
static const float ADSR_ATTACK_MIN = 0.001f;
static const float ADSR_ATTACK_RATIO = 5000.f;
/** DECAY and RELEASE knobs 0..1: 5 ms .. 10 s (log), the time to fall 60 dB. */
static const float ADSR_FALL_MIN = 0.005f;
static const float ADSR_FALL_RATIO = 2000.f;

/** HOLD knob 0..1: how long a note stays on, 10 ms .. 10 s (log). At the very top it stays on until the
playhead starts another marker. */
static const float ADSR_HOLD_MIN = 0.01f;
static const float ADSR_HOLD_RATIO = 1000.f;
static const float ADSR_HOLD_TOP = 0.995f;

inline float adsrHoldSeconds(float k) {
	return k >= ADSR_HOLD_TOP ? -1.f : ADSR_HOLD_MIN * std::pow(ADSR_HOLD_RATIO, std::max(k, 0.f));
}

inline float adsrAttackSeconds(float k) {
	return ADSR_ATTACK_MIN * std::pow(ADSR_ATTACK_RATIO, std::min(std::max(k, 0.f), 1.f));
}
inline float adsrFallSeconds(float k) {
	return ADSR_FALL_MIN * std::pow(ADSR_FALL_RATIO, std::min(std::max(k, 0.f), 1.f));
}

/** Linear attack, exponential decay and release. A new note attacks from the current level, so it cannot click. */
struct Adsr {
	enum Stage {
		IDLE,
		ATTACK,
		DECAY,
		SUSTAIN,
		RELEASE
	};
	Stage stage = IDLE;
	float level = 0.f;

	void reset() {
		stage = IDLE;
		level = 0.f;
	}
	void gateOn() {
		stage = ATTACK;
	}
	void gateOff() {
		if (stage != IDLE)
			stage = RELEASE;
	}

	/** Times in seconds, sustain 0..1. Returns the level 0..1. */
	float process(float attack, float decay, float sustain, float release, float sampleTime) {
		switch (stage) {
			case IDLE:
				break;
			case ATTACK:
				level += sampleTime / attack;
				if (level >= 1.f) {
					level = 1.f;
					stage = DECAY;
				}
				break;
			case DECAY:
				level = sustain + (level - sustain) * fallCoef(decay, sampleTime);
				if (std::fabs(level - sustain) < SETTLED) {
					level = sustain;
					stage = SUSTAIN;
				}
				break;
			case SUSTAIN:
				// Follows the knob smoothly while the note is held.
				level += (sustain - level) * std::min(sampleTime / SUSTAIN_FOLLOW, 1.f);
				break;
			case RELEASE:
				level *= fallCoef(release, sampleTime);
				if (level < SETTLED) {
					level = 0.f;
					stage = IDLE;
				}
				break;
		}
		return level;
	}

private:
	/** Below -80 dB the stage is done. */
	static constexpr float SETTLED = 1e-4f;
	static constexpr float SUSTAIN_FOLLOW = 0.005f;
	/** ln(1000): times are for a 60 dB fall */
	static constexpr float LN_1000 = 6.907755279f;

	/** e^-x for the small x of one sample (x < 0.1 down to 8 kHz at the shortest time), without the exp. */
	static float fallCoef(float seconds, float sampleTime) {
		return std::max(1.f - LN_1000 * sampleTime / seconds, 0.f);
	}
};

/** Decides which markers the playhead starts. A marker fires when the playhead reaches it (within REACH of
the bullet, so markers at the very ends of the bar work too) or jumps over it, and must then be left by REARM
before it can fire again, so a noisy CV cannot machine-gun it. A step that passes several markers at once (a
saw LFO's reset, a square, a fast knob) fires only the one it lands nearest to. Markers at the same place fire
together. The first call after reset() fires the nearest marker, so entering POLY makes a sound at once. */
struct MarkerTrigger {
	/** Fired on this sample */
	bool fired[NUM_MARKERS] = {};

	void reset() {
		needFirst = true;
		for (int m = 0; m < NUM_MARKERS; m++) {
			side[m] = 0;
			armed[m] = false;
			fired[m] = false;
		}
	}

	void process(const float* pos, const bool* active, float p) {
		bool hit[NUM_MARKERS] = {};
		int best = -1;
		float bestDistance = 1e9f;
		for (int m = 0; m < NUM_MARKERS; m++) {
			fired[m] = false;
			if (!active[m]) {
				side[m] = 0;
				continue;
			}
			float d = p - pos[m];
			float distance = std::fabs(d);
			int s = (d >= 0.f) ? 1 : -1;
			if (side[m] == 0) {
				// Just turned on (or just reset): it knows where it is, without firing.
				armed[m] = distance > REARM;
			}
			else if (armed[m] && (distance <= REACH || s != side[m])) {
				hit[m] = true;
				armed[m] = false;
				if (distance < bestDistance) {
					best = m;
					bestDistance = distance;
				}
			}
			else if (distance > REARM) {
				armed[m] = true;
			}
			side[m] = s;
		}
		if (best >= 0) {
			fireGroup(pos, hit, best);
			needFirst = false;
			return;
		}
		if (!needFirst)
			return;
		bool all[NUM_MARKERS];
		for (int m = 0; m < NUM_MARKERS; m++) {
			all[m] = active[m];
			if (active[m] && std::fabs(p - pos[m]) < bestDistance) {
				best = m;
				bestDistance = std::fabs(p - pos[m]);
			}
		}
		if (best >= 0) {
			fireGroup(pos, all, best);
			needFirst = false;
		}
	}

private:
	static constexpr float REACH = 0.005f;
	static constexpr float REARM = 0.01f;
	/** Bullets this close are one place on the bar */
	static constexpr float SAME_PLACE = 1e-4f;

	int side[NUM_MARKERS] = {};
	bool armed[NUM_MARKERS] = {};
	bool needFirst = true;

	void fireGroup(const float* pos, const bool* candidates, int best) {
		for (int m = 0; m < NUM_MARKERS; m++) {
			bool open = candidates[m] && std::fabs(pos[m] - pos[best]) < SAME_PLACE;
			fired[m] = open;
		}
	}
};

/** Playing markers directly, not through the playhead (expander GATE, a press on the marker's slot) */
struct VoiceGates {
	/** An expander's GATE is patched to this marker: only the gate plays it, the playhead does not */
	bool patched[NUM_MARKERS] = {};
	/** On while high. Rising starts a note, falling releases it, whatever HOLD says. */
	bool high[NUM_MARKERS] = {};
};

struct AdsrSettings {
	float attack = 0.005f;
	float decay = 0.2f;
	float sustain = 1.f;
	/** Seconds the note stays on, or negative: until the playhead starts another marker */
	float hold = 1.f;
	float release = 0.3f;
	/** LATCH: a note from the playhead waits for the next multiple of this many beats. 0 = starts at once. */
	float latchBeats = 0.f;
};

/** The five voices: trigger plus one ADSR per marker. Each note from the playhead lasts its own HOLD, whatever the
playhead does next, so notes overlap; the same marker firing again restarts its note. A note from a gate lasts as
long as the gate.

LATCH: the playhead still decides which marker plays, but a marker with latchBeats waits for its next beat. If the
playhead reaches another latched marker meanwhile, the newest replaces the one waiting, so a fast move makes a
melody rather than a chord (markers on one spot still start together). Gates and slot presses play at once. */
struct PolyVoices {
	MarkerTrigger trigger;
	Adsr adsr[NUM_MARKERS];
	/** Level of each voice 0..1 */
	float env[NUM_MARKERS] = {};
	/** The voice started a new note on this sample */
	bool start[NUM_MARKERS] = {};
	/** The note is on (before its release) */
	bool gate[NUM_MARKERS] = {};

	void reset() {
		trigger.reset();
		for (int m = 0; m < NUM_MARKERS; m++) {
			adsr[m].reset();
			env[m] = 0.f;
			start[m] = false;
			gate[m] = false;
			holdLeft[m] = 0.f;
			directPrev[m] = false;
			waiting[m] = false;
		}
		beatReady = false;
	}

	/** A note is waiting for its beat (LATCH) */
	bool isWaiting(int m) const {
		return waiting[m];
	}

	/** `beat`: the clock's position in beats, for LATCH */
	void process(const float* pos, const bool* active, float position, const AdsrSettings* settings, float sampleTime, const VoiceGates* direct = nullptr, double beat = 0.0) {
		bool followsPlayhead[NUM_MARKERS];
		for (int m = 0; m < NUM_MARKERS; m++)
			followsPlayhead[m] = active[m] && !(direct && direct->patched[m]);
		trigger.process(pos, followsPlayhead, position);
		bool fired[NUM_MARKERS];
		bool newWaiting = false;
		for (int m = 0; m < NUM_MARKERS; m++) {
			fired[m] = trigger.fired[m] && settings[m].latchBeats <= 0.f;
			newWaiting = newWaiting || (trigger.fired[m] && settings[m].latchBeats > 0.f);
		}
		bool anyFired = false;
		for (int m = 0; m < NUM_MARKERS; m++) {
			if (newWaiting)
				waiting[m] = trigger.fired[m] && settings[m].latchBeats > 0.f;
			if (waiting[m] && !followsPlayhead[m])
				waiting[m] = false;
			if (waiting[m] && (settings[m].latchBeats <= 0.f || !beatReady || crossedBeat(beat, settings[m].latchBeats))) {
				fired[m] = true;
				waiting[m] = false;
			}
			anyFired = anyFired || fired[m];
		}
		lastBeat = beat;
		beatReady = true;
		for (int m = 0; m < NUM_MARKERS; m++) {
			const AdsrSettings& s = settings[m];
			bool on = direct && direct->high[m] && active[m];
			start[m] = fired[m] || (on && !directPrev[m]);
			if (start[m]) {
				gate[m] = true;
				holdLeft[m] = s.hold;
				adsr[m].gateOn();
			}
			else if (on) {
				gate[m] = true;
			}
			else if (directPrev[m]) {
				gate[m] = false;
			}
			else if (gate[m]) {
				if (!active[m]) {
					gate[m] = false;
				}
				else if (s.hold < 0.f) {
					gate[m] = !anyFired;
				}
				else {
					holdLeft[m] -= sampleTime;
					gate[m] = holdLeft[m] > 0.f;
				}
			}
			directPrev[m] = on;
			if (!gate[m])
				adsr[m].gateOff();
			env[m] = adsr[m].process(s.attack, s.decay, s.sustain, s.release, sampleTime);
		}
	}

private:
	float holdLeft[NUM_MARKERS] = {};
	bool directPrev[NUM_MARKERS] = {};
	bool waiting[NUM_MARKERS] = {};
	double lastBeat = 0.0;
	bool beatReady = false;

	/** The tiny offset is the one the grain grid uses: a position landing on a grid point up to rounding counts. */
	bool crossedBeat(double beat, float interval) const {
		return std::floor(beat / interval + 1e-7) != std::floor(lastBeat / interval + 1e-7);
	}
};

} // namespace wa
