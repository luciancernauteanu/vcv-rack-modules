// Offline tests for the sequencer's parameter locks, trig conditions and slide (StepLocks.hpp, no Rack needed).
// Build and run: cd tests && make run
//
// Every check prints the measured value next to its threshold.

#include "StepLocks.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>

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

static uint32_t rngState = 12345u;
static float rnd() {
	rngState ^= rngState << 13;
	rngState ^= rngState >> 17;
	rngState ^= rngState << 5;
	return (float) (rngState >> 8) / 16777216.f;
}

static void testConditions() {
	std::printf("conditions\n");
	wa::StepData s;
	CHECK(wa::conditionPasses(s, 0, false, 0.99f), "ALWAYS must play");

	// Chance: 50% over 10000 passes plays about half
	s.cond = wa::COND_CHANCE;
	s.chance = 50;
	int played = 0;
	for (int i = 0; i < 10000; i++)
		played += wa::conditionPasses(s, i, false, rnd()) ? 1 : 0;
	std::printf("  50%% chance: %d of 10000 (want 4800..5200)\n", played);
	CHECK(played > 4800 && played < 5200, "50%% chance played %d", played);
	s.chance = 10;
	played = 0;
	for (int i = 0; i < 10000; i++)
		played += wa::conditionPasses(s, i, false, rnd()) ? 1 : 0;
	std::printf("  10%% chance: %d of 10000 (want 850..1150)\n", played);
	CHECK(played > 850 && played < 1150, "10%% chance played %d", played);

	// A:B: 2:4 plays on pass 2 of every 4 (pass index 1, 5, 9 ...)
	s.cond = wa::COND_RATIO;
	s.a = 2;
	s.b = 4;
	for (long p = 0; p < 12; p++) {
		bool want = (p % 4) == 1;
		CHECK(wa::conditionPasses(s, p, false, 0.f) == want, "2:4 on pass index %ld: got %d", p, (int) !want);
	}
	s.a = 1;
	s.b = 2;
	CHECK(wa::conditionPasses(s, 0, false, 0.f) && !wa::conditionPasses(s, 1, false, 0.f) && wa::conditionPasses(s, 2, false, 0.f), "1:2 must play passes 1, 3 ...");

	// FILL / NOT FILL
	s.cond = wa::COND_FILL;
	CHECK(wa::conditionPasses(s, 0, true, 0.f) && !wa::conditionPasses(s, 0, false, 0.f), "FILL must follow fill");
	s.cond = wa::COND_NOT_FILL;
	CHECK(!wa::conditionPasses(s, 0, true, 0.f) && wa::conditionPasses(s, 0, false, 0.f), "NOT FILL must be the opposite");

	char text[16];
	s.cond = wa::COND_RATIO;
	s.a = 3;
	s.b = 8;
	wa::conditionText(s, text, sizeof(text));
	CHECK(std::string(text) == "3:8", "text %s", text);
}

