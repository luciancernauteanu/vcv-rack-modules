// Offline tests for POLY play mode (no Rack needed). Build and run: cd tests && make run
//
// Every check prints the measured value next to its threshold.

#include "GrainEngine.hpp"
#include "MorphEngine.hpp"
#include "PolyVoices.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
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

static const double PI = 3.14159265358979323846;
static const float SR = 48000.f;
static const unsigned SRC_SR = 44100;
static const int M = wa::NUM_MARKERS;

static double rms(const std::vector<float>& v, size_t from, size_t to) {
	double s = 0.0;
	for (size_t i = from; i < to && i < v.size(); i++)
		s += (double) v[i] * v[i];
	return std::sqrt(s / std::max<size_t>(to - from, 1));
}

static double tone(const std::vector<float>& v, size_t from, size_t to, double hz) {
	double w = 2 * PI * hz / SR, c = 2 * std::cos(w), s1 = 0, s2 = 0;
	for (size_t i = from; i < to; i++) {
		double s = v[i] + c * s1 - s2;
		s2 = s1;
		s1 = s;
	}
	double p = s1 * s1 + s2 * s2 - c * s1 * s2;
	return 2.0 * std::sqrt(std::max(p, 0.0)) / (double) (to - from);
}

/** Hann-windowed Goertzel: a loud neighbor does not leak into the reading */
static double toneClean(const std::vector<float>& v, size_t from, size_t to, double hz) {
	double w = 2 * PI * hz / SR, c = 2 * std::cos(w), s1 = 0, s2 = 0;
	size_t n = to - from;
	for (size_t i = 0; i < n; i++) {
		double win = 0.5 - 0.5 * std::cos(2 * PI * i / n);
		double s = v[from + i] * win + c * s1 - s2;
		s2 = s1;
		s1 = s;
	}
	double p = s1 * s1 + s2 * s2 - c * s1 * s2;
	return 4.0 * std::sqrt(std::max(p, 0.0)) / (double) n;
}

static double maxStep(const std::vector<float>& v, size_t from = 1) {
	double m = 0.0;
	for (size_t i = std::max<size_t>(from, 1); i < v.size(); i++)
		m = std::max(m, (double) std::fabs(v[i] - v[i - 1]));
	return m;
}

static double peakOf(const std::vector<float>& v) {
	double m = 0.0;
	for (float x : v)
		m = std::max(m, (double) std::fabs(x));
	return m;
}

/** Every table one sine cycle, plus a noise sample for the granular engine. */
static std::unique_ptr<wa::WavetableBank> makeBank() {
	std::unique_ptr<wa::WavetableBank> bank(new wa::WavetableBank);
	int tables = 2;
	bank->numTables = tables;
	bank->data.assign((size_t) tables * wa::NUM_LEVELS * wa::FRAME_SIZE, 0.f);
	bank->brightness.assign(tables, 0.f);
	bank->pitched.assign(tables, 1);
	for (int t = 0; t < tables; t++)
		for (int l = 0; l < wa::NUM_LEVELS; l++)
			for (int i = 0; i < wa::FRAME_SIZE; i++)
				bank->frame(t, l)[i] = (float) std::sin(2 * PI * i / wa::FRAME_SIZE);
	std::vector<float> s((size_t) (6 * SRC_SR));
	uint32_t r = 12345u;
	for (float& x : s) {
		r = r * 1664525u + 1013904223u;
		x = ((r >> 8) * (1.f / 8388608.f) - 1.f) * 0.5f;
	}
	wa::buildSampleBuffer(s.data(), s.size(), 1, SRC_SR, bank->sample);
	return bank;
}

static float densityKnob(float hz) {
	return std::log(hz / 0.5f) / 6.907755279f;
}
static float sizeKnob(float seconds) {
	return std::log(seconds / 0.002f) / 6.214608098f;
}

/** Trigger and ADSRs driven the way the module drives them */
struct Voices {
	wa::PolyVoices v;
	float pos[M] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};
	bool on[M] = {true, true, true, true, true};
	wa::AdsrSettings s[M];
	Voices() {
		for (int m = 0; m < M; m++)
			s[m].hold = -1.f; // until another marker: most of these tests are about one thing at a time
	}
	void step(float position) {
		v.process(pos, on, position, s, 1.f / SR);
	}
};

static void testAdsr() {
	std::printf("adsr\n");
	wa::Adsr a;
	const float dt = 1.f / SR;
	a.gateOn();
	long n = 0;
	while (a.stage == wa::Adsr::ATTACK && n < 100000) {
		a.process(0.05f, 0.2f, 0.5f, 0.3f, dt);
		n++;
	}
	double attack = n / (double) SR;
	std::printf("  attack: %.1f ms (knob 50, limit +-1)\n", attack * 1000.0);
	CHECK(std::fabs(attack - 0.05) < 0.001, "attack %.4f", attack);

	// Decay: 60 dB of the way to sustain in the DECAY time.
	long t60 = 0;
	while (std::fabs(a.level - 0.5f) > 0.5f * 0.001f && t60 < 200000) {
		a.process(0.05f, 0.2f, 0.5f, 0.3f, dt);
		t60++;
	}
	std::printf("  decay to -60 dB of its way: %.1f ms (knob 200, limit +-4), level %.3f\n", t60 * 1000.0 / SR, a.level);
	CHECK(std::fabs(t60 / (double) SR - 0.2) < 0.004, "decay %ld samples", t60);
	for (int i = 0; i < 24000; i++)
		a.process(0.05f, 0.2f, 0.5f, 0.3f, dt);
	std::printf("  sustain: %.4f (knob 0.5)\n", a.level);
	CHECK(a.stage == wa::Adsr::SUSTAIN && std::fabs(a.level - 0.5f) < 1e-3f, "sustain %.4f", a.level);

	a.gateOff();
	long r60 = 0;
	while (a.level > 0.5f * 0.001f && r60 < 200000) {
		a.process(0.05f, 0.2f, 0.5f, 0.3f, dt);
		r60++;
	}
	std::printf("  release to -60 dB: %.1f ms (knob 300, limit +-6)\n", r60 * 1000.0 / SR);
	CHECK(std::fabs(r60 / (double) SR - 0.3) < 0.006, "release %ld samples", r60);
	for (int i = 0; i < 48000 && a.stage != wa::Adsr::IDLE; i++)
		a.process(0.05f, 0.2f, 0.5f, 0.3f, dt);
	CHECK(a.stage == wa::Adsr::IDLE && a.level == 0.f, "must end silent and idle: %.6f", a.level);

	// A new note during the release continues from where the level is.
	wa::Adsr b;
	b.gateOn();
	float last = 0.f, biggest = 0.f;
	for (int i = 0; i < 48000; i++) {
		if (i == 12000)
			b.gateOff();
		if (i == 13000)
			b.gateOn();
		float l = b.process(0.001f, 0.2f, 1.f, 0.05f, dt);
		biggest = std::max(biggest, std::fabs(l - last));
		last = l;
	}
	float bound = dt / 0.001f * 1.01f;
	std::printf("  retrigger in release: biggest level step %.5f (limit %.5f, the attack's own slope)\n", biggest, bound);
	CHECK(biggest <= bound, "step %.5f", biggest);

	// Knob ends
	CHECK(std::fabs(wa::adsrAttackSeconds(0.f) - 0.001f) < 1e-6f && std::fabs(wa::adsrAttackSeconds(1.f) - 5.f) < 1e-3f, "attack range");
	CHECK(std::fabs(wa::adsrFallSeconds(0.f) - 0.005f) < 1e-6f && std::fabs(wa::adsrFallSeconds(1.f) - 10.f) < 1e-2f, "fall range");
	// The defaults on the panel
	std::printf("  panel defaults: A %.1f ms, D %.0f ms, R %.0f ms (5, 200, 300)\n", wa::adsrAttackSeconds(0.18896f) * 1000.f, wa::adsrFallSeconds(0.48533f) * 1000.f, wa::adsrFallSeconds(0.53866f) * 1000.f);
	CHECK(std::fabs(wa::adsrAttackSeconds(0.18896f) - 0.005f) < 1e-4f, "attack default");
	CHECK(std::fabs(wa::adsrFallSeconds(0.48533f) - 0.2f) < 1e-3f, "decay default");
	CHECK(std::fabs(wa::adsrFallSeconds(0.53866f) - 0.3f) < 1e-3f, "release default");
}

