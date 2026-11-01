#pragma once
#include "Lfo.hpp"
#include "MorphEngine.hpp"

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

struct ToExpanderMessage {
	/** The main module's clock, so expander LFOs can sync to CLOCK */
	ClockTracker clock;
	/** Chain position of the sender: 0 = main module, k = k-th expander */
	int index = 0;
	/** The sender is connected to a main module (possibly through other expanders) */
	bool linked = false;
	/** RESET arrived this sample: restart the LFOs */
	bool reset = false;
};

struct ToMainMessage {
	/** Volts x AMT, summed over every linked expander assigned to the marker (GAIN excluded) */
	float mod[NUM_MARKERS][NUM_MOD_TARGETS] = {};
	/** Product of every linked expander's VCA level and GATE (0 while a patched gate is low) for the marker (1 = untouched) */
	float gainMul[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	/** Volts on the expanders' V/OCT inputs, summed per artifact (added to the main V/OCT) */
	float voct[NUM_MARKERS] = {};
	/** A linked expander is actively modulating this artifact (a CV with a non-zero amount, or V/OCT) */
	bool modulated[NUM_MARKERS] = {};
};

} // namespace wa