static void testLocks() {
	std::printf("locks\n");
	float knobs[wa::NUM_LOCKABLE] = {};
	for (int k = 0; k < wa::NUM_LOCKABLE; k++)
		knobs[k] = 0.5f;
	wa::MarkerLocks locks;
	CHECK(locks.get(9, 0.5f) == 0.5f, "no step played: the knob");

	// A step locking CUTOFF (9) to 0.2: it plays 0.2 until the next step, the others play their knobs.
	wa::StepData a;
	a.setLock(9, 0.2f);
	locks.apply(a.value, a.mask, false, 0.25, knobs, 0);
	CHECK(locks.get(9, 0.5f) == 0.2f, "locked cutoff: %g", locks.get(9, 0.5f));
	CHECK(locks.get(8, 0.7f) == 0.7f, "unlocked gain follows its knob: %g", locks.get(8, 0.7f));
	for (int i = 0; i < 1000; i++)
		locks.advance(0.001);
	CHECK(locks.get(9, 0.9f) == 0.2f, "the lock holds until the next step, whatever the knob: %g", locks.get(9, 0.9f));

	// The next step has no locks: back to the knob at once (no slide).
	wa::StepData plain;
	locks.apply(plain.value, plain.mask, false, 0.25, knobs, 0);
	CHECK(locks.get(9, 0.5f) == 0.5f, "an unlocked step plays the knob: %g", locks.get(9, 0.5f));

	// Clear (SEQ off): everything is the knob.
	locks.apply(a.value, a.mask, false, 0.25, knobs, 0);
	locks.clear();
	CHECK(locks.get(9, 0.5f) == 0.5f, "cleared: the knob");

	// Lock bits beyond 31 work (64-bit mask): RELEASE is 32, SCAN LENGTH 36.
	wa::StepData far;
	far.setLock(36, 0.9f);
	far.setLock(32, 0.1f);
	CHECK(far.locked(36) && far.locked(32) && !far.locked(4), "high bits");
	locks.apply(far.value, far.mask, false, 0.25, knobs, 0);
	CHECK(locks.get(36, 0.f) == 0.9f && locks.get(32, 0.f) == 0.1f && locks.get(4, 0.3f) == 0.3f, "high-bit locks play");
	far.clearLock(36);
	CHECK(!far.locked(36) && far.locked(32), "clearLock only that one");
}

static void testLockable() {
	std::printf("lockable\n");
	int count = 0;
	for (int k = 0; k < wa::NUM_LOCKABLE; k++)
		count += wa::isLockable(k) ? 1 : 0;
	std::printf("  lockable settings: %d of %d (want 28)\n", count, wa::NUM_LOCKABLE);
	CHECK(count == 28, "lockable count %d", count);
	// The four lockable switches / lists, and the nine that are not
	for (int k : {7, 11, 20, 27})
		CHECK(wa::isLockable(k), "%s must be lockable", wa::LOCK_NAMES[k]);
	for (int k : {4, 5, 6, 14, 21, 22, 23, 33, 34})
		CHECK(!wa::isLockable(k), "%s must not be lockable", wa::LOCK_NAMES[k]);
	wa::StepData s;
	s.setLock(4, 1.f);
	CHECK(!s.locked(4) && s.mask == 0, "setLock ignores a setting that is not lockable");
}

static void testSlide() {
	std::printf("slide\n");
	float knobs[wa::NUM_LOCKABLE] = {};
	for (int k = 0; k < wa::NUM_LOCKABLE; k++)
		knobs[k] = 0.5f;
	wa::MarkerLocks locks;
	wa::StepData a, b;
	a.setLock(9, 0.2f);
	b.setLock(9, 0.8f);
	locks.apply(a.value, a.mask, false, 0.25, knobs, 0);
	// Slide to 0.8 over one step (0.25 beats), at 0.0025 beats per sample = 100 samples.
	locks.apply(b.value, b.mask, true, 0.25, knobs, 0);
	CHECK(locks.get(9, 0.5f) == 0.2f, "a slide starts from what was playing: %g", locks.get(9, 0.5f));
	float last = 0.2f, most = 0.f;
	int samples = 0;
	while (locks.gliding && samples < 1000) {
		locks.advance(0.0025);
		float v = locks.get(9, 0.5f);
		most = std::max(most, v);
		CHECK(v >= last - 1e-6f, "the glide must not go back: %g after %g", v, last);
		last = v;
		samples++;
	}
	std::printf("  glide took %d samples (want 100), peak %.4f (want <= 0.8)\n", samples, most);
	CHECK(samples >= 99 && samples <= 101, "glide length %d samples", samples);
	CHECK(most <= 0.8f + 1e-6f, "overshoot %g", most);
	CHECK(locks.get(9, 0.5f) == 0.8f, "ends on the target: %g", locks.get(9, 0.5f));

	// Half way: half way there.
	locks.apply(a.value, a.mask, true, 0.25, knobs, 0);
	for (int i = 0; i < 50; i++)
		locks.advance(0.0025);
	float mid = locks.get(9, 0.5f);
	std::printf("  half way: %.4f (want 0.5)\n", mid);
	CHECK(std::fabs(mid - 0.5f) < 0.01f, "half way %g", mid);

	// A slide step without the lock glides back to the knob.
	wa::StepData plain;
	locks.apply(b.value, b.mask, false, 0.25, knobs, 0);
	locks.apply(plain.value, plain.mask, true, 0.25, knobs, 0);
	CHECK(locks.get(9, 0.5f) == 0.8f, "glide back starts at the old lock");
	for (int i = 0; i < 200; i++)
		locks.advance(0.0025);
	CHECK(locks.get(9, 0.5f) == 0.5f, "glide back ends on the knob: %g", locks.get(9, 0.5f));

	// Switches never glide (jumpMask).
	wa::StepData sw;
	sw.setLock(11, 3.f);
	locks.apply(plain.value, plain.mask, false, 0.25, knobs, 0);
	locks.apply(sw.value, sw.mask, true, 0.25, knobs, (uint64_t) 1u << 11);
	CHECK(locks.get(11, 0.f) == 3.f, "a switch jumps even on a slide step: %g", locks.get(11, 0.f));
}