static void testTrigger() {
	std::printf("playhead trigger\n");
	auto sweep = [](Voices& v, float from, float to, double seconds, int* counts) {
		long n = (long) (seconds * SR);
		for (long i = 0; i < n; i++) {
			v.step(from + (to - from) * (float) i / n);
			for (int m = 0; m < M; m++)
				counts[m] += v.v.start[m];
		}
	};
	{
		Voices v;
		int c[M] = {};
		sweep(v, 0.f, 1.f, 1.0, c);
		std::printf("  sweep up: fires %d %d %d %d %d (one each)\n", c[0], c[1], c[2], c[3], c[4]);
		for (int m = 0; m < M; m++)
			CHECK(c[m] == 1, "up: marker %d fired %d", m + 1, c[m]);
		// It stops on the last marker, at the very end of the bar: that note holds.
		CHECK(v.v.gate[4], "marker 5 should hold");
		int d[M] = {};
		sweep(v, 1.f, 0.f, 1.0, d);
		std::printf("  sweep down: fires %d %d %d %d %d (marker 5 is left, the rest once each)\n", d[0], d[1], d[2], d[3], d[4]);
		CHECK(d[4] == 0, "down: marker 5 fired %d", d[4]);
		for (int m = 0; m < 4; m++)
			CHECK(d[m] == 1, "down: marker %d fired %d", m + 1, d[m]);
		CHECK(v.v.gate[0], "marker 1, at the other end of the bar, should hold");
	}
	{
		// Noise on top of a bullet fires it once.
		Voices v;
		v.step(0.1f);
		int c[M] = {};
		sweep(v, 0.1f, 0.5f, 0.2, c);
		for (int m = 0; m < M; m++)
			c[m] = 0;
		uint32_t r = 7u;
		for (int i = 0; i < 96000; i++) {
			r = r * 1664525u + 1013904223u;
			// Reaches past REACH, short of REARM: only the distance needed to re-arm stops it firing again.
			float noise = ((r >> 8) * (1.f / 8388608.f) - 1.f) * 0.009f;
			v.step(0.5f + noise);
			for (int m = 0; m < M; m++)
				c[m] += v.v.start[m];
		}
		std::printf("  +-0.9%% noise sitting on marker 3 for 2 s: fired %d more times (0)\n", c[2]);
		CHECK(c[2] == 0, "noise fired it %d times", c[2]);
	}
	{
		// One step over several markers (saw reset) fires only the last one it passes: landing at 0.05 has
		// not reached marker 1 at 0, so that is marker 2.
		Voices v;
		sweep(v, 0.9f, 0.95f, 0.1, (int[M]){});
		v.step(0.05f);
		int c[M];
		for (int m = 0; m < M; m++)
			c[m] = v.v.start[m];
		std::printf("  jump 0.95 -> 0.05: fires %d %d %d %d %d (only marker 2)\n", c[0], c[1], c[2], c[3], c[4]);
		CHECK(c[1] == 1 && c[0] + c[2] + c[3] + c[4] == 0, "jump");
	}
	{
		// A still playhead is never silent, and the next crossing moves the gate.
		Voices v;
		v.step(0.3f);
		std::printf("  start at 0.30: gate on marker %d (2, the nearest)\n", v.v.gate[1] ? 2 : 0);
		CHECK(v.v.gate[1] && v.v.start[1], "nearest marker should open");
		for (int i = 0; i < 48000; i++)
			v.step(0.3f);
		CHECK(v.v.env[1] > 0.99f, "held note level %.3f", v.v.env[1]);
		int starts = 0;
		for (int i = 0; i < 48000; i++) {
			v.step(0.3f);
			starts += v.v.start[1];
		}
		CHECK(starts == 0, "a still playhead retriggered %d times", starts);
		// Deleting the sounding marker releases its note.
		v.on[1] = false;
		v.step(0.3f);
		CHECK(!v.v.gate[1], "a deleted marker's note must end");
		// Turning one on does not fire it.
		v.on[1] = true;
		v.step(0.3f);
		CHECK(!v.v.start[1], "turning a marker on must not fire it");
	}
	{
		// Markers at one place fire together and hold together: a chord.
		Voices v;
		v.pos[3] = v.pos[2];
		v.step(0.2f);
		int c[M] = {};
		sweep(v, 0.2f, 0.6f, 0.2, c);
		std::printf("  markers 3 and 4 on one spot: fired %d and %d, both holding: %d %d\n", c[2], c[3], (int) v.v.gate[2], (int) v.v.gate[3]);
		CHECK(c[2] == 1 && c[3] == 1 && v.v.gate[2] && v.v.gate[3], "stacked markers fire together");
	}
}

