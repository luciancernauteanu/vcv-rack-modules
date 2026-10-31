// Offline tests for the WAVE LFO and clock tracker (no Rack needed).

#include "Lfo.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

static int failures = 0;

#define CHECK(cond, ...) \
	do { \
		if (!(cond)) { \
			failures++; \
			std::printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
			std::printf(__VA_ARGS__); \
			std::printf("\n"); \
		} \
	} while (0)

static const float SR = 48000.f;

/** Rising-edge count of the LFO (cycles) over `seconds`, with an optional clock at `bpm` (0 = unpatched). */
static int countCycles(bool sync, float knob, float bpm, float seconds, float settle = 0.f) {
	wa::ClockTracker clock;
	wa::Lfo lfo;
	int period = bpm > 0 ? (int) (SR * 60.f / bpm) : 0;
	float prev = 0.f;
	int cycles = 0;
	int total = (int) ((settle + seconds) * SR);
	for (int n = 0; n < total; n++) {
		float v = (period > 0 && n % period < (int) (0.001f * SR)) ? 10.f : 0.f;
		clock.process(v, period > 0, SR);
		float out = lfo.process(sync, knob, clock, 1.f / SR);
		if (n >= (int) (settle * SR) && prev < 0.f && out >= 0.f)
			cycles++;
		prev = out;
	}
	return cycles;
}

static float knobFor(const char* name) {
	for (int i = 0; i < wa::NUM_SYNC_DIVISIONS; i++) {
		if (std::strcmp(wa::SYNC_DIVISIONS[i].name, name) == 0)
			return (float) i / (wa::NUM_SYNC_DIVISIONS - 1);
	}
	return -1.f;
}

static void testFree() {
	std::printf("free rate\n");
	CHECK(std::fabs(wa::lfoFreeHz(0.f) - 0.01f) < 1e-5f, "knob 0 -> 0.01 Hz");
	CHECK(std::fabs(wa::lfoFreeHz(1.f) - 20.f) < 1e-3f, "knob 1 -> 20 Hz");
	// 2 Hz: knob = ln(200) / ln(2000)
	float knob = std::log(200.f) / std::log(2000.f);
	int c = countCycles(false, knob, 0.f, 5.f);
	std::printf("  2 Hz for 5 s: %d cycles\n", c);
	CHECK(std::abs(c - 10) <= 1, "expected 10 cycles, got %d", c);
}

static void testSync() {
	std::printf("clock sync at 120 BPM (1 pulse per quarter = 2 Hz clock)\n");
	struct Case {
		const char* div;
		float expectedHz;
	} cases[] = {
		{"1/4", 2.f},          // one cycle per beat
		{"1/8", 4.f},
		{"1/1", 0.5f},
		{"1/4D", 2.f / 1.5f},  // dotted: 1.5 beats
		{"1/8T", 6.f},         // triplet eighths: 3 per beat
		{"1/16", 8.f},
	};
	for (const Case& c : cases) {
		float knob = knobFor(c.div);
		int cycles = countCycles(true, knob, 120.f, 6.f, 1.f);
		float hz = cycles / 6.f;
		std::printf("  %-5s expected %.3f Hz, got %.3f Hz\n", c.div, c.expectedHz, hz);
		CHECK(std::fabs(hz - c.expectedHz) <= 0.2f, "%s: expected %f Hz, got %f", c.div, c.expectedHz, hz);
	}
	// Tempo follows the clock: 1/4 at 150 BPM = 2.5 Hz.
	int c150 = countCycles(true, knobFor("1/4"), 150.f, 6.f, 1.f);
	std::printf("  1/4 at 150 BPM: %.3f Hz (expected 2.5)\n", c150 / 6.f);
	CHECK(std::fabs(c150 / 6.f - 2.5f) <= 0.2f, "tempo change not followed");
	// No clock: 120 BPM fallback.
	int none = countCycles(true, knobFor("1/4"), 0.f, 6.f);
	std::printf("  1/4 with no clock: %.3f Hz (120 BPM fallback = 2)\n", none / 6.f);
	CHECK(std::fabs(none / 6.f - 2.f) <= 0.2f, "fallback tempo wrong");
}