/** SLIDE as an amount: the glide takes that share of a step; 0 = a jump. A tied step keeps the group's locks. */
static void testSlideAmountAndTie() {
	std::printf("slide amount, tie\n");
	float knobs[wa::NUM_LOCKABLE] = {};
	for (int k = 0; k < wa::NUM_LOCKABLE; k++)
		knobs[k] = 0.5f;
	wa::StepData a, b;
	a.setLock(9, 0.2f);
	b.setLock(9, 0.8f);
	// One step = 0.25 beats at 0.0025 beats per sample = 100 samples.
	const float amounts[4] = {1.f, 0.5f, 0.1f, 0.01f};
	const int want[4] = {100, 50, 10, 1};
	for (int i = 0; i < 4; i++) {
		wa::MarkerLocks locks;
		locks.apply(a.value, a.mask, 0.f, 0.25, knobs, 0);
		locks.apply(b.value, b.mask, amounts[i], 0.25, knobs, 0);
		int samples = 0;
		while (locks.gliding && samples < 1000) {
			locks.advance(0.0025);
			samples++;
		}
		std::printf("  slide %3.0f%%: glide %d samples (want %d)\n", amounts[i] * 100.f, samples, want[i]);
		CHECK(std::abs(samples - want[i]) <= 1, "slide %g: %d samples", amounts[i], samples);
		CHECK(locks.get(9, 0.5f) == 0.8f, "ends on the target: %g", locks.get(9, 0.5f));
	}
	{
		wa::MarkerLocks locks;
		locks.apply(a.value, a.mask, 0.f, 0.25, knobs, 0);
		locks.apply(b.value, b.mask, 0.f, 0.25, knobs, 0);
		CHECK(!locks.gliding && locks.get(9, 0.5f) == 0.8f, "slide off jumps: %g", locks.get(9, 0.5f));
	}
	// A tied step: the group's locks stay, its own go on top, its own slide.
	wa::StepData head, tied;
	head.setLock(9, 0.2f);
	head.setLock(8, 2.f);
	head.slide = 0.3f;
	tied.setLock(9, 0.7f);
	tied.slide = 0.6f;
	wa::StepData m = wa::mergeTied(head, tied);
	std::printf("  tied: cutoff %.2f (0.70) gain %.2f (2.00, kept) slide %.2f (0.60)\n", m.value[9], m.value[8], m.slide);
	CHECK(m.locked(9) && m.value[9] == 0.7f, "the tied step's own lock wins");
	CHECK(m.locked(8) && m.value[8] == 2.f, "the group's lock stays");
	CHECK(m.slide == 0.6f && m.tie, "its own slide, marked tied");
	wa::StepData empty;
	wa::StepData m2 = wa::mergeTied(head, empty);
	CHECK(m2.mask == head.mask, "a tied step without locks keeps the group's: %llx vs %llx", (unsigned long long) m2.mask, (unsigned long long) head.mask);
	wa::StepData t;
	t.tie = true;
	CHECK(!t.isPlain(), "a tie is saved");
	t.tie = false;
	t.slide = 0.01f;
	CHECK(!t.isPlain(), "a 1%% slide is saved");
}