static void testHold() {
	std::printf("note length\n");
	CHECK(std::fabs(wa::adsrHoldSeconds(0.f) - 0.01f) < 1e-6f, "hold minimum");
	CHECK(std::fabs(wa::adsrHoldSeconds(0.99f) - 9.3f) < 0.2f, "near the top: %.2f", wa::adsrHoldSeconds(0.99f));
	CHECK(wa::adsrHoldSeconds(1.f) < 0.f, "the top of the knob holds until the next marker");
	std::printf("  panel default: %.2f s (1)\n", wa::adsrHoldSeconds(0.66667f));
	CHECK(std::fabs(wa::adsrHoldSeconds(0.66667f) - 1.f) < 0.01f, "hold default");

	Voices v;
	for (int m = 0; m < M; m++) {
		v.s[m].hold = 0.5f;
		v.s[m].release = 0.05f;
	}
	auto run = [&](float p, double seconds) {
		for (long i = 0; i < (long) (seconds * SR); i++)
			v.step(p);
	};
	v.step(0.f);
	run(0.f, 0.1);
	run(0.25f, 0.1);
	run(0.5f, 0.1);
	std::printf("  three crossings 100 ms apart, 500 ms notes: levels %.2f %.2f %.2f (all on together)\n", v.v.env[0], v.v.env[1], v.v.env[2]);
	CHECK(v.v.gate[0] && v.v.gate[1] && v.v.gate[2], "reaching a marker must not end the other notes");
	CHECK(v.v.env[0] > 0.99f && v.v.env[1] > 0.99f && v.v.env[2] > 0.99f, "all at full level");
	// Each ends on its own clock: marker 1 at 0.5 s, marker 3 at 0.7 s.
	run(0.5f, 0.21);
	std::printf("  at 510 ms: gates %d %d %d (1 off, 2 and 3 on)\n", (int) v.v.gate[0], (int) v.v.gate[1], (int) v.v.gate[2]);
	CHECK(!v.v.gate[0] && v.v.gate[1] && v.v.gate[2], "marker 1's note ends first");
	run(0.5f, 0.2);
	std::printf("  at 710 ms: gates %d %d %d (all off)\n", (int) v.v.gate[0], (int) v.v.gate[1], (int) v.v.gate[2]);
	CHECK(!v.v.gate[0] && !v.v.gate[1] && !v.v.gate[2], "all ended");
	// A still playhead does not start the note again when it ends.
	int starts = 0;
	for (long i = 0; i < (long) (3 * SR); i++) {
		v.step(0.5f);
		for (int m = 0; m < M; m++)
			starts += v.v.start[m];
	}
	std::printf("  then 3 s at rest: %d new notes (0), level %.3f (silent)\n", starts, v.v.env[2]);
	CHECK(starts == 0 && v.v.env[2] == 0.f, "%d starts, level %.3f", starts, v.v.env[2]);
	// Reaching a marker again restarts its length.
	run(0.3f, 0.1);
	run(0.5f, 0.3);
	run(0.3f, 0.1);
	run(0.5f, 0.3);
	std::printf("  marker 3 reached again after 400 ms: still on 300 ms later: %d (1)\n", (int) v.v.gate[2]);
	CHECK(v.v.gate[2], "the note should restart");
	// Each marker has its own length.
	Voices w;
	w.s[0].hold = 0.1f;
	w.s[1].hold = 2.f;
	w.step(0.f);
	for (long i = 0; i < (long) (0.05 * SR); i++)
		w.step(0.25f);
	for (long i = 0; i < (long) (0.2 * SR); i++)
		w.step(0.25f);
	std::printf("  lengths 100 ms and 2 s: gates %d %d (0 1)\n", (int) w.v.gate[0], (int) w.v.gate[1]);
	CHECK(!w.v.gate[0] && w.v.gate[1], "own lengths");
}

static void testDirectPlay() {
	std::printf("playing markers directly\n");
	const float dt = 1.f / SR;
	Voices v;
	for (int m = 0; m < M; m++) {
		v.s[m].hold = 0.1f;
		v.s[m].release = 0.05f;
	}
	wa::VoiceGates g;
	g.patched[1] = true;
	auto run = [&](float p, double seconds, int* starts = nullptr) {
		for (long i = 0; i < (long) (seconds * SR); i++) {
			v.v.process(v.pos, v.on, p, v.s, dt, &g);
			if (starts)
				for (int m = 0; m < M; m++)
					starts[m] += v.v.start[m];
		}
	};
	run(0.f, 0.3);
	// The playhead crossing a marker with a patched gate does nothing; the next one still plays.
	int c[M] = {};
	for (int k = 0; k <= 100; k++)
		run(k * 0.0049f, 0.01, c); // 0 -> 0.49, over marker 2, up to marker 3
	run(0.5f, 0.01, c);
	std::printf("  playhead over a gated marker 2, on to marker 3: notes %d %d %d (0 0 1), level of marker 2 %.2f\n", c[0], c[1], c[2], v.v.env[1]);
	CHECK(c[1] == 0 && c[2] == 1 && v.v.env[1] == 0.f, "patched marker must ignore the playhead");
	// Gate up: starts at once, and stays well past HOLD.
	g.high[1] = true;
	int d[M] = {};
	run(0.5f, 0.5, d);
	std::printf("  gate high for 500 ms with H 100 ms: %d note, level %.2f, on %d\n", d[1], v.v.env[1], (int) v.v.gate[1]);
	CHECK(d[1] == 1 && v.v.env[1] > 0.99f && v.v.gate[1], "a gate holds past HOLD");
	// Gate down: release follows.
	g.high[1] = false;
	run(0.5f, 0.001);
	CHECK(!v.v.gate[1], "falling gate releases");
	float falling = v.v.env[1];
	run(0.5f, 0.3);
	std::printf("  gate low: level %.2f after 1 ms, %.4f after 300 ms\n", falling, v.v.env[1]);
	CHECK(falling > 0.8f && v.v.env[1] == 0.f, "release: %.2f, %.4f", falling, v.v.env[1]);
	// Two gates overlap.
	g.patched[0] = true;
	g.high[0] = g.high[1] = true;
	run(0.5f, 0.1);
	std::printf("  two gates up: levels %.2f %.2f (both full)\n", v.v.env[0], v.v.env[1]);
	CHECK(v.v.env[0] > 0.99f && v.v.env[1] > 0.99f, "two at once");
	// A new rising edge restarts the note.
	g.high[0] = false;
	run(0.5f, 0.005);
	g.high[0] = true;
	int e[M] = {};
	run(0.5f, 0.01, e);
	CHECK(e[0] == 1, "a second gate is a second note: %d", e[0]);
	g.high[0] = g.high[1] = false;
	g.patched[0] = g.patched[1] = false;
	run(0.5f, 0.3);

	// A press: plays a marker that still follows the playhead afterwards.
	g.high[3] = true;
	int f[M] = {};
	run(0.5f, 0.3, f);
	std::printf("  press on marker 4's slot, 300 ms: %d note, level %.2f\n", f[3], v.v.env[3]);
	CHECK(f[3] == 1 && v.v.env[3] > 0.99f && v.v.gate[3], "press plays");
	g.high[3] = false;
	run(0.5f, 0.001);
	CHECK(!v.v.gate[3], "letting go releases");
	run(0.5f, 0.3);
	int h[M] = {};
	for (int k = 0; k <= 60; k++)
		run(0.5f + k * 0.005f, 0.005, h);
	std::printf("  then the playhead reaches marker 4: %d note (1)\n", h[3]);
	CHECK(h[3] == 1, "it still follows the playhead: %d", h[3]);
	// A marker that is off cannot be played.
	v.on[4] = false;
	g.high[4] = true;
	int i4[M] = {};
	run(0.5f, 0.05, i4);
	CHECK(i4[4] == 0 && v.v.env[4] == 0.f, "a marker that is off");
	g.high[4] = false;
	v.on[4] = true;

	// A gate already high when POLY starts plays its note.
	Voices w;
	wa::VoiceGates hi;
	hi.patched[2] = hi.high[2] = true;
	w.v.process(w.pos, w.on, 0.f, w.s, dt, &hi);
	CHECK(w.v.start[2], "a gate already high starts its note");
	CHECK(w.v.start[0], "the playhead's nearest marker also plays");
}

