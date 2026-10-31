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

/** What 10 V at AMT 100% does to each target, in the marker param's own units:
WAVE whole sample, GAIN +100%, CUTOFF its full 10-octave range (so ~1 V/oct), DRIVE full range,
FINE 2 semitones, PAN hard left to hard right. */
static const float MOD_SCALE_PER_10V[NUM_MOD_TARGETS] = {1.f, 1.f, 1.f, 1.f, 2.f, 2.f};

struct ToExpanderMessage {
	/** The main module's clock, so expander LFOs can sync to CLOCK */
	ClockTracker clock;
	/** Chain position of the sender: 0 = main module, k = k-th expander */
	int index = 0;
	/** The sender is connected to a main module (possibly through other expanders) */
	bool linked = false;
};

struct ToMainMessage {
	/** Volts x AMT, summed over every linked expander assigned to the marker */
	float mod[NUM_MARKERS][NUM_MOD_TARGETS] = {};
};

} // namespace wa