/** COND RND's picker: every result is a valid condition, mostly chances and A:B, FILL / NOT FILL rare */
static void testRandomCondition() {
	std::printf("random condition\n");
	int count[wa::NUM_CONDITIONS] = {};
	bool valid = true;
	const int N = 20000;
	for (int i = 0; i < N; i++) {
		wa::StepData d;
		wa::randomCondition(d, rnd(), rnd(), rnd());
		count[d.cond]++;
		if (d.cond == wa::COND_CHANCE) {
			bool listed = false;
			for (int c : wa::CHANCES)
				listed = listed || d.chance == c;
			valid = valid && listed;
		}
		if (d.cond == wa::COND_RATIO)
			valid = valid && d.b >= 2 && d.b <= 8 && d.a >= 1 && d.a <= d.b;
	}
	// The edges of [0, 1) too
	for (float u : {0.f, 0.999999f}) {
		wa::StepData d;
		wa::randomCondition(d, u, u, u);
		valid = valid && d.cond != wa::COND_ALWAYS && d.a >= 1 && d.a <= d.b && d.b <= 8;
	}
	std::printf("  always %d, chance %.1f%% (50), A:B %.1f%% (35), FILL %.1f%% (7.5), NOT FILL %.1f%% (7.5), all valid %d\n", count[wa::COND_ALWAYS],
		100.0 * count[wa::COND_CHANCE] / N, 100.0 * count[wa::COND_RATIO] / N, 100.0 * count[wa::COND_FILL] / N, 100.0 * count[wa::COND_NOT_FILL] / N, (int) valid);
	CHECK(valid, "every random condition must be one the menu offers");
	CHECK(count[wa::COND_ALWAYS] == 0, "the picker always gives a condition: %d", count[wa::COND_ALWAYS]);
	CHECK(std::abs(count[wa::COND_CHANCE] - N / 2) < N / 50, "chance share %d", count[wa::COND_CHANCE]);
	CHECK(count[wa::COND_FILL] > N / 20 && count[wa::COND_FILL] < N / 10, "FILL share %d", count[wa::COND_FILL]);
}

/** MOD output: jumps without slide, glides over slide x a step with it, holds after */
static void testGlideValue() {
	std::printf("glide value (MOD)\n");
	wa::GlideValue g;
	g.set(0.8f, 0.f, 0.25);
	CHECK(g.value == 0.8f, "no slide jumps: %g", g.value);
	// 0.25 beats per step, 0.0025 beats per sample: a step is 100 samples; slide 50% = 50 samples
	g.set(0.2f, 0.5f, 0.25);
	CHECK(g.value == 0.8f, "a glide starts where it was: %g", g.value);
	int n = 0;
	while (g.progress < 1.0 && n < 1000) {
		g.advance(0.0025);
		n++;
	}
	std::printf("  slide 50%%: %d samples (50), ends %.3f (0.200)\n", n, g.value);
	CHECK(n == 50 && g.value == 0.2f, "glide %d samples to %g", n, g.value);
	g.advance(1.0);
	CHECK(g.value == 0.2f, "holds after: %g", g.value);
	g.set(0.2f, 1.f, 0.25);
	CHECK(g.progress == 1.0, "the same value does not start a glide");
}

int main() {
	testGlideValue();
	testRandomCondition();
	testSlideAmountAndTie();
	testConditions();
	testLocks();
	testLockable();
	testSlide();
	if (failures) {
		std::printf("test_seq: %d failures\n", failures);
		return 1;
	}
	std::printf("test_seq: all checks passed\n");
	return 0;
}