static void testStep() {
	std::printf("STEP\n");
	const float pos[M] = {0.6f, 0.1f, 0.8f, 0.3f, 0.5f}; // left to right: markers 2, 4, 5, 1, 3
	bool playable[M] = {true, true, true, true, true};
	wa::MarkerStepper st;
	std::vector<int> played;
	double phase = 0.0;
	auto cycles = [&](int n) {
		for (int c = 0; c < n; c++) {
			for (int k = 0; k < 100; k++) {
				st.process(phase, pos, playable);
				for (int m = 0; m < M; m++)
					if (st.fired[m])
						played.push_back(m + 1);
				phase += 0.01;
				phase -= std::floor(phase);
			}
		}
	};
	cycles(7);
	std::printf("  7 cycles: plays");
	for (int p : played)
		std::printf(" %d", p);
	std::printf(" (2 4 5 1 3 2 4: left to right, round again)\n");
	const int want[] = {2, 4, 5, 1, 3, 2, 4};
	CHECK(played.size() == 7, "one marker per cycle: %d", (int) played.size());
	for (int i = 0; i < 7 && i < (int) played.size(); i++)
		CHECK(played[i] == want[i], "step %d: %d, wanted %d", i + 1, played[i], want[i]);
	// Off or muted markers are skipped; RESET starts from the leftmost.
	played.clear();
	playable[3] = false;
	st.reset();
	cycles(4);
	std::printf("  marker 4 off, after RESET: plays %d %d %d %d (2 5 1 3)\n", played[0], played[1], played[2], played[3]);
	CHECK(played.size() == 4 && played[0] == 2 && played[1] == 5 && played[2] == 1 && played[3] == 3, "skip and reset");
	playable[3] = true;
	// A small backward pull of the phase (the clock correcting) is not a step.
	{
		wa::MarkerStepper s2;
		int fires = 0;
		double p[] = {0.40, 0.41, 0.405, 0.42, 0.43};
		for (double x : p) {
			s2.process(x, pos, playable);
			for (int m = 0; m < M; m++)
				fires += s2.fired[m];
		}
		CHECK(fires == 1, "only the first call steps: %d", fires);
	}

	// ALL: every cycle plays every playable marker together.
	{
		wa::MarkerStepper all;
		bool some[M] = {true, false, true, true, true};
		int perCycle[3] = {};
		double ph = 0.0;
		for (int c = 0; c < 3; c++) {
			for (int k = 0; k < 100; k++) {
				all.process(ph, pos, some, true);
				for (int m = 0; m < M; m++)
					perCycle[c] += all.fired[m];
				CHECK(!all.fired[1], "an off marker must not play");
				ph += 0.01;
				ph -= std::floor(ph);
			}
		}
		std::printf("  ALL, marker 2 off: %d %d %d notes per cycle (4 each)\n", perCycle[0], perCycle[1], perCycle[2]);
		CHECK(perCycle[0] == 4 && perCycle[1] == 4 && perCycle[2] == 4, "every marker each cycle");
	}

	// The voices in STEP: H at the top keeps every marker on until its own next turn.
	{
		Voices v;
		bool steps[M] = {};
		auto stepTo = [&](int marker) {
			for (int m = 0; m < M; m++)
				steps[m] = m == marker;
			v.v.process(v.pos, v.on, 0.f, v.s, 1.f / SR, nullptr, steps);
			for (int m = 0; m < M; m++)
				steps[m] = false;
			for (int i = 0; i < 4800; i++)
				v.v.process(v.pos, v.on, 0.f, v.s, 1.f / SR, nullptr, steps);
		};
		for (int m = 0; m < M; m++)
			stepTo(m);
		std::printf("  H at the top, after 5 steps: levels %.2f %.2f %.2f %.2f %.2f (all on)\n", v.v.env[0], v.v.env[1], v.v.env[2], v.v.env[3], v.v.env[4]);
		for (int m = 0; m < M; m++)
			CHECK(v.v.gate[m] && v.v.env[m] > 0.99f, "marker %d should still sound", m + 1);
		// Its turn again: a new attack on a sounding voice.
		for (int m = 0; m < M; m++)
			steps[m] = m == 0;
		v.v.process(v.pos, v.on, 0.f, v.s, 1.f / SR, nullptr, steps);
		CHECK(v.v.start[0], "struck again on its turn");
		// The playhead does nothing in STEP.
		for (int m = 0; m < M; m++)
			steps[m] = false;
		int starts = 0;
		for (int i = 0; i < 48000; i++) {
			v.v.process(v.pos, v.on, (float) i / 48000.f, v.s, 1.f / SR, nullptr, steps);
			for (int m = 0; m < M; m++)
				starts += v.v.start[m];
		}
		CHECK(starts == 0, "the playhead must not play notes in STEP: %d", starts);
	}
	{
		// A timed H ends on its own: a melody.
		Voices v;
		for (int m = 0; m < M; m++)
			v.s[m].hold = 0.1f;
		bool steps[M] = {};
		steps[1] = true;
		v.v.process(v.pos, v.on, 0.f, v.s, 1.f / SR, nullptr, steps);
		steps[1] = false;
		for (int i = 0; i < 9600; i++)
			v.v.process(v.pos, v.on, 0.f, v.s, 1.f / SR, nullptr, steps);
		std::printf("  H 100 ms: marker 2 on after 200 ms %d (0)\n", (int) v.v.gate[1]);
		CHECK(!v.v.gate[1], "a timed note ends");
	}
}

/** Poly wavetable engine with the module's wiring */
struct WaveRig {
	std::unique_ptr<wa::MorphEngine> engine{new wa::MorphEngine};
	std::unique_ptr<wa::WavetableBank> bank = makeBank();
	wa::EngineInput in;
	Voices v;
	float position = 0.f;
	WaveRig() {
		engine->setSampleRate(SR);
		in.bank = bank.get();
		in.poly = true;
		in.masterMode = -1;
		in.masterDrive = 0.f;
		for (int m = 0; m < M; m++) {
			in.markers[m].active = true;
			in.markers[m].pos = v.pos[m];
		}
	}
	void run(double seconds, std::vector<float>* l = nullptr, std::vector<float>* r = nullptr) {
		long n = (long) (seconds * SR);
		for (long i = 0; i < n; i++) {
			if (in.poly) {
				v.step(position);
				for (int m = 0; m < M; m++) {
					in.voiceEnv[m] = v.v.env[m];
					in.voiceStart[m] = v.v.start[m];
				}
			}
			in.position = position;
			float a, b;
			engine->process(in, &a, &b);
			if (l)
				l->push_back(a);
			if (r)
				r->push_back(b);
		}
	}
};

