#pragma once
#include <algorithm>
#include <cmath>

// Each marker's modulation, set on its expander (Elektron style): an LFO and an AD envelope, each sent to one of the
// marker's own settings with a depth. They live in the main module as marker settings, so a sequencer step can lock
// them like CUTOFF: leave DEPTH at 0 and lock it on one trig, and only that trig is modulated. No Rack dependency
// (tested in tests/test_lfo.cpp).

namespace wa {

/** Where an LFO or AD goes: one of the marker's own settings */
enum ModDest {
	MOD_DEST_WAVE,
	MOD_DEST_GAIN,
	MOD_DEST_CUTOFF,
	MOD_DEST_RES,
	MOD_DEST_DRIVE,
	MOD_DEST_PITCH,
	MOD_DEST_PAN,
	NUM_MOD_DESTS
};

static const char* const MOD_DEST_NAMES[NUM_MOD_DESTS] = {"WAVE / START", "GAIN", "CUTOFF", "RES", "DRIVE / P.SPRD", "PITCH", "PAN"};
/** On the expander's DEST button (WAVE reads START, DRIVE reads P.SPRD on the granular engine) */
static const char* const MOD_DEST_SHORT[NUM_MOD_DESTS] = {"WAVE", "GAIN", "CUTOFF", "RES", "DRIVE", "PITCH", "PAN"};
static const char* const MOD_DEST_SHORT_GRAIN[NUM_MOD_DESTS] = {"START", "GAIN", "CUTOFF", "RES", "P.SPRD", "PITCH", "PAN"};
static const char* const MOD_DEST_SHORT_SAMPLE[NUM_MOD_DESTS] = {"START", "GAIN", "CUTOFF", "RES", "DRIVE", "PITCH", "PAN"};

/** How far each destination moves at DEPTH 100% and a full source (LFO at +-1, AD at 1), in its own units: the whole
range of WAVE, CUTOFF (20 Hz .. 20 kHz), RES and DRIVE, PAN from one side to the other, PITCH 2 octaves. GAIN adds
to the GAIN knob's level (1 = 0 dB): at the default 0 dB, 0 .. x2 (+6 dB); with the knob at silent, up to 0 dB. */
static const float MOD_DEST_RANGE[NUM_MOD_DESTS] = {1.f, 1.f, 1.f, 1.f, 1.f, 24.f, 2.f};

/** What the LFO and the AD add to each destination this sample (amount = depth x source, -1..1 each) */
struct ModSum {
	float add[NUM_MOD_DESTS] = {};

	void put(int dest, float amount) {
		if (dest >= 0 && dest < NUM_MOD_DESTS)
			add[dest] += amount * MOD_DEST_RANGE[dest];
	}
	/** GAIN: the level with the GAIN knob at `knob` (a level, 0 .. GAIN_LEVEL_MAX). Added, not multiplied, so the
	LFO / AD is heard even with the knob all the way down. */
	static constexpr float GAIN_LEVEL_MAX = 4.f;
	float gainLevel(float knob) const {
		return std::min(std::max(knob + add[MOD_DEST_GAIN], 0.f), GAIN_LEVEL_MAX);
	}
};

/** AD stage times: MOD_AD_MIN_TIME * (max / MOD_AD_MIN_TIME)^knob (attack up to 2 s, decay up to 4 s). At the
minimum a stage is instant. */
static constexpr float MOD_AD_MIN_TIME = 0.005f;
static constexpr float MOD_AD_ATTACK_MAX = 2.f;
static constexpr float MOD_AD_DECAY_MAX = 4.f;

inline float modAdTime(float knob, float maxTime) {
	return MOD_AD_MIN_TIME * std::pow(maxTime / MOD_AD_MIN_TIME, std::min(std::max(knob, 0.f), 1.f));
}

/** The AD: a linear attack to 1, then an exponential decay (60 dB in the DECAY time). A note (a sequencer trig, a
POLY note) runs attack then decay. A GATE start in gate mode (PERC off) holds at 1 while the gate stays high and
decays when it falls; in PERC mode a gate edge runs attack then decay like a note. */
class ModEnvelope {
public:
	/** One sample: `trigger` starts it (from the GATE jack when `fromGate`), `gateHigh` is the GATE now. 0..1. */
	float process(bool trigger, bool fromGate, bool gateHigh, bool perc, float attackKnob, float decayKnob, float sampleTime) {
		const float instant = MOD_AD_MIN_TIME * 1.05f;
		float attack = modAdTime(attackKnob, MOD_AD_ATTACK_MAX), decay = modAdTime(decayKnob, MOD_AD_DECAY_MAX);
		if (trigger) {
			stage = ATTACK;
			holdByGate = fromGate && !perc;
		}
		if (stage == ATTACK) {
			if (holdByGate && !gateHigh) {
				stage = DECAY;
			}
			else {
				env = (attack <= instant) ? 1.f : std::min(1.f, env + sampleTime / attack);
				if (env >= 1.f) {
					env = 1.f;
					// The peak is heard for this sample; the decay starts on the next.
					stage = holdByGate ? HOLD : DECAY;
					return env;
				}
			}
		}
		else if (stage == HOLD && !gateHigh) {
			stage = DECAY;
		}
		if (stage == DECAY) {
			if (decay <= instant) {
				env = 0.f;
				stage = IDLE;
			}
			else {
				env *= std::exp(-6.9077553f * sampleTime / decay);
				if (env < 1e-3f) {
					env = 0.f;
					stage = IDLE;
				}
			}
		}
		return env;
	}

	float level() const {
		return env;
	}

	void reset() {
		stage = IDLE;
		env = 0.f;
		holdByGate = false;
	}

private:
	enum Stage { IDLE, ATTACK, HOLD, DECAY };
	Stage stage = IDLE;
	float env = 0.f;
	bool holdByGate = false;
};

} // namespace wa
