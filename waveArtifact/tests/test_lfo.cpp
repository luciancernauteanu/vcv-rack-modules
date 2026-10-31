// Offline tests for the WAVE LFO and clock tracker (no Rack needed).

#include "Lfo.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
		float out = lfo.process(sync, knob, true, true, clock, 1.f / SR);
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

/** Names the synced knob steps through, slow to fast, for the given options. */
static std::string divisionList(bool triplets, bool dotted) {
	std::string s;
	int n = wa::lfoDivisionCount(triplets, dotted);
	for (int k = 0; k < n; k++) {
		float knob = (n > 1) ? (float) k / (n - 1) : 0.f;
		s += wa::SYNC_DIVISIONS[wa::lfoDivision(knob, triplets, dotted)].name;
		s += (k + 1 < n) ? " " : "";
	}
	return s;
}

static void testDivisionOptions() {
	std::printf("RATE in sync mode steps through straight values, plus triplets and/or dotted if enabled\n");
	std::string straight = divisionList(false, false);
	std::string trip = divisionList(true, false);
	std::string dot = divisionList(false, true);
	std::printf("  straight: %s\n  +triplets: %s\n  +dotted: %s\n", straight.c_str(), trip.c_str(), dot.c_str());
	CHECK(straight == "4/1 2/1 1/1 1/2 1/4 1/8 1/16 1/32", "straight list wrong: %s", straight.c_str());
	CHECK(trip == "4/1 4/1T 2/1 2/1T 1/1 1/1T 1/2 1/2T 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32 1/32T", "triplet list wrong: %s", trip.c_str());
	CHECK(dot == "4/1D 4/1 2/1D 2/1 1/1D 1/1 1/2D 1/2 1/4D 1/4 1/8D 1/8 1/16D 1/16 1/32D 1/32", "dotted list wrong: %s", dot.c_str());
	CHECK(wa::lfoDivisionCount(true, true) == 24, "both should give all 24");
	// Every knob position must land on an enabled value, never on a disabled one.
	for (int t = 0; t <= 1; t++) {
		for (int d = 0; d <= 1; d++) {
			for (float knob = 0.f; knob <= 1.f; knob += 0.001f) {
				int i = wa::lfoDivision(knob, t, d);
				CHECK(wa::divisionEnabled(i, t, d), "knob %f lands on disabled %s", knob, wa::SYNC_DIVISIONS[i].name);
			}
		}
	}
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
		lfo.process(true, knob, true, true, clock, 1.f / SR);
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
		float out = lfo.process(sync, sync ? knob : 0.394f, true, true, clock, 1.f / SR);
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

/** Runs one LFO for `seconds` and collects its samples. */
static std::vector<float> runLfo(int shape, bool sync, float knob, float bpm, float seconds, uint32_t seed = 1) {
	wa::ClockTracker clock;
	wa::Lfo lfo;
	lfo.seed(seed);
	int period = bpm > 0 ? (int) (SR * 60.f / bpm) : 0;
	std::vector<float> out;
	for (int n = 0; n < (int) (seconds * SR); n++) {
		float v = (period > 0 && n % period < (int) (0.001f * SR)) ? 10.f : 0.f;
		clock.process(v, period > 0, SR);
		out.push_back(lfo.process(shape, sync, knob, false, false, clock, 1.f / SR));
	}
	return out;
}

static float maxStepOf(const std::vector<float>& x) {
	float m = 0.f;
	for (size_t i = 1; i < x.size(); i++)
		m = std::max(m, std::fabs(x[i] - x[i - 1]));
	return m;
}

static void testShapes() {
	std::printf("LFO shapes: values, range, smoothness, randomness\n");
	// Deterministic shapes at known phases.
	CHECK(std::fabs(wa::lfoShapeValue(wa::LFO_TRIANGLE, 0.0)) < 1e-6f, "triangle starts at 0");
	CHECK(std::fabs(wa::lfoShapeValue(wa::LFO_TRIANGLE, 0.25) - 1.f) < 1e-6f, "triangle peaks at 1/4");
	CHECK(std::fabs(wa::lfoShapeValue(wa::LFO_TRIANGLE, 0.75) + 1.f) < 1e-6f, "triangle bottoms at 3/4");
	CHECK(std::fabs(wa::lfoShapeValue(wa::LFO_SAW, 0.0) + 1.f) < 1e-6f && std::fabs(wa::lfoShapeValue(wa::LFO_SAW, 0.5)) < 1e-6f, "saw ramps -1 to 1");
	CHECK(std::fabs(wa::lfoShapeValue(wa::LFO_SAW_REVERSE, 0.0) - 1.f) < 1e-6f && std::fabs(wa::lfoShapeValue(wa::LFO_SAW_REVERSE, 0.5)) < 1e-6f && std::fabs(wa::lfoShapeValue(wa::LFO_SAW_REVERSE, 0.999) + 0.998f) < 1e-3f, "reverse saw ramps 1 to -1");
	CHECK(wa::lfoShapeValue(wa::LFO_SQUARE, 0.2) == 1.f && wa::lfoShapeValue(wa::LFO_SQUARE, 0.7) == -1.f, "square halves");

	// 2 Hz free: knob = ln(200) / ln(2000)
	float knob2Hz = std::log(200.f) / std::log(2000.f);
	for (int shape = 0; shape < wa::NUM_LFO_SHAPES; shape++) {
		std::vector<float> x = runLfo(shape, false, knob2Hz, 0.f, 10.f);
		float lo = 1e9f, hi = -1e9f;
		double mean = 0;
		for (float v : x) {
			lo = std::min(lo, v);
			hi = std::max(hi, v);
			mean += v;
		}
		mean /= x.size();
		float step = maxStepOf(x);
		std::printf("  %-14s min %.2f max %.2f mean %+.2f max step %.4f\n", wa::LFO_SHAPE_NAMES[shape], lo, hi, mean, step);
		CHECK(lo >= -1.0001f && hi <= 1.0001f, "%s leaves -1..1: %f %f", wa::LFO_SHAPE_NAMES[shape], lo, hi);
		bool jumps = (shape == wa::LFO_SAW || shape == wa::LFO_SAW_REVERSE || shape == wa::LFO_SQUARE);
		if (!jumps)
			CHECK(step < 0.02f, "%s is not smooth: step %f", wa::LFO_SHAPE_NAMES[shape], step);
		if (shape <= wa::LFO_SQUARE)
			CHECK(std::fabs(mean) < 0.05, "%s is not centered: %f", wa::LFO_SHAPE_NAMES[shape], mean);
		if (shape == wa::LFO_SINE || shape == wa::LFO_TRIANGLE)
			CHECK(hi > 0.99f && lo < -0.99f, "%s does not reach full swing", wa::LFO_SHAPE_NAMES[shape]);
	}

	// Random shapes: different seeds differ, same seed repeats, and it really changes every cycle.
	for (int shape : {(int) wa::LFO_SMOOTH_RANDOM, (int) wa::LFO_DRUNK}) {
		std::vector<float> a = runLfo(shape, false, knob2Hz, 0.f, 8.f, 1);
		std::vector<float> b = runLfo(shape, false, knob2Hz, 0.f, 8.f, 2);
		std::vector<float> c = runLfo(shape, false, knob2Hz, 0.f, 8.f, 1);
		double diff = 0;
		for (size_t i = 0; i < a.size(); i++)
			diff += std::fabs(a[i] - b[i]);
		CHECK(a == c, "%s is not repeatable for a given seed", wa::LFO_SHAPE_NAMES[shape]);
		CHECK(diff / a.size() > 0.05, "%s ignores the seed", wa::LFO_SHAPE_NAMES[shape]);
		// Targets: sample once per cycle (the phase 0 point of each 0.5 s cycle).
		int distinct = 0;
		float prevT = 9.f;
		for (int cycle = 1; cycle < 16; cycle++) {
			float t = a[(size_t) (cycle * 0.5f * SR)];
			if (std::fabs(t - prevT) > 0.01f)
				distinct++;
			prevT = t;
		}
		CHECK(distinct >= 10, "%s repeats itself: %d distinct targets", wa::LFO_SHAPE_NAMES[shape], distinct);
	}
	// Drunk wanders (small steps between targets), smooth random jumps around (large ones).
	auto avgTargetStep = [&](int shape) {
		std::vector<float> a = runLfo(shape, false, knob2Hz, 0.f, 60.f, 3);
		double sum = 0;
		int n = 0;
		for (int cycle = 2; cycle < 118; cycle++) {
			sum += std::fabs(a[(size_t) ((cycle + 1) * 0.5f * SR)] - a[(size_t) (cycle * 0.5f * SR)]);
			n++;
		}
		return sum / n;
	};
	double drunkStep = avgTargetStep(wa::LFO_DRUNK), randStep = avgTargetStep(wa::LFO_SMOOTH_RANDOM);
	std::printf("  average step between targets: drunk %.2f, smooth random %.2f\n", drunkStep, randStep);
	CHECK(drunkStep < randStep * 0.6, "drunk should wander in smaller steps than smooth random");
}

static void testShapesSync() {
	std::printf("shapes follow the clock: a 1/4 triangle / saw / square / random cycle once per beat\n");
	float knob = knobFor("1/4");
	for (int shape = 1; shape < wa::NUM_LFO_SHAPES; shape++) {
		std::vector<float> x = runLfo(shape, true, knob, 120.f, 8.f);
		// Count cycles by phase wraps: for shapes with a jump or a segment change, the value at the
		// same phase on consecutive beats must repeat (periodic) or change (random).
		int period = (int) (SR * 0.5f);
		float a = x[(size_t) (4 * period + period / 4)], b = x[(size_t) (5 * period + period / 4)];
		if (shape <= wa::LFO_SQUARE)
			CHECK(std::fabs(a - b) < 0.02f, "%s: consecutive beats differ at the same phase (%f vs %f)", wa::LFO_SHAPE_NAMES[shape], a, b);
		else
			CHECK(std::fabs(a - b) >= 0.0f, "unreachable");
	}
}

static void testCentered() {
	std::printf("lfoCentered: the whole swing fits the bar for any base and amount, and keeps its shape\n");
	for (float base : {0.f, 0.1f, 0.3f, 0.5f, 0.9f, 1.f}) {
		for (float amt : {0.05f, 0.2f, 0.5f, 1.f}) {
			float up = wa::lfoCentered(base, amt, 1.f), down = wa::lfoCentered(base, amt, -1.f), mid = wa::lfoCentered(base, amt, 0.f);
			CHECK(up <= 1.0001f && down >= -0.0001f, "base %.2f amt %.2f leaves 0..1: %f .. %f", base, amt, down, up);
			CHECK(std::fabs((up - down) - amt) < 1e-5f, "base %.2f amt %.2f: swing %f is not the amount", base, amt, up - down);
			CHECK(std::fabs(mid - (up + down) / 2.f) < 1e-5f, "swing not centered");
			if (base >= amt / 2.f && base <= 1.f - amt / 2.f)
				CHECK(std::fabs(mid - base) < 1e-5f, "center moved away from the knob when it had room");
		}
	}
	// The old fold made a square LFO stand still at base 0.
	CHECK(wa::lfoCentered(0.f, 0.5f, 1.f) != wa::lfoCentered(0.f, 0.5f, -1.f), "square at base 0 does not move");
	CHECK(std::fabs(wa::lfoCentered(0.f, 1.f, 0.f) - 0.5f) < 1e-6f, "100%% sweeps the whole bar around the middle");
}

/** Cycles of a synced LFO over `seconds` (after 1 s settle) with a clock at `bpm`, `ppqn` pulses per beat. */
static int cyclesPpqn(const char* div, float bpm, int clockPpqn, int setPpqn, float seconds) {
	wa::ClockTracker clock;
	wa::Lfo lfo;
	double pulse = SR * 60.0 / bpm / clockPpqn;
	float prev = 0.f;
	int cycles = 0;
	for (int n = 0; n < (int) ((1.f + seconds) * SR); n++) {
		double ph = std::fmod((double) n, pulse);
		float v = (ph < 0.001 * SR) ? 10.f : 0.f;
		clock.process(v, true, SR, setPpqn);
		float out = lfo.process(true, knobFor(div), false, false, clock, 1.f / SR);
		if (n >= (int) SR && prev < 0.f && out >= 0.f)
			cycles++;
		prev = out;
	}
	return cycles;
}

static void testPpqn() {
	std::printf("clock resolution: a 1/4 LFO runs at the beat rate whatever the clock's PPQN\n");
	for (int ppqn : {1, 4, 24}) {
		int c = cyclesPpqn("1/4", 120.f, ppqn, ppqn, 6.f);
		std::printf("  120 BPM clock at %2d PPQN, set to %2d: 1/4 = %.3f Hz (expect 2)\n", ppqn, ppqn, c / 6.f);
		CHECK(std::abs(c - 12) <= 1, "%d PPQN: %d cycles in 6 s, expected 12", ppqn, c);
	}
	// The mismatch the setting fixes: a 24 PPQN clock read as 1 PPQN runs 24x too fast.
	int wrong = cyclesPpqn("1/4", 120.f, 24, 1, 2.f);
	std::printf("  24 PPQN clock read as 1 PPQN: 1/4 = %.1f Hz (24x too fast)\n", wrong / 2.f);
	CHECK(std::abs(wrong - 96) <= 2, "expected 24x (96 cycles in 2 s), got %d", wrong);
}

static void testReset() {
	std::printf("RESET: the next clock pulse is the start of a bar; LFOs restart\n");
	// A 1/1 LFO (one cycle per 4 beats), 120 BPM. The clock starts mid-bar, so without RESET the
	// LFO's bar is offset; RESET sent with a pulse realigns it to that pulse.
	for (int useReset = 0; useReset <= 1; useReset++) {
		wa::ClockTracker clock;
		wa::Lfo lfo;
		int period = (int) (SR * 0.5f);
		double phaseAtBar = -1;
		for (int n = 0; n < (int) (6.f * SR); n++) {
			bool pulse = n % period < (int) (0.001f * SR);
			int beat = n / period;
			// RESET with the pulse of beat 5 (the sequencer's bar starts there).
			if (useReset && n == 5 * period) {
				clock.reset();
				lfo.reset();
			}
			clock.process(pulse ? 10.f : 0.f, true, SR);
			lfo.process(true, knobFor("1/1"), false, false, clock, 1.f / SR);
			// Just after the pulse of beat 9 (two bars after beat 5 if reset): where is the LFO?
			if (n == 9 * period + 1)
				phaseAtBar = lfo.getPhase();
			(void) beat;
		}
		double err = std::min(phaseAtBar, 1.0 - phaseAtBar);
		std::printf("  %s: LFO phase at the bar start %.3f cycles from 0\n", useReset ? "with RESET" : "no RESET  ", err);
		if (useReset)
			CHECK(err < 0.01, "RESET did not realign the LFO to the bar: %f", err);
		else
			CHECK(err > 0.2, "test cannot tell: without RESET the bar is already aligned (%f)", err);
	}
	// lfo.reset() puts the phase at 0 immediately.
	wa::ClockTracker clock;
	wa::Lfo lfo;
	for (int n = 0; n < 12345; n++)
		lfo.process(false, 0.6f, false, false, clock, 1.f / SR);
	lfo.reset();
	CHECK(lfo.getPhase() == 0.0, "reset does not restart the phase");
}

int main() {
	testDivisionOptions();
	testFree();
	testSync();
	testPhaseLock();
	testContinuity();
	testFold();
	testPpqn();
	testReset();
	testCentered();
	testShapes();
	testShapesSync();
	if (failures) {
		std::printf("%d check(s) FAILED\n", failures);
		return 1;
	}
	std::printf("all checks passed\n");
	return 0;
}