struct GrainRig {
	std::unique_ptr<wa::GrainEngine> engine{new wa::GrainEngine};
	std::unique_ptr<wa::WavetableBank> bank = makeBank();
	wa::ClockTracker clock;
	wa::GrainInput in;
	Voices v;
	float position = 0.f;
	GrainRig() {
		engine->setSampleRate(SR);
		in.bank = bank.get();
		in.clock = &clock;
		in.poly = true;
		in.masterMode = -1;
		in.masterDrive = 0.f;
		for (int m = 0; m < M; m++) {
			in.markers[m].active = true;
			in.markers[m].pos = v.pos[m];
			in.markers[m].start = 0.1f + 0.15f * m;
			in.markers[m].density = densityKnob(10.f);
			in.markers[m].size = sizeKnob(0.378f);
		}
	}
	void run(double seconds, std::vector<float>* l = nullptr, std::vector<float>* r = nullptr) {
		long n = (long) (seconds * SR);
		for (long i = 0; i < n; i++) {
			v.step(position);
			for (int m = 0; m < M; m++) {
				in.voiceEnv[m] = v.v.env[m];
				in.voiceStart[m] = v.v.start[m];
			}
			in.position = position;
			clock.processInternal(120.f, SR);
			float a, b;
			engine->process(in, &a, &b);
			if (l)
				l->push_back(a);
			if (r)
				r->push_back(b);
		}
	}
};

static void testWavePoly() {
	std::printf("wavetable voices\n");
	const double f1 = 261.6256, f2 = 261.6256 * std::pow(2.0, 7.0 / 12.0);
	// Overlap: after the crossing the old note rings through its release under the new one.
	{
		WaveRig r;
		r.in.markers[0].pitch = 0.f;
		r.in.markers[1].pitch = 7.f;
		for (int m = 0; m < M; m++)
			r.v.s[m].release = 1.f;
		r.position = 0.f;
		r.run(0.5);
		std::vector<float> l;
		r.position = 0.25f;
		r.run(0.1);
		r.run(0.2, &l);
		double a = tone(l, 0, l.size(), f1), b = tone(l, 0, l.size(), f2);
		std::printf("  100..300 ms after crossing, 1 s release: old note %.3f, new note %.3f (both present)\n", a, b);
		CHECK(a > 0.2 && b > 0.8, "overlap: %.3f, %.3f", a, b);
		// Then the old one is gone.
		r.run(2.0);
		std::vector<float> t;
		r.run(0.2, &t);
		double a2 = toneClean(t, 0, t.size(), f1), b2 = toneClean(t, 0, t.size(), f2);
		std::printf("  2 s later: old note %.5f (under 0.001), new note %.3f\n", a2, b2);
		CHECK(a2 < 0.001 && b2 > 0.8, "after release: %.5f, %.3f", a2, b2);
	}
	// HOLD: two notes at full level together, not just a tail.
	{
		WaveRig r;
		r.in.markers[1].pitch = 7.f;
		for (int m = 0; m < M; m++)
			r.v.s[m].hold = 1.f;
		r.run(0.2);
		r.position = 0.25f;
		r.run(0.1);
		std::vector<float> l;
		r.run(0.2, &l);
		double a = toneClean(l, 0, l.size(), f1), b = toneClean(l, 0, l.size(), f2);
		std::printf("  1 s notes: after the crossing, old note %.3f, new note %.3f (both full, 1.0)\n", a, b);
		CHECK(a > 0.97 && b > 0.97, "both notes: %.3f, %.3f", a, b);
	}
	// Independent settings: a held marker keeps its own filter and pan, with no interpolation.
	{
		WaveRig r;
		r.in.markers[0].pan = -1.f;
		r.in.markers[1].pan = 1.f;
		r.in.markers[1].cutoff = 0.f;
		r.position = 0.12f; // nearest is marker 1; in the morph this would be half way to marker 2
		r.run(0.5);
		std::vector<float> a, b;
		r.run(0.2, &a, &b);
		double left = rms(a, 0, a.size()), right = rms(b, 0, b.size());
		std::printf("  between markers, gate on 1: L %.3f, R %.5f (hard left, unaffected by marker 2's pan and cutoff)\n", left, right);
		CHECK(left > 0.5 && right < 0.001, "pan L %.3f R %.5f", left, right);
	}
	// Same level as the morph on a marker.
	{
		WaveRig p;
		p.run(0.5);
		std::vector<float> a;
		p.run(0.5, &a);
		WaveRig m;
		m.in.poly = false;
		m.position = 0.f;
		m.run(0.5);
		std::vector<float> b;
		m.run(0.5, &b);
		double pl = rms(a, 0, a.size()), ml = rms(b, 0, b.size());
		double db = 20.0 * std::log10(pl / ml);
		std::printf("  one voice against the morph: %.3f / %.3f = %+.2f dB (limit +-0.1)\n", pl, ml, db);
		CHECK(std::fabs(db) < 0.1, "%+.2f dB", db);
	}
	// A plucked note really falls to its sustain, from the same engine run.
	{
		WaveRig r;
		for (int m = 0; m < M; m++) {
			r.v.s[m].sustain = 0.f;
			r.v.s[m].decay = 0.1f;
		}
		std::vector<float> l;
		r.run(0.5, &l);
		double start = rms(l, 240, 960), later = rms(l, 12000, 24000);
		std::printf("  S 0, D 100 ms: first 20 ms %.3f, at 250..500 ms %.6f (silent)\n", start, later);
		CHECK(start > 0.3 && later < 1e-3, "pluck %.3f, %.6f", start, later);
	}
	// Starting and stopping notes makes no click: a low pure tone shows any step.
	{
		WaveRig r;
		for (int m = 0; m < M; m++) {
			r.in.markers[m].pitch = -24.f;
			r.v.s[m].attack = 0.002f;
			r.v.s[m].release = 0.02f;
		}
		std::vector<float> l;
		r.run(0.2, &l);
		for (int k = 0; k < 16; k++) {
			r.position = (k & 1) ? 0.f : 1.f; // jumps
			r.run(0.08, &l);
		}
		double step = maxStep(l, 1);
		double slope = 2 * PI * 65.4 / SR;
		// Five limit: tone slope x level plus the attack's slope x level at most
		double bound = 2.0 * (slope + 1.0 / (0.002 * SR));
		std::printf("  notes starting every 80 ms on a 65 Hz tone: biggest step %.4f (limit %.4f)\n", step, bound);
		CHECK(step <= bound, "click: %.4f above %.4f", step, bound);
	}
	// Switching between modes while playing fades.
	{
		WaveRig r;
		for (int m = 0; m < M; m++)
			r.in.markers[m].pitch = -24.f;
		r.run(0.3);
		std::vector<float> l;
		r.run(0.1, &l);
		r.in.poly = false;
		r.run(0.1, &l);
		r.in.poly = true;
		r.v.v.reset();
		r.run(0.1, &l);
		double step = maxStep(l);
		std::printf("  POLY -> MORPH -> POLY while playing: biggest step %.4f (limit 0.02)\n", step);
		CHECK(step < 0.02, "mode switch step %.4f", step);
	}
	// Artifact outputs carry a voice and take it out of the main mix.
	{
		WaveRig r;
		r.in.wantDirect[0] = true;
		r.run(0.5);
		std::vector<float> l;
		long n = (long) (0.2 * SR);
		double direct = 0.0;
		for (long i = 0; i < n; i++) {
			r.run(1.0 / SR, &l);
			float a, b;
			r.engine->getDirect(0, &a, &b);
			direct += (double) a * a;
		}
		double main = rms(l, 0, l.size());
		std::printf("  marker 1's output patched: its jack %.3f, main mix %.5f\n", std::sqrt(direct / n), main);
		CHECK(std::sqrt(direct / n) > 0.3 && main < 1e-3, "direct");
	}
}