static void testPhaseLock() {
	std::printf("phase lock: a 1/4 LFO is at phase 0 on every clock pulse\n");
	wa::ClockTracker clock;
	wa::Lfo lfo;
	int period = (int) (SR * 0.5f);
	float knob = knobFor("1/4");
	double worst = 0.0;
	for (int n = 0; n < (int) (4.f * SR); n++) {
		float v = (n % period < (int) (0.001f * SR)) ? 10.f : 0.f;
		clock.process(v, true, SR);
		lfo.process(true, knob, clock, 1.f / SR);
		// After the 3rd pulse, the phase right after each pulse must be ~0 (one sample in).
		if (clock.isEdge() && clock.getBeatCount() >= 2) {
			double p = lfo.getPhase();
			double err = std::min(p, 1.0 - p);
			worst = std::max(worst, err);
		}
	}
	std::printf("  worst phase error at pulses: %.5f cycles\n", worst);
	CHECK(worst < 0.001, "LFO drifts from the clock: %f", worst);
}

static void testFold() {
	std::printf("fold keeps WAVE in 0..1 and continuous\n");
	CHECK(std::fabs(wa::fold01(0.3f) - 0.3f) < 1e-6f, "inside range unchanged");
	CHECK(std::fabs(wa::fold01(1.2f) - 0.8f) < 1e-6f, "1.2 folds to 0.8");
	CHECK(std::fabs(wa::fold01(-0.25f) - 0.25f) < 1e-6f, "-0.25 folds to 0.25");
	float prev = wa::fold01(-2.f), maxStep = 0.f;
	for (float x = -2.f; x <= 3.f; x += 0.001f) {
		float y = wa::fold01(x);
		maxStep = std::max(maxStep, std::fabs(y - prev));
		prev = y;
		CHECK(y >= 0.f && y <= 1.f, "fold out of range at %f", x);
	}
	CHECK(maxStep < 0.0011f, "fold is discontinuous: step %f", maxStep);
}

static void testContinuity() {
	std::printf("LFO output never jumps: first lock, note value change, sync on/off\n");
	wa::ClockTracker clock;
	wa::Lfo lfo;
	int period = (int) (SR * 0.5f);
	float prev = 0.f, maxStep = 0.f;
	bool sync = true;
	float knob = knobFor("1/4");
	for (int n = 0; n < (int) (8.f * SR); n++) {
		if (n == (int) (2.3f * SR)) knob = knobFor("1/8T");   // division change mid-cycle
		if (n == (int) (4.1f * SR)) sync = false;              // to free 0.2 Hz
		if (n == (int) (5.2f * SR)) sync = true;               // back to sync: re-lock
		if (n == (int) (6.7f * SR)) knob = knobFor("1/1D");
		// Clock starts late (first lock happens with a large phase error).
		float v = (n > (int) (0.77f * SR) && n % period < (int) (0.001f * SR)) ? 10.f : 0.f;
		clock.process(v, true, SR);
		float out = lfo.process(sync, sync ? knob : 0.394f, clock, 1.f / SR);
		if (n > 0)
			maxStep = std::max(maxStep, std::fabs(out - prev));
		prev = out;
	}
	// The fastest rate here is 1/8T = 6 Hz; a sine's max step per sample is 2*pi*f/sr.
	// Pulling in a phase error adds a little speed for ~20 ms, so allow 4x.
	float limit = (float) (4.0 * 2 * 3.14159265 * 6.0 / SR);
	std::printf("  max step %.5f, limit %.5f (a snap would be up to 2.0)\n", maxStep, limit);
	CHECK(maxStep <= limit, "LFO output jumps: %f > %f", maxStep, limit);
}

int main() {
	testFree();
	testSync();
	testPhaseLock();
	testContinuity();
	testFold();
	if (failures) {
		std::printf("%d check(s) FAILED\n", failures);
		return 1;
	}
	std::printf("all checks passed\n");
	return 0;
}