static void testGrainPoly() {
	std::printf("granular voices\n");
	// Attack is heard at once, whatever the grain. Reference: the level of the running cloud. Marker 1 has no
	// gain here, so its tail cannot pass for the new note's start.
	struct Case {
		const char* name;
		bool sync;
		float density, size;
		int window;
		double riseLimitMs;
	};
	const Case cases[] = {
		{"378 ms, 10 grains/s", false, densityKnob(10.f), sizeKnob(0.378f), wa::GRAIN_HANN, 25.0},
		{"378 ms, 2 grains/s (sparse)", false, densityKnob(2.f), sizeKnob(0.378f), wa::GRAIN_HANN, 25.0},
		{"synced, 1/4 note grid", true, 0.3f, 0.5f, wa::GRAIN_HANN, 40.0},
		{"Smear, 8 grains/s", false, densityKnob(8.f), sizeKnob(0.375f), wa::GRAIN_SMEAR, 60.0},
	};
	for (const Case& c : cases) {
		GrainRig r;
		for (int m = 0; m < M; m++) {
			r.in.markers[m].sync = c.sync;
			r.in.markers[m].density = c.density;
			r.in.markers[m].size = c.size;
			r.in.markers[m].window = c.window;
			r.v.s[m].attack = 0.002f;
			r.v.s[m].release = 0.02f;
		}
		r.in.markers[0].gain = 0.f;
		r.position = 0.f;
		r.run(1.0);
		// Marker 2 silent until now.
		r.position = 0.25f;
		std::vector<float> l;
		r.run(1.5, &l);
		double steady = rms(l, (size_t) (0.5 * SR), l.size());
		// First 10 ms frame reaching half the steady level (noise source, so a whole-frame RMS)
		double rise = -1.0;
		const size_t frame = (size_t) (0.01 * SR);
		for (size_t i = 0; i + frame < l.size(); i += frame / 2) {
			if (rms(l, i, i + frame) > 0.5 * steady) {
				rise = i * 1000.0 / SR;
				break;
			}
		}
		std::printf("  %-28s half level after %5.1f ms (limit %.0f), steady %.3f\n", c.name, rise, c.riseLimitMs, steady);
		CHECK(rise >= 0.0 && rise <= c.riseLimitMs, "%s: rise %.1f ms", c.name, rise);
	}
	// A note from silence does not start with a burst or a dip against the running cloud.
	{
		GrainRig r;
		for (int m = 0; m < M; m++)
			r.v.s[m].release = 0.01f;
		r.position = 0.f;
		r.run(1.0);
		r.position = 0.25f;
		std::vector<float> l;
		r.run(1.5, &l);
		double steady = rms(l, (size_t) (0.8 * SR), l.size());
		double first = rms(l, (size_t) (0.02 * SR), (size_t) (0.2 * SR));
		double db = 20.0 * std::log10(first / steady);
		std::printf("  20..200 ms against the running cloud: %+.2f dB (limit +-2.5)\n", db);
		CHECK(std::fabs(db) < 2.5, "%+.2f dB", db);
	}
	// Old note's grains do not leak into the next one, and silence is silence.
	{
		GrainRig r;
		for (int m = 0; m < M; m++) {
			r.v.s[m].sustain = 0.f;
			r.v.s[m].decay = 0.05f;
		}
		std::vector<float> l;
		r.run(1.0, &l);
		double later = rms(l, (size_t) (0.5 * SR), l.size());
		std::printf("  S 0, D 50 ms, at 500..1000 ms: %.6f (under 0.0001), grains running %d\n", later, r.engine->getActiveGrains(0));
		CHECK(later < 1e-4, "cloud should be silent: %.6f", later);
	}
	// A note from silence starts clean: the last note's grains are gone, not sounding under the new one.
	{
		GrainRig r;
		for (int m = 0; m < M; m++)
			r.v.s[m].release = 0.005f;
		r.run(0.1);
		r.position = 0.25f;
		r.run(0.03); // marker 1 is silent by now, with long grains still running
		int left = r.engine->getActiveGrains(0);
		r.position = 0.f;
		r.run(2.0 / SR);
		int now = r.engine->getActiveGrains(0);
		int expected = (int) std::ceil(10.f * 0.378f);
		std::printf("  marker 1 again 30 ms after its note ended: %d grains (a new note's %d; %d were left over)\n", now, expected, left);
		CHECK(now == expected, "%d grains, expected %d", now, expected);
	}
	// Level equals the morph on a marker.
	{
		GrainRig p;
		p.run(1.0);
		std::vector<float> a;
		p.run(1.5, &a);
		GrainRig m;
		m.in.poly = false;
		m.run(1.0);
		std::vector<float> b;
		m.run(1.5, &b);
		double db = 20.0 * std::log10(rms(a, 0, a.size()) / rms(b, 0, b.size()));
		std::printf("  one cloud against the morph: %+.2f dB (limit +-1, noise source)\n", db);
		CHECK(std::fabs(db) < 1.0, "%+.2f dB", db);
	}
	// Real overlap in the engines: two timed notes both at full level.
	{
		GrainRig r;
		for (int m = 0; m < M; m++)
			r.v.s[m].hold = 2.f;
		r.run(1.0);
		std::vector<float> a;
		r.run(0.3, &a);
		r.position = 0.25f;
		std::vector<float> b;
		r.run(0.1);
		r.run(0.3, &b);
		double ratio = rms(b, 0, b.size()) / rms(a, 0, a.size());
		std::printf("  second timed note over the first: level x%.2f (about 1.41, two equal uncorrelated clouds)\n", ratio);
		CHECK(ratio > 1.3 && ratio < 1.55, "ratio %.2f", ratio);
	}
	// Two clouds at once: overlapping notes at different read positions are both heard.
	{
		GrainRig r;
		for (int m = 0; m < M; m++)
			r.v.s[m].release = 10.f;
		r.run(1.0);
		std::vector<float> a;
		r.run(0.3, &a);
		r.position = 0.25f;
		std::vector<float> b;
		r.run(0.1);
		r.run(0.3, &b);
		double ratio = rms(b, 0, b.size()) / rms(a, 0, a.size());
		std::printf("  second note over a 10 s release: level x%.2f (about 1.3: two uncorrelated clouds, the first falling)\n", ratio);
		CHECK(ratio > 1.15 && ratio < 1.5, "ratio %.2f", ratio);
	}
}

static void testGrainPan() {
	std::printf("granular PAN\n");
	auto render = [](float pan, float spread, std::vector<float>* l, std::vector<float>* r) {
		GrainRig g;
		g.in.markers[0].pan = pan;
		g.in.markers[0].spread = spread;
		g.run(0.5);
		g.run(1.5, l, r);
	};
	std::vector<float> cl, cr, ll, lr, rl, rr, ql, qr;
	render(0.f, 0.f, &cl, &cr);
	render(-1.f, 0.f, &ll, &lr);
	render(1.f, 0.f, &rl, &rr);
	render(0.5f, 0.f, &ql, &qr);
	double center = rms(cl, 0, cl.size());
	double centerSide = 0.0;
	for (size_t i = 0; i < cl.size(); i++)
		centerSide = std::max(centerSide, (double) std::fabs(cl[i] - cr[i]));
	std::printf("  center: L %.3f, R %.3f, biggest L-R difference %.6f\n", center, rms(cr, 0, cr.size()), centerSide);
	CHECK(centerSide < 1e-5, "center must be identical in L and R: %.6f", centerSide);
	double hardL = rms(ll, 0, ll.size()), hardLR = rms(lr, 0, lr.size());
	double hardR = rms(rr, 0, rr.size()), hardRL = rms(rl, 0, rl.size());
	std::printf("  hard left: L %.3f, R %.6f; hard right: L %.6f, R %.3f\n", hardL, hardLR, hardRL, hardR);
	CHECK(hardLR < 1e-4 && hardL > center * 1.3, "hard left: %.3f, %.6f", hardL, hardLR);
	CHECK(hardRL < 1e-4 && hardR > center * 1.3, "hard right: %.6f, %.3f", hardRL, hardR);
	// Equal power: the total does not change as it moves.
	auto power = [](const std::vector<float>& a, const std::vector<float>& b) {
		double x = rms(a, 0, a.size()), y = rms(b, 0, b.size());
		return x * x + y * y;
	};
	double db1 = 10.0 * std::log10(power(ll, lr) / power(cl, cr)), db2 = 10.0 * std::log10(power(ql, qr) / power(cl, cr));
	std::printf("  total power against center: hard left %+.2f dB, half right %+.2f dB (limit +-0.1), half right L %.3f < R %.3f\n", db1, db2, rms(ql, 0, ql.size()), rms(qr, 0, qr.size()));
	CHECK(std::fabs(db1) < 0.1 && std::fabs(db2) < 0.1, "equal power: %+.2f, %+.2f", db1, db2);
	CHECK(rms(ql, 0, ql.size()) < rms(qr, 0, qr.size()) * 0.6, "half right");
	// SPREAD still scatters around a part-way PAN, and the cloud leans that way.
	std::vector<float> sl, sr;
	render(0.5f, 1.f, &sl, &sr);
	double leftS = rms(sl, 0, sl.size()), rightS = rms(sr, 0, sr.size());
	double diff = 0.0;
	for (size_t i = 0; i < sl.size(); i++)
		diff += std::fabs(sl[i] * rightS - sr[i] * leftS);
	diff /= sl.size() * std::max(leftS * rightS, 1e-9);
	double diffPlain = 0.0;
	double l0 = rms(ql, 0, ql.size()), r0 = rms(qr, 0, qr.size());
	for (size_t i = 0; i < ql.size(); i++)
		diffPlain += std::fabs(ql[i] * r0 - qr[i] * l0);
	diffPlain /= ql.size() * std::max(l0 * r0, 1e-9);
	std::printf("  PAN +0.5 with SPREAD: L %.3f, R %.3f; L and R differ by %.3f (without SPREAD %.5f)\n", leftS, rightS, diff, diffPlain);
	CHECK(rightS > leftS && diff > 0.2 && diffPlain < 0.01, "spread around pan");
	// SPREAD at hard left cannot reach the right side.
	std::vector<float> hl, hr;
	render(-1.f, 1.f, &hl, &hr);
	std::printf("  hard left with SPREAD: L %.3f, R %.6f (the right side is off)\n", rms(hl, 0, hl.size()), rms(hr, 0, hr.size()));
	CHECK(rms(hr, 0, hr.size()) < 1e-4, "hard left with spread");

	// Moving the knob slides over a few ms instead of jumping.
	{
		GrainRig g;
		g.in.markers[0].pan = -1.f;
		g.run(1.0);
		g.in.markers[0].pan = 1.f;
		std::vector<float> l, r;
		g.run(0.1, &l, &r);
		// Measured from where R first appears, so the limiter's look-ahead does not count as a slide.
		size_t first = 0;
		while (first < r.size() && std::fabs(r[first]) < 1e-3f)
			first++;
		double justAfter = rms(r, first, first + 24), later = rms(r, (size_t) (0.05 * SR), r.size());
		double middle = rms(r, first + (size_t) (0.003 * SR), first + (size_t) (0.006 * SR));
		std::printf("  hard left to hard right: from where R appears (%.2f ms), %.3f in the first 0.5 ms, %.3f at 3..6 ms, %.3f once settled\n", first * 1000.0 / SR, justAfter, middle, later);
		CHECK(justAfter < later * 0.2, "pan must not jump: %.3f against %.3f", justAfter, later);
		CHECK(middle > later * 0.3, "pan must arrive within a few ms: %.3f against %.3f", middle, later);
	}
	// A new note begins where its marker is panned, not with a slide from the last place.
	{
		// Marker 2 has no gain here: its tail (and the limiter's DC filter settling after it) would otherwise read
		// as signal on the right.
		GrainRig g;
		g.position = 0.25f;
		g.in.markers[0].pan = 1.f;
		g.in.markers[1].gain = 0.f;
		g.run(0.5);
		g.in.markers[0].pan = -1.f;
		g.position = 0.f;
		std::vector<float> l, r;
		g.run(0.05, &l, &r);
		double late = rms(r, (size_t) (0.03 * SR), r.size());
		std::printf("  PAN to hard left on the sample the note starts: R %.6f from 30 ms on, L %.3f\n", late, rms(l, (size_t) (0.03 * SR), l.size()));
		CHECK(late < 1e-4, "right side after a hard-left note: %.6f", late);
	}
	// The same, with the knob moved while the marker is silent, long before its note.
	{
		// In MORPH nothing resets it at a note start, so this is the path for a cloud fading in.
		GrainRig g;
		// Parked well away: right next to it, the smoothed playhead never quite arrives and the cloud never idles.
		g.position = 0.6f;
		g.in.markers[0].pan = 1.f;
		for (int m = 1; m < M; m++)
			g.in.markers[m].gain = 0.f;
		g.in.poly = false;
		auto step = [&](double seconds, std::vector<float>* l, std::vector<float>* r) {
			for (long i = 0; i < (long) (seconds * SR); i++) {
				g.in.position = g.position;
				g.clock.processInternal(120.f, SR);
				float a, b;
				g.engine->process(g.in, &a, &b);
				if (l)
					l->push_back(a);
				if (r)
					r->push_back(b);
			}
		};
		// Long enough for the cloud to go fully idle (its last grains, then 0.5 s of filter ring-out).
		step(1.5, nullptr, nullptr);
		g.in.markers[0].pan = -1.f;
		step(0.1, nullptr, nullptr);
		g.position = 0.f;
		std::vector<float> l, r;
		step(0.06, &l, &r);
		double late = rms(r, (size_t) (0.03 * SR), r.size());
		std::printf("  MORPH, PAN moved while silent, playhead arrives: R %.6f, L %.3f\n", late, rms(l, (size_t) (0.03 * SR), l.size()));
		// The first grain of a long window is still opening here, so L is small; R should be nothing at all.
		CHECK(late < 1e-5 && rms(l, (size_t) (0.03 * SR), l.size()) > 0.01, "a cloud fading in: %.6f", late);
	}
	// Each POLY voice has its own place: two notes on opposite sides.
	{
		GrainRig g;
		g.in.markers[0].pan = -1.f;
		g.in.markers[1].pan = 1.f;
		for (int m = 0; m < M; m++)
			g.v.s[m].hold = 2.f;
		g.run(0.5);
		std::vector<float> l0v, r0v;
		g.run(0.2, &l0v, &r0v);
		g.position = 0.25f;
		g.run(0.05);
		std::vector<float> l1, r1;
		g.run(0.2, &l1, &r1);
		std::printf("  marker 1 alone: L %.3f, R %.5f; then marker 2 too: L %.3f, R %.3f\n", rms(l0v, 0, l0v.size()), rms(r0v, 0, r0v.size()), rms(l1, 0, l1.size()), rms(r1, 0, r1.size()));
		CHECK(rms(r0v, 0, r0v.size()) < 1e-4 && rms(l1, 0, l1.size()) > 0.3 && rms(r1, 0, r1.size()) > 0.3, "two places");
	}
	// An artifact output carries the pan too.
	{
		GrainRig g;
		g.in.markers[0].pan = -1.f;
		g.in.wantDirect[0] = true;
		g.run(1.0);
		double dl = 0.0, dr = 0.0;
		long n = (long) (0.3 * SR);
		for (long i = 0; i < n; i++) {
			g.run(1.0 / SR);
			float a, b;
			g.engine->getDirect(0, &a, &b);
			dl += (double) a * a;
			dr += (double) b * b;
		}
		std::printf("  marker 1's own output, hard left: L %.3f, R %.6f\n", std::sqrt(dl / n), std::sqrt(dr / n));
		CHECK(std::sqrt(dl / n) > 0.3 && std::sqrt(dr / n) < 1e-4, "direct output");
	}
}

static void testLoudAndCost() {
	std::printf("five voices: level and CPU\n");
	// Five held notes, all at once (stacked at one place).
	{
		WaveRig r;
		for (int m = 0; m < M; m++)
			r.v.pos[m] = 0.5f;
		r.position = 0.5f;
		r.run(1.0);
		std::vector<float> l;
		r.run(0.5, &l);
		std::printf("  wave, 5 same-pitch notes: peak %.3f, limiter gain %.3f (%.1f dB) (peak limit 2.0)\n", peakOf(l), r.engine->getLimiterGain(), 20.0 * std::log10(r.engine->getLimiterGain()));
		CHECK(peakOf(l) <= 2.0, "peak %.3f", peakOf(l));
	}
	{
		WaveRig r;
		const float chord[M] = {0.f, 4.f, 7.f, 11.f, 14.f};
		for (int m = 0; m < M; m++) {
			r.v.pos[m] = 0.5f;
			r.in.markers[m].pitch = chord[m];
		}
		r.position = 0.5f;
		r.run(1.0);
		std::vector<float> l;
		r.run(0.5, &l);
		double limiting = 20.0 * std::log10(r.engine->getLimiterGain());
		std::printf("  wave, 5-note chord: rms %.3f, peak %.3f, limiter %.1f dB (limit -6)\n", rms(l, 0, l.size()), peakOf(l), limiting);
		CHECK(peakOf(l) <= 2.0, "peak %.3f", peakOf(l));
		CHECK(limiting > -6.0, "limiter %.1f dB", limiting);
	}
	{
		GrainRig r;
		for (int m = 0; m < M; m++)
			r.v.pos[m] = 0.5f;
		r.position = 0.5f;
		r.run(1.0);
		std::vector<float> l;
		r.run(0.5, &l);
		std::printf("  grain, 5 clouds: rms %.3f, peak %.3f, limiter gain %.3f (%.1f dB)\n", rms(l, 0, l.size()), peakOf(l), r.engine->getLimiterGain(), 20.0 * std::log10(r.engine->getLimiterGain()));
		CHECK(peakOf(l) <= 2.0, "peak %.3f", peakOf(l));
	}
	// Two voices keep full level: the trim only starts after that.
	{
		WaveRig r;
		const float pitches[M] = {0.f, 7.f, 0.f, 0.f, 0.f};
		for (int m = 0; m < M; m++) {
			r.in.markers[m].pitch = pitches[m];
			r.v.s[m].release = 10.f;
		}
		r.run(0.5);
		r.position = 0.25f;
		r.run(0.05);
		std::vector<float> l;
		r.run(0.1, &l);
		double fifth = toneClean(l, 0, l.size(), 261.6256 * std::pow(2.0, 7.0 / 12.0));
		std::printf("  two voices: the new note reads %.3f (full level 1.0, limit 0.97)\n", fifth);
		CHECK(fifth > 0.97, "two voices trimmed: %.3f", fifth);
	}
	// CPU: all five chains with drive on.
	{
		WaveRig r;
		for (int m = 0; m < M; m++) {
			r.v.pos[m] = 0.5f;
			r.in.markers[m].drive = 0.5f;
			r.in.markers[m].cutoff = 0.7f;
		}
		r.position = 0.5f;
		r.run(0.2);
		auto t0 = std::chrono::steady_clock::now();
		r.run(2.0);
		double us5 = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (2.0 * SR);

		WaveRig q;
		q.in.markers[0].drive = 0.5f;
		q.run(0.2);
		t0 = std::chrono::steady_clock::now();
		q.run(2.0);
		double us1 = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (2.0 * SR);
		std::printf("  wave CPU: 5 voices with drive %.2f us per sample, 1 voice %.2f us (budget at 48 kHz 20.8; limit 4)\n", us5, us1);
		CHECK(us5 < 4.0, "%.2f us", us5);
		CHECK(us1 < us5 * 0.8, "idle voices must cost little: %.2f against %.2f", us1, us5);
	}
}

int main() {
	testAdsr();
	testTrigger();
	testHold();
	testDirectPlay();
	testStep();
	testWavePoly();
	testGrainPoly();
	testGrainPan();
	testLoudAndCost();
	if (failures) {
		std::printf("%d FAILED\n", failures);
		return 1;
	}
	std::printf("all checks passed\n");
	return 0;
}
