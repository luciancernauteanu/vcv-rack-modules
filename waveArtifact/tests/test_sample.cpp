// Offline tests for the SAMPLE engine (SamplePlayer.hpp, played through GrainEngine with GrainInput::player).
// Build and run: cd tests && make run
//
// Every check prints the measured value next to its threshold.

#include "GrainEngine.hpp"

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
static const unsigned SRC_SR = 44100;

static std::unique_ptr<wa::WavetableBank> makeBank(const std::vector<float>& src, unsigned channels = 1) {
	std::unique_ptr<wa::WavetableBank> bank(new wa::WavetableBank);
	wa::buildSampleBuffer(src.data(), src.size() / channels, channels, SRC_SR, bank->sample);
	return bank;
}

static std::vector<float> sineSource(double seconds, double hz, float amp = 0.5f) {
	std::vector<float> v((size_t) (seconds * SRC_SR));
	for (size_t i = 0; i < v.size(); i++)
		v[i] = amp * (float) std::sin(2 * PI * hz * i / SRC_SR);
	return v;
}

static std::vector<float> noiseSource(double seconds) {
	std::vector<float> v((size_t) (seconds * SRC_SR));
	uint32_t s = 12345u;
	for (float& x : v) {
		s = s * 1664525u + 1013904223u;
		x = ((s >> 8) * (1.f / 8388608.f) - 1.f) * 0.5f;
	}
	return v;
}

/** Goertzel amplitude at hz */
static double tone(const std::vector<float>& v, size_t from, size_t to, double hz, double sr) {
	double w = 2 * PI * hz / sr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
	for (size_t i = from; i < to; i++) {
		double s = v[i] + c * s1 - s2;
		s2 = s1;
		s1 = s;
	}
	double p = s1 * s1 + s2 * s2 - c * s1 * s2;
	return 2.0 * std::sqrt(std::max(p, 0.0)) / (double) (to - from);
}

static double rms(const std::vector<float>& v, size_t from, size_t to) {
	double s = 0.0;
	to = std::min(to, v.size());
	for (size_t i = from; i < to; i++)
		s += (double) v[i] * v[i];
	return std::sqrt(s / std::max<size_t>(to - from, 1));
}

static double maxStep(const std::vector<float>& v, size_t from, size_t to) {
	double m = 0.0;
	to = std::min(to, v.size());
	for (size_t i = std::max<size_t>(from, 1); i < to; i++)
		m = std::max(m, (double) std::fabs(v[i] - v[i - 1]));
	return m;
}

/** The engine on SAMPLE, marker 0 alone, recording the cloud's own output (before the master section). */
struct Rig {
	std::unique_ptr<wa::GrainEngine> engine{new wa::GrainEngine};
	std::unique_ptr<wa::WavetableBank> bank;
	wa::ClockTracker clock;
	wa::GrainInput in;
	float sr;

	Rig(std::unique_ptr<wa::WavetableBank> b, float sampleRate = 48000.f, bool poly = true) : bank(std::move(b)), sr(sampleRate) {
		engine->setSampleRate(sr);
		in.bank = bank.get();
		in.clock = &clock;
		in.player = true;
		in.poly = poly;
		in.masterMode = -1;
		in.masterDrive = 0.f;
		in.wantDirect[0] = true;
		in.markers[0].active = true;
		in.markers[0].start = 0.f;
		in.markers[0].scanFade = 0.f;
	}

	void step(float* l, float* r) {
		clock.processInternal(120.f, sr);
		engine->process(in, l, r);
		engine->getDirect(0, l, r);
		in.voiceStart[0] = false;
		in.reset = false;
	}

	void run(double seconds, std::vector<float>* out = nullptr, std::vector<float>* outR = nullptr) {
		long n = (long) (seconds * sr);
		for (long i = 0; i < n; i++) {
			float a, b;
			step(&a, &b);
			if (out)
				out->push_back(a);
			if (outR)
				outR->push_back(b);
		}
	}

	/** A POLY note (level 1) from this sample on */
	void note() {
		in.voiceEnv[0] = 1.f;
		in.voiceStart[0] = true;
	}
};

static void testVoiceExact() {
	std::printf("player voice reads the sample as it is\n");
	auto bank = makeBank(noiseSource(1.0));
	const wa::SampleBuffer& S = bank->sample;
	for (int dir : {1, -1}) {
		wa::PlayerVoice v;
		wa::PlayerSettings p;
		p.start = 1000.0;
		p.end = 30000.0;
		p.dir = dir;
		p.rate = 1.0;
		p.loop = wa::PLAYER_ONE;
		v.start(p, false, 240.f);
		double worst = 0.0;
		long n = 20000;
		for (long i = 0; i < n; i++) {
			float l, r;
			v.process(S, p, &l, &r);
			long at = dir > 0 ? 1000 + i : 30000 - i;
			worst = std::max(worst, (double) std::fabs(l - S.levels[0][(size_t) at]));
		}
		std::printf("  %s, rate 1: largest difference from the sample's frames %.1e (limit 1e-7)\n", dir > 0 ? "forward" : "reverse", worst);
		CHECK(worst < 1e-7, "%s differs by %.2e", dir > 0 ? "forward" : "reverse", worst);
	}
	// Stereo stays stereo
	{
		std::vector<float> st(2 * 20000);
		for (size_t i = 0; i < 20000; i++) {
			st[2 * i] = 0.5f * (float) std::sin(2 * PI * 440.0 * i / SRC_SR);
			st[2 * i + 1] = 0.25f * (float) std::sin(2 * PI * 660.0 * i / SRC_SR);
		}
		auto sb = makeBank(st, 2);
		wa::PlayerVoice v;
		wa::PlayerSettings p;
		p.end = 19000.0;
		p.loop = wa::PLAYER_ONE;
		v.start(p, false, 240.f);
		double wl = 0.0, wr = 0.0;
		for (long i = 0; i < 10000; i++) {
			float l, r;
			v.process(sb->sample, p, &l, &r);
			wl = std::max(wl, (double) std::fabs(l - st[2 * i]));
			wr = std::max(wr, (double) std::fabs(r - st[2 * i + 1]));
		}
		std::printf("  stereo: left %.1e, right %.1e from the source (limit 1e-7)\n", wl, wr);
		CHECK(wl < 1e-7 && wr < 1e-7, "stereo differs: %.2e / %.2e", wl, wr);
	}
}

static void testEngineExact() {
	std::printf("through the engine (0 st, same sample rate)\n");
	// The cloud's bus (an open LP is no filter on SAMPLE, DRIVE 0 is the clean path, gain 1, center) and the output's look-ahead limiter and 4 Hz DC blocker are all
	// that is left: the waveform is the sample's, times its loudness gain, 1.5 ms later.
	auto src = sineSource(2.0, 997.0, 0.3f);
	Rig rig(makeBank(src), (float) SRC_SR);
	rig.run(0.05);
	rig.note();
	std::vector<float> out;
	rig.run(1.0, &out);
	float g = rig.bank->sample.normGain;
	// The look-ahead limiter (1.5 ms) and the clean path's match of the DRIVE latency (31 samples)
	int lat = (int) std::lround(0.0015 * SRC_SR) + 31;
	double err = 0.0, ref = 0.0;
	for (size_t i = 4410; i < out.size(); i++) {
		double want = g * src[i - lat];
		err += (out[i] - want) * (out[i] - want);
		ref += want * want;
	}
	double db = 10.0 * std::log10(err / ref);
	std::printf("  997 Hz sine: residual against the source %.1f dB (limit -45: the 4 Hz DC blocker alone is -48 at 1 kHz), gain x%.2f (the sample's loudness gain)\n", db, g);
	CHECK(db < -45.0, "residual %.1f dB", db);
}

static void testPitch() {
	std::printf("pitch (tape: speed and pitch together)\n");
	{
		Rig rig(makeBank(sineSource(4.0, 1000.0)));
		rig.in.markers[0].pitch = 12.f;
		rig.run(0.05);
		rig.note();
		std::vector<float> out;
		rig.run(0.5, &out);
		double at2k = tone(out, 4800, out.size(), 2000.0, 48000.0), at1k = tone(out, 4800, out.size(), 1000.0, 48000.0);
		std::printf("  +12 st on 1 kHz: 2 kHz %.3f, 1 kHz %.4f (2 kHz above 10x)\n", at2k, at1k);
		CHECK(at2k > 10.0 * at1k && at2k > 0.1, "2k %.3f, 1k %.4f", at2k, at1k);
	}
	// A 15 kHz tone an octave up would alias at 48 - 30 = 18 kHz; the decimated copy removes it first.
	{
		Rig hi(makeBank(sineSource(4.0, 15000.0)));
		hi.in.markers[0].pitch = 12.f;
		hi.run(0.05);
		hi.note();
		std::vector<float> a;
		hi.run(0.5, &a);
		Rig ref(makeBank(sineSource(4.0, 3000.0)));
		ref.in.markers[0].pitch = 12.f;
		ref.run(0.05);
		ref.note();
		std::vector<float> b;
		ref.run(0.5, &b);
		double alias = tone(a, 4800, a.size(), 18000.0, 48000.0), wanted = tone(b, 4800, b.size(), 6000.0, 48000.0);
		double db = 20.0 * std::log10(std::max(alias, 1e-9) / wanted);
		std::printf("  15 kHz +12 st: alias at 18 kHz %.1f dB under a 3 kHz tone played the same way (limit -40)\n", db);
		CHECK(db < -40.0, "alias %.1f dB", db);
	}
	// Up to the first copy change (about +7 st) the original itself is read: the top end stays (the decimated copy
	// would leave almost nothing of 14 kHz). Cubic interpolation alone loses about 1.6 dB up there.
	{
		Rig rig(makeBank(sineSource(4.0, 14000.0)));
		rig.in.markers[0].pitch = 3.f;
		rig.run(0.05);
		rig.note();
		std::vector<float> out;
		rig.run(0.5, &out);
		double f = 14000.0 * std::pow(2.0, 3.0 / 12.0) * 44100.0 / 44100.0;
		double got = tone(out, 4800, out.size(), f, 48000.0) / rig.bank->sample.normGain;
		std::printf("  14 kHz +3 st: level %.3f of the source's 0.5 (above 0.35; cubic interpolation's own roll-off)\n", got);
		CHECK(got > 0.35, "level %.3f", got);
	}
}

static void testLoopAndEnds() {
	std::printf("slice end: LOOP, ONE, PING\n");
	// Sine bound: a sample step of a sine is at most 2 pi f / sr of its peak. A cut at the loop point would be up to
	// twice the peak.
	const double hz = 300.25;
	auto src = sineSource(2.0, hz);
	for (float fade : {0.f, 0.1f}) {
		Rig rig(makeBank(src));
		rig.in.markers[0].scanLength = 0.1234f; // 0.2468 s, not a whole number of cycles
		rig.in.markers[0].scanFade = fade;
		rig.in.markers[0].loopMode = wa::PLAYER_LOOP;
		rig.run(0.05);
		rig.note();
		std::vector<float> out;
		rig.run(2.0, &out);
		double peak = 0.5 * rig.bank->sample.normGain;
		double bound = 2 * PI * hz / 48000.0 * peak;
		double step = maxStep(out, 480, out.size());
		double late = rms(out, out.size() - 4800, out.size());
		std::printf("  LOOP, FADE %.0f%%: largest step %.4f (sine bound %.4f, limit x1.5), still playing after 8 loops: rms %.3f\n",
			fade * 100.f, step, bound, late);
		CHECK(step < 1.5 * bound, "loop step %.4f", step);
		CHECK(late > 0.3 * peak, "loop stopped: rms %.4f", late);
	}
	{
		Rig rig(makeBank(src));
		rig.in.markers[0].scanLength = 0.1f; // 0.2 s
		rig.in.markers[0].loopMode = wa::PLAYER_ONE;
		rig.run(0.05);
		rig.note();
		std::vector<float> out;
		rig.run(0.6, &out);
		double peak = 0.5 * rig.bank->sample.normGain;
		double bound = 2 * PI * hz / 48000.0 * peak;
		double during = rms(out, 4800, 9000), after = rms(out, (size_t) (0.21 * 48000), out.size());
		double step = maxStep(out, 480, out.size());
		std::printf("  ONE: rms %.3f while playing, %.1e after the slice (limit 1e-4), largest step %.4f (bound %.4f, limit x1.5)\n",
			during, after, step, bound);
		CHECK(after < 1e-4, "one shot still sounds: %.2e", after);
		CHECK(during > 0.3 * peak, "one shot too quiet: %.3f", during);
		CHECK(step < 1.5 * bound, "end step %.4f", step);
	}
	{
		Rig rig(makeBank(noiseSource(2.0)));
		rig.in.markers[0].scanLength = 0.1f; // 8820 frames
		rig.in.markers[0].loopMode = wa::PLAYER_PING;
		rig.run(0.05);
		rig.note();
		float lo = 1.f, hi = 0.f, last = -1.f;
		int turns = 0, dir = 0;
		for (int i = 0; i < 48000; i++) {
			float l, r;
			rig.step(&l, &r);
			float at = rig.engine->getReadPosition(0);
			if (last >= 0.f && at != last) {
				int d = at > last ? 1 : -1;
				if (dir != 0 && d != dir)
					turns++;
				dir = d;
			}
			last = at;
			lo = std::min(lo, at);
			hi = std::max(hi, at);
		}
		// 1 s at 0.919 frames per sample is 44100 frames: 5 runs of the 8820-frame slice, so 4 turns
		std::printf("  PING: read place %.3f..%.3f of the sample (slice 0..0.1), %d turns in 1 s (4)\n", lo, hi, turns);
		CHECK(lo >= 0.f && hi <= 0.1001f && turns == 4, "ping %.3f..%.3f, %d turns", lo, hi, turns);
	}
}

static void testNotes() {
	std::printf("notes\n");
	// Equal notes sound the same, whenever they start; a note over a sounding one fades the old head out.
	auto src = noiseSource(3.0);
	std::vector<float> a, b, c;
	{
		Rig rig(makeBank(src));
		rig.in.markers[0].gain = 0.2f; // under the output limiter
		rig.run(0.13);
		rig.note();
		rig.run(0.5, &a);
		rig.in.voiceEnv[0] = 0.f;
		rig.run(1.37);
		rig.note();
		rig.run(0.5, &b);
	}
	{
		Rig rig(makeBank(src));
		rig.in.markers[0].gain = 0.2f;
		rig.run(0.05);
		rig.note();
		rig.run(0.33);
		rig.note(); // over the sounding note
		rig.run(0.5, &c);
	}
	double dab = 0.0, dac = 0.0, ref = 0.0;
	// After the 5 ms fade, the limiter's look-ahead (1.5 ms) and the clean path's DRIVE match (0.65 ms)
	size_t from = (size_t) (0.008 * 48000);
	for (size_t i = 1; i < a.size(); i++) {
		ref = std::max(ref, (double) std::fabs(a[i] - a[i - 1]));
		dab = std::max(dab, (double) std::fabs((a[i] - a[i - 1]) - (b[i] - b[i - 1])));
		if (i >= from)
			dac = std::max(dac, (double) std::fabs((a[i] - a[i - 1]) - (c[i] - c[i - 1])));
	}
	std::printf("  two notes 1.9 s apart: sample-to-sample difference %.1e (limit 1e-4)\n", dab / ref);
	std::printf("  a note over a sounding one, from 8 ms (fade and output delay): %.1e from a fresh note (limit 1e-3)\n", dac / ref);
	CHECK(dab / ref < 1e-4, "notes differ by %.2e", dab / ref);
	CHECK(dac / ref < 1e-3, "retrigger differs by %.2e", dac / ref);
	double fadeStep = maxStep(c, 1, from), noteStep = maxStep(c, from, c.size());
	std::printf("  largest step in the fade %.3f, in the note after it %.3f (limit x1.5)\n", fadeStep, noteStep);
	CHECK(fadeStep < 1.5 * noteStep, "fade step %.3f", fadeStep);
}

static void testPitchSweep() {
	std::printf("pitch sweep across the copy changes\n");
	// 0 to +24 st over 2 s on a 500 Hz sine: the head moves to the decimated copies twice, with no step above the
	// sine's own at the highest pitch.
	Rig rig(makeBank(sineSource(8.0, 500.0)));
	rig.in.markers[0].loopMode = wa::PLAYER_ONE;
	rig.run(0.05);
	rig.note();
	std::vector<float> out;
	for (int i = 0; i < 96000; i++) {
		rig.in.markers[0].pitch = 24.f * i / 96000.f;
		float l, r;
		rig.step(&l, &r);
		out.push_back(l);
	}
	double peak = 0.5 * rig.bank->sample.normGain;
	double bound = 2 * PI * 2000.0 * 44100.0 / 48000.0 / 48000.0 * peak;
	double step = maxStep(out, 480, out.size());
	std::printf("  largest step %.4f (bound at the top pitch %.4f, limit x1.1)\n", step, bound);
	CHECK(step < 1.1 * bound, "sweep step %.4f", step);
}

static void testMorph() {
	std::printf("MORPH (no notes)\n");
	// The playhead between two markers: both heard, and ONE keeps looping (there is no note to start it again).
	Rig rig(makeBank(noiseSource(4.0)), 48000.f, false);
	rig.in.markers[0].pos = 0.f;
	rig.in.markers[0].loopMode = wa::PLAYER_ONE;
	rig.in.markers[0].scanLength = 0.05f;
	rig.in.markers[1].active = true;
	rig.in.markers[1].pos = 1.f;
	rig.in.markers[1].start = 0.5f;
	rig.in.position = 0.f;
	std::vector<float> out;
	rig.run(1.5, &out);
	double late = rms(out, (size_t) (1.0 * 48000), out.size());
	std::printf("  ONE on marker 1, 1 s after its 0.2 s slice: rms %.3f (above 0.1: it loops)\n", late);
	CHECK(late > 0.1, "morph one shot stopped: %.3f", late);
}

static void testCpu() {
	std::printf("CPU\n");
	Rig rig(makeBank(noiseSource(8.0)));
	for (int m = 0; m < wa::NUM_MARKERS; m++) {
		rig.in.markers[m].active = true;
		rig.in.markers[m].pos = m * 0.25f;
		rig.in.markers[m].start = m * 0.18f;
		rig.in.markers[m].pitch = 7.5f; // between two copies' thresholds: the decimated copy
		rig.in.markers[m].scanLength = 0.01f; // short loops: crossfades all the time
		rig.in.voiceEnv[m] = 1.f;
		rig.in.voiceStart[m] = true;
	}
	rig.run(0.5);
	long n = (long) (4 * 48000);
	auto t0 = std::chrono::steady_clock::now();
	float acc = 0.f;
	for (long i = 0; i < n; i++) {
		float l, r;
		rig.step(&l, &r);
		acc += l;
	}
	double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
	std::printf("  five markers playing short loops: %.2f us per sample (limit 2; the grain test allows 4 for two clouds) %s\n", us, acc == 1e30f ? "" : "");
	CHECK(us < 2.0, "%.2f us per sample", us);
}

/** Read places of marker 0 over `seconds` of a note, as shares of the sample */
static std::vector<float> readPlaces(Rig& rig, double seconds) {
	std::vector<float> at;
	long n = (long) (seconds * rig.sr);
	for (long i = 0; i < n; i++) {
		float l, r;
		rig.step(&l, &r);
		at.push_back(rig.engine->getReadPosition(0));
	}
	return at;
}

static void testLoopPoint() {
	std::printf("LOOP point\n");
	// Slice 0..0.1 of the sample, loop point 40%: the first run plays from START, then only 0.04..0.1 repeats.
	struct Case {
		int loop, dir;
		float lo, hi;
		const char* name;
	};
	const Case cases[] = {
		{wa::PLAYER_LOOP, wa::GRAIN_FORWARD, 0.04f, 0.1f, "LOOP forward"},
		{wa::PLAYER_LOOP, wa::GRAIN_REVERSE, 0.f, 0.06f, "LOOP reverse (mirror)"},
		{wa::PLAYER_PING, wa::GRAIN_FORWARD, 0.04f, 0.1f, "PING forward"},
		{wa::PLAYER_PING, wa::GRAIN_REVERSE, 0.f, 0.06f, "PING reverse (mirror)"},
	};
	for (const Case& k : cases) {
		Rig rig(makeBank(noiseSource(2.0)));
		rig.in.markers[0].scanLength = 0.1f; // 8820 frames, 0.2 s at 44.1 kHz
		rig.in.markers[0].loopMode = k.loop;
		rig.in.markers[0].loopPoint = 0.4f;
		rig.in.markers[0].direction = k.dir;
		rig.run(0.05);
		rig.note();
		std::vector<float> first = readPlaces(rig, 0.15);
		std::vector<float> later = readPlaces(rig, 1.5);
		float f0 = *std::min_element(first.begin(), first.end()), f1 = *std::max_element(first.begin(), first.end());
		float l0 = *std::min_element(later.begin(), later.end()), l1 = *std::max_element(later.begin(), later.end());
		std::printf("  %-22s first run %.3f..%.3f, then %.3f..%.3f (the repeated part %.2f..%.2f)\n", k.name, f0, f1, l0, l1, k.lo, k.hi);
		bool firstFromStart = k.dir == wa::GRAIN_FORWARD ? f0 < 0.001f : f1 > 0.099f;
		CHECK(firstFromStart, "%s: the first run does not start at its end of the slice", k.name);
		CHECK(l0 > k.lo - 0.002f && l1 < k.hi + 0.002f, "%s: loops over %.3f..%.3f", k.name, l0, l1);
		CHECK(l1 - l0 > 0.9f * (k.hi - k.lo), "%s: loop covers only %.3f..%.3f", k.name, l0, l1);
	}
	// The loop point still never clicks
	{
		const double hz = 300.25;
		Rig rig(makeBank(sineSource(2.0, hz)));
		rig.in.markers[0].scanLength = 0.1234f;
		rig.in.markers[0].loopPoint = 0.37f;
		rig.in.markers[0].loopMode = wa::PLAYER_LOOP;
		rig.run(0.05);
		rig.note();
		std::vector<float> out;
		rig.run(2.0, &out);
		double bound = 2 * PI * hz / 48000.0 * 0.5 * rig.bank->sample.normGain;
		double step = maxStep(out, 480, out.size());
		std::printf("  loop point 37%%: largest step %.4f (sine bound %.4f, limit x1.5)\n", step, bound);
		CHECK(step < 1.5 * bound, "loop point step %.4f", step);
	}
}

static void testDrive() {
	std::printf("DRIVE\n");
	const double hz = 1000.0;
	auto render = [&](float drive, std::vector<float>* out, bool sweep) {
		Rig rig(makeBank(sineSource(3.0, hz, 0.4f)));
		rig.in.markers[0].gain = 0.5f;
		rig.in.markers[0].drive = sweep ? 0.f : drive;
		rig.run(0.05);
		rig.note();
		long n = (long) (1.0 * 48000);
		for (long i = 0; i < n; i++) {
			if (sweep && i == n / 2)
				rig.in.markers[0].drive = drive;
			float l, r;
			rig.step(&l, &r);
			out->push_back(l);
		}
	};
	// A drive just above 0 runs (it is heard) but sounds like the clean path: same level at 1 kHz and 12 kHz. The
	// drive is 30.5 samples late and the clean path 31, so the two are half a sample apart at most.
	std::vector<float> clean, tiny;
	render(0.f, &clean, false);
	render(0.0005f, &tiny, false);
	double lv = 20.0 * std::log10(rms(tiny, 9600, tiny.size()) / rms(clean, 9600, clean.size()));
	std::printf("  DRIVE 0.05%% against DRIVE 0 at 1 kHz: %+.3f dB (limit +-0.1)\n", lv);
	CHECK(std::fabs(lv) < 0.1, "level %+.3f dB", lv);
	// Full drive adds harmonics
	std::vector<float> hot;
	render(1.f, &hot, false);
	double h1 = tone(hot, 9600, hot.size(), hz, 48000.0), h2 = tone(hot, 9600, hot.size(), 2 * hz, 48000.0),
		h3 = tone(hot, 9600, hot.size(), 3 * hz, 48000.0), c3 = tone(clean, 9600, clean.size(), 3 * hz, 48000.0);
	std::printf("  DRIVE 100%%: 2nd %.1f dB, 3rd %.1f dB under the fundamental (above -40), clean 3rd %.1f dB\n",
		20 * std::log10(h2 / h1), 20 * std::log10(h3 / h1), 20 * std::log10(std::max(c3, 1e-12) / h1));
	CHECK(h3 / h1 > 0.01 && h2 / h1 > 0.001, "no harmonics: 2nd %.4f, 3rd %.4f", h2 / h1, h3 / h1);
	// Turning DRIVE up while playing fades in: no step bigger than the driven sound's own, and no dip in level
	// while the clean and driven paths cross (1 ms windows over the 20 ms after the change).
	std::vector<float> swept;
	render(0.8f, &swept, true);
	size_t mid = swept.size() / 2;
	double at = maxStep(swept, mid, mid + 960), driven = maxStep(swept, mid + 4800, swept.size());
	double lo = 1e9, hi = 0.0;
	for (size_t w = mid; w + 48 <= mid + 960; w += 48) {
		double v = rms(swept, w, w + 48);
		lo = std::min(lo, v);
		hi = std::max(hi, v);
	}
	double ends = std::min(rms(swept, mid - 960, mid), rms(swept, mid + 4800, swept.size()));
	std::printf("  DRIVE 0 -> 80%% while playing: largest step %.4f in the 20 ms after, %.4f once driven (limit x1.2);"
		" 1 ms level %.3f..%.3f (no dip under %.3f x0.9)\n", at, driven, lo, hi, ends);
	CHECK(at < 1.2 * driven, "drive switch step %.4f", at);
	CHECK(lo > 0.9 * ends, "dip while crossing: %.3f", lo);
	// 12 kHz through the crossfade: the half-sample offset dims it at most a little, for 5 ms
	{
		Rig rig(makeBank(sineSource(3.0, 12000.0, 0.4f)));
		rig.in.markers[0].gain = 0.5f;
		rig.run(0.05);
		rig.note();
		std::vector<float> out;
		rig.run(0.5, &out);
		rig.in.markers[0].drive = 0.0005f;
		rig.run(0.5, &out);
		size_t m = (size_t) (0.5 * 48000);
		double steady = rms(out, m - 4800, m), worst = 1e9;
		for (size_t w = m; w + 48 <= m + 960; w += 48)
			worst = std::min(worst, rms(out, w, w + 48));
		double db12 = 20.0 * std::log10(worst / steady);
		std::printf("  12 kHz while DRIVE crosses in: lowest 1 ms level %+.2f dB (limit -1.5)\n", db12);
		CHECK(db12 > -1.5, "12 kHz dip %.2f dB", db12);
	}
}
/** Drum-like hits: decaying noise bursts with a sine body, at known frames, louder and softer, on a quiet bed */
/** HITS slider settings the tests use: few, the default, more, all */
static const int NUM_SENS = 4;
static const float SENS[NUM_SENS] = {0.2f, 0.5f, 0.75f, 1.f};
static const char* const SENS_NAMES[NUM_SENS] = {"20 %", "50 %", "75 %", "100 %"};
static std::vector<float> hitsSource(const std::vector<long>& at, const std::vector<float>& amp, double seconds) {
	std::vector<float> v((size_t) (seconds * SRC_SR));
	uint32_t s = 777u;
	for (size_t i = 0; i < v.size(); i++) {
		s = s * 1664525u + 1013904223u;
		v[i] = ((s >> 8) * (1.f / 8388608.f) - 1.f) * 0.002f; // a quiet bed (-54 dB)
	}
	for (size_t k = 0; k < at.size(); k++) {
		for (long i = 0; i < (long) (0.25 * SRC_SR) && at[k] + i < (long) v.size(); i++) {
			s = s * 1664525u + 1013904223u;
			float noise = ((s >> 8) * (1.f / 8388608.f) - 1.f);
			float env = std::exp(-(float) i / (0.03f * SRC_SR));
			v[(size_t) (at[k] + i)] += amp[k] * env * (0.6f * noise + 0.4f * (float) std::sin(2 * PI * 90.0 * i / SRC_SR));
		}
	}
	return v;
}

static void testTransients() {
	std::printf("transients\n");
	// Eight hits, 0.3 s apart, two of them soft (-18 dB)
	std::vector<long> at;
	std::vector<float> amp;
	for (int k = 0; k < 8; k++) {
		at.push_back((long) ((0.05 + 0.3 * k) * SRC_SR) + 37 * k);
		amp.push_back((k == 3 || k == 6) ? 0.06f : 0.5f);
	}
	auto bank = makeBank(hitsSource(at, amp, 2.6));
	const wa::SampleBuffer& S = bank->sample;
	int counts[NUM_SENS];
	for (int sens = 0; sens < NUM_SENS; sens++) {
		std::vector<long> found = wa::findTransients(S, SENS[sens]);
		counts[sens] = (int) found.size();
		// Each loud hit found, close to where it starts
		int hits = 0;
		double worstMs = 0.0;
		for (size_t k = 0; k < at.size(); k++) {
			double best = 1e9;
			for (long f : found)
				best = std::min(best, std::fabs((double) (f - at[k])));
			if (best < 0.01 * SRC_SR) {
				hits++;
				worstMs = std::max(worstMs, best * 1000.0 / SRC_SR);
			}
		}
		std::printf("  %-6s: %d found, %d of the 8 hits within 10 ms (worst %.2f ms)\n", SENS_NAMES[sens],
			counts[sens], hits, worstMs);
		if (sens == 1) {
			CHECK(hits == 8 && counts[sens] == 8, "Normal: %d hits, %d found (want 8 and 8)", hits, counts[sens]);
			CHECK(worstMs < 1.0, "Normal: placed %.2f ms off", worstMs);
		}
	}
	CHECK(counts[0] <= counts[1] && counts[1] <= counts[2] && counts[2] <= counts[3], "fewer..most not in order: %d %d %d %d",
		counts[0], counts[1], counts[2], counts[3]);
	CHECK(counts[0] >= 6 && counts[0] <= 8, "Fewer should keep the loud hits: %d", counts[0]);
	// Steady noise has one transient (its start) and no more
	{
		auto noise = makeBank(noiseSource(2.0));
		std::vector<long> found = wa::findTransients(noise->sample, SENS[1]);
		std::printf("  steady noise, 50 %%: %d found (want 1, its start)\n", (int) found.size());
		CHECK(found.size() == 1, "steady noise: %d", (int) found.size());
	}
	// Each one sits on a zero crossing
	{
		std::vector<long> found = wa::findTransients(S, SENS[1]);
		int off = 0;
		for (long f : found) {
			if (f < 1)
				continue;
			float a = wa::monoFrame(S, f - 1), b = wa::monoFrame(S, f), c = wa::monoFrame(S, std::min(f + 1, (long) S.frames - 1));
			bool zc = b == 0.f || (a < 0.f) != (b < 0.f) || (b < 0.f) != (c < 0.f);
			off += zc ? 0 : 1;
		}
		std::printf("  on a zero crossing: %d of %d (want all)\n", (int) found.size() - off, (int) found.size());
		CHECK(off == 0, "%d transients not on a zero crossing", off);
	}
}

static void testZeroSnap() {
	std::printf("zero-crossing snap\n");
	// A 100 Hz sine, START at its peak: a note starting there at full level would jump by the peak. The note's fade-in
	// (PLAYER_ATTACK_SECONDS) keeps every step under the bound of a sine with that fade, snap on or off; the snap
	// starts it near 0 besides.
	std::vector<float> src((size_t) SRC_SR * 2);
	for (size_t i = 0; i < src.size(); i++)
		src[i] = 0.5f * (float) std::sin(2 * PI * 100.0 * i / SRC_SR);
	for (int snap = 0; snap < 2; snap++) {
		Rig rig(makeBank(src));
		rig.in.zeroSnap = snap == 1;
		rig.in.markers[0].start = 0.25f / 2.f / 100.f * 1.f + 0.1f; // a quarter cycle in: the peak
		rig.in.markers[0].loopMode = wa::PLAYER_ONE;
		rig.run(0.05);
		rig.note();
		std::vector<float> out;
		rig.run(0.05, &out);
		// The first sound after the output delay (1.5 ms limiter + 31 samples)
		size_t first = 0;
		while (first < out.size() && std::fabs(out[first]) < 1e-6f)
			first++;
		float jump = first < out.size() ? std::fabs(out[first]) : 0.f;
		double peak = 0.5 * rig.bank->sample.normGain;
		// Largest step of the sine itself plus that of the smoothstep fade (1.5 / its length), with 20 % margin
		double bound = 1.2 * peak * (2 * PI * 100.0 / 48000.0 + 1.5 / (wa::PLAYER_ATTACK_SECONDS * 48000.0));
		double step = std::max((double) jump, maxStep(out, first, first + (size_t) (0.004 * 48000)));
		std::printf("  %s: first sample %.4f, largest step at the start %.4f (bound %.4f; a start at full level jumps %.3f)\n",
			snap ? "snap on " : "snap off", jump, step, bound, peak);
		CHECK(step < bound, "%s: the start steps %.4f", snap ? "snapped" : "unsnapped", step);
		CHECK(bound < 0.1 * peak, "the check cannot fail: bound %.4f of a %.3f jump", bound, peak);
		if (snap)
			CHECK(jump < 0.02f, "snapped start jumps %.4f", jump);
	}
	// The snap moves at most 5 ms, and onto a real crossing
	{
		auto bank = makeBank(src);
		long at = wa::nearestZeroCrossing(bank->sample, 4410 + 110, (long) (0.005 * SRC_SR));
		std::printf("  nearest crossing to frame 4520: %ld (the sine crosses at 4410 and 4630.5)\n", at);
		CHECK(std::labs(at - 4520) <= 221 && (std::labs(at - 4410) <= 1 || std::labs(at - 4630) <= 1), "crossing at %ld", at);
	}
}

static void testBeat() {
	std::printf("BEAT\n");
	// A 0.45 s slice at 120 bpm (0.5 s a beat): FIT plays it in 1 beat, so 0.5 s, a little slower.
	Rig rig(makeBank(noiseSource(2.0)));
	rig.in.markers[0].scanLength = 0.45f / 2.0f; // of a 2 s sample
	rig.in.markers[0].loopMode = wa::PLAYER_LOOP;
	rig.in.markers[0].beatMode = wa::SAMPLE_BEAT_FIT;
	rig.run(0.05);
	rig.note();
	long wraps = 0;
	float last = -1.f;
	long firstWrap = -1, lastWrap = -1;
	for (long i = 0; i < 48000 * 4; i++) {
		float l, r;
		rig.step(&l, &r);
		float at = rig.engine->getReadPosition(0);
		if (last >= 0.f && at < last - 0.1f) {
			if (firstWrap < 0)
				firstWrap = i;
			lastWrap = i;
			wraps++;
		}
		last = at;
	}
	double period = wraps > 1 ? (double) (lastWrap - firstWrap) / (wraps - 1) / 48000.0 : 0.0;
	std::printf("  0.45 s slice at 120 bpm: loops every %.4f s (want 0.5000, 1 beat), %g beats shown\n", period, rig.engine->getBeats(0));
	CHECK(std::fabs(period - 0.5) < 0.002 && rig.engine->getBeats(0) == 1.0, "beat loop %.4f s", period);
}
static void testTransientSensitivity() {
	std::printf("transient sensitivity\n");
	// Ten hits getting softer (0.6 down to 0.004) over a steady tone (0.1) and a noise bed (0.01): fewer settings keep
	// the strong ones only, more settings dig into the bed.
	std::vector<long> at;
	std::vector<float> amp;
	for (int k = 0; k < 10; k++) {
		at.push_back((long) ((0.1 + 0.3 * k) * SRC_SR));
		amp.push_back(0.6f * std::pow(0.004f / 0.6f, k / 9.f));
	}
	std::vector<float> src = hitsSource(at, amp, 3.3);
	for (size_t i = 0; i < src.size(); i++)
		src[i] += 0.1f * (float) std::sin(2 * PI * 220.0 * i / SRC_SR) + 0.01f * (float) std::sin(i * 2.3999 + std::sin(i * 0.731) * 40.0);
	auto bank = makeBank(src);
	int counts[NUM_SENS];
	for (int s = 0; s < NUM_SENS; s++)
		counts[s] = (int) wa::findTransients(bank->sample, SENS[s]).size();
	std::printf("  hits from 0.6 down to 0.004 over a 0.1 tone and a 0.01 bed: 20 %% %d, 50 %% %d, 75 %% %d, 100 %% %d (of 10, plus the start)\n",
		counts[0], counts[1], counts[2], counts[3]);
	CHECK(counts[0] <= counts[1] && counts[1] <= counts[2] && counts[2] <= counts[3] && counts[0] + 2 <= counts[3], "sensitivity does not grade: %d %d %d %d",
		counts[0], counts[1], counts[2], counts[3]);
	CHECK(counts[3] <= 11, "Most finds more than there are: %d", counts[3]);
}
/** One drum hit added at `at`: a noise click decaying over `click` seconds and a pitched body over `body` seconds
(none at 0), faded out at its end (a cut would itself be a click) */
static void addHit(std::vector<float>& v, long at, float amp, double click, double body, double bodyHz, uint32_t* s) {
	long len = (long) (6 * std::max(click, body) * SRC_SR);
	for (long i = 0; i < len && at + i < (long) v.size(); i++) {
		*s = *s * 1664525u + 1013904223u;
		float noise = ((*s >> 8) * (1.f / 8388608.f) - 1.f);
		float env = std::exp(-(float) i / (float) (click * SRC_SR));
		float bodyEnv = body > 0.0 ? std::exp(-(float) i / (float) (body * SRC_SR)) : 0.f;
		float tail = std::min(1.f, (float) (len - i) / (0.3f * len));
		v[(size_t) (at + i)] += tail * amp * (0.7f * env * noise + 0.5f * bodyEnv * (float) std::sin(2 * PI * bodyHz * i / SRC_SR));
	}
}

static void testDenseHits() {
	std::printf("dense hits (a drum loop)\n");
	// 140 bpm, 4 bars: kick on 1 and 3, snare on 2 and 4, 16th hats (soft, -20 dB under the kick) all over a sustained
	// bass at -14 dB; bar 3 has a 32nd hat roll, bar 4 a fast buzz run (every 50 ms, the dense part of a waveform).
	// HITS judges each hit against the loudest around it: the fewer settings keep the kicks and snares, Normal adds
	// the buzz run (the hats by a kick may go), the more settings find every hit. All close to where they start.
	const double beat = 60.0 / 140.0;
	std::vector<float> v((size_t) (16 * beat * SRC_SR + SRC_SR / 2), 0.f);
	for (size_t i = 0; i < v.size(); i++)
		v[i] = 0.2f * (float) std::sin(2 * PI * 55.0 * i / SRC_SR) * (0.8f + 0.2f * (float) std::sin(2 * PI * 0.5 * i / SRC_SR));
	uint32_t s = 4242u;
	enum { KICK, SNARE, HAT, BUZZ, NUM_KINDS };
	std::vector<long> hits;
	std::vector<int> kinds;
	auto put = [&](double t, int kind, float amp, double click, double body, double hz) {
		long at = (long) std::lround(t * SRC_SR);
		addHit(v, at, amp, click, body, hz, &s);
		hits.push_back(at);
		kinds.push_back(kind);
	};
	for (int b = 0; b < 4; b++) {
		double bar = b * 4 * beat;
		if (b < 3) {
			for (int k = 0; k < 16; k++) {
				double t = bar + k * beat / 4;
				if (k % 8 == 0)
					put(t, KICK, 0.8f, 0.004, 0.12, 60.0);   // kick: a short click, a long body
				else if (k % 8 == 4)
					put(t, SNARE, 0.6f, 0.03, 0.04, 190.0);  // snare: noisy
				else
					put(t, HAT, 0.08f, 0.008, 0.0, 0.0);     // hat
			}
			if (b == 2)
				for (int k = 0; k < 8; k++)
					put(bar + 2 * beat + (2 * k + 1) * beat / 8, HAT, 0.1f, 0.006, 0.0, 0.0); // the 32nd roll's in-between hats
		}
		else {
			put(bar, KICK, 0.8f, 0.004, 0.12, 60.0);
			for (int k = 1; k < 30; k++)
				put(bar + 0.05 * k, BUZZ, 0.25f + 0.15f * (float) ((k * 7) % 5) / 4.f, 0.006, 0.01, 3000.0); // buzz run
		}
	}
	auto bank = makeBank(v);
	for (int sens = 0; sens < NUM_SENS; sens++) {
		std::vector<long> found = wa::findTransients(bank->sample, SENS[sens]);
		int got = 0, extra = 0, of[NUM_KINDS] = {}, gotKind[NUM_KINDS] = {}, masked = 0, maskedGot = 0;
		double worstMs = 0.0;
		for (size_t i = 0; i < hits.size(); i++) {
			double best = 1e9;
			for (long f : found)
				best = std::min(best, std::fabs((double) (f - hits[i])));
			of[kinds[i]]++;
			// A hat within 60 ms after a snare is as loud as the snare's noisy tail there (the roll's 4.768 s one): it may go
			bool inTail = false;
			for (size_t j = 0; j < hits.size(); j++)
				inTail = inTail || (kinds[i] == HAT && kinds[j] == SNARE && hits[i] > hits[j] && hits[i] - hits[j] < (long) (0.06 * SRC_SR));
			masked += inTail ? 1 : 0;
			if (best < 0.005 * SRC_SR) {
				maskedGot += inTail ? 1 : 0;
				got++;
				gotKind[kinds[i]]++;
				worstMs = std::max(worstMs, best * 1000.0 / SRC_SR);
			}
		}
		for (long f : found) {
			double best = 1e9;
			for (long h : hits)
				best = std::min(best, std::fabs((double) (f - h)));
			extra += best < 0.005 * SRC_SR ? 0 : 1;
		}
		std::printf("  %-6s: %d of %d hits within 5 ms (kicks %d/%d, snares %d/%d, hats %d/%d, buzz %d/%d; worst %.2f ms), %d found that are not hits\n",
			SENS_NAMES[sens], got, (int) hits.size(), gotKind[KICK], of[KICK], gotKind[SNARE], of[SNARE], gotKind[HAT], of[HAT],
			gotKind[BUZZ], of[BUZZ], worstMs, extra);
		// Kicks and snares at every setting; the buzz run from Normal on (closer than the fewer settings' gap); every
		// hat from 75 %, but the ones in a snare's tail. Measured: not hits 0, 1, 3, 7; worst 1.00 ms.
		if (sens >= 2)
			std::printf("          %d of %d hats in a snare's tail found\n", maskedGot, masked);
		CHECK(gotKind[KICK] == of[KICK] && gotKind[SNARE] == of[SNARE], "%s misses kicks or snares: %d/%d, %d/%d", SENS_NAMES[sens],
			gotKind[KICK], of[KICK], gotKind[SNARE], of[SNARE]);
		if (sens >= 1)
			CHECK(gotKind[BUZZ] == of[BUZZ], "%s misses %d of the buzz run", SENS_NAMES[sens], of[BUZZ] - gotKind[BUZZ]);
		if (sens >= 2)
			CHECK(got - maskedGot == (int) hits.size() - masked, "%s misses %d hits", SENS_NAMES[sens],
				(int) hits.size() - masked - (got - maskedGot));
		CHECK(worstMs < 1.2, "%s: placed %.2f ms off", SENS_NAMES[sens], worstMs);
		CHECK(extra <= (sens == 0 ? 1 : sens == 1 ? 2 : sens == 2 ? 10 : 25), "%s: %d found that are not hits", SENS_NAMES[sens], extra);
	}
}

static void testHitSnap() {
	std::printf("HIT snap\n");
	// Hits every 0.25 s; START turned to 0.4 s plays from the nearest hit (0.5 s), off plays from 0.4 s.
	std::vector<float> v((size_t) (2 * SRC_SR), 0.f);
	uint32_t s = 99u;
	for (int k = 0; k < 8; k++)
		addHit(v, (long) (k * 0.25 * SRC_SR), 0.5f, 0.01, 0.04, 120.0, &s);
	for (int on = 0; on < 2; on++) {
		Rig rig(makeBank(v));
		rig.in.markers[0].start = 0.2f; // 0.4 s of 2 s
		rig.in.markers[0].hitSnap = on == 1;
		std::vector<long> hits = wa::findTransients(rig.bank->sample, wa::TRANSIENT_DEFAULT_SENSITIVITY);
		for (auto& h : rig.in.hits)
			h = &hits;
		rig.in.markers[0].loopMode = wa::PLAYER_ONE;
		rig.run(0.05);
		rig.note();
		float l, r;
		rig.step(&l, &r);
		double at = rig.engine->getReadPosition(0) * 2.0;
		float sliceStart, sliceEnd;
		rig.engine->getScanSlice(0, &sliceStart, &sliceEnd);
		std::printf("  HIT %s: plays from %.4f s (map shows the slice from %.4f s)\n", on ? "on " : "off", at, sliceStart * 2.0);
		if (on)
			CHECK(std::fabs(at - 0.5) < 0.002 && std::fabs(sliceStart * 2.0 - 0.5) < 0.002, "HIT plays from %.4f", at);
		else
			CHECK(std::fabs(at - 0.4) < 0.006, "unsnapped plays from %.4f", at);
	}
}
static void testBeatCut() {
	std::printf("BEAT (LENGTH in note values, the sample's own speed)\n");
	// ONE, 1 beat at 120 bpm: plays 0.5 s of the sample at its own speed, then stops.
	{
		Rig rig(makeBank(noiseSource(3.0)));
		rig.in.markers[0].beatMode = wa::SAMPLE_BEAT_CUT;
		rig.in.markers[0].beatLength = 1.f;
		rig.in.markers[0].loopMode = wa::PLAYER_ONE;
		rig.run(0.05);
		rig.note();
		std::vector<float> out;
		std::vector<float> at;
		for (int i = 0; i < 48000; i++) {
			float l, r;
			rig.step(&l, &r);
			out.push_back(l);
			at.push_back(rig.engine->getReadPosition(0));
		}
		size_t last = 0;
		for (size_t i = 0; i < out.size(); i++)
			if (std::fabs(out[i]) > 4e-3f) // -40 dB of the sound; the output's DC blocker leaves a slow, tiny tail
				last = i;
		// The output is 1.5 ms (limiter) + 31 samples (the DRIVE match) late
		double sounded = (last - (0.0015 * 48000 + 31)) / 48000.0;
		double speed = (at[2400] - at[1200]) * 3.0 * SRC_SR / 1200.0 * 48000.0 / SRC_SR; // source frames per second / its rate
		std::printf("  ONE, 1 beat at 120 bpm: sounds %.4f s (want 0.5000), reads at %.4fx its own speed (want 1)\n", sounded, speed);
		CHECK(std::fabs(sounded - 0.5) < 0.003, "one shot lasts %.4f s", sounded);
		CHECK(std::fabs(speed - 1.0) < 0.002, "speed %.4f", speed);
	}
	// LOOP, 1 beat, FADE 10%: one pass every 0.5 s exactly, at 120 and 140 bpm
	for (float bpm : {120.f, 140.f}) {
		Rig rig(makeBank(noiseSource(3.0)));
		rig.in.markers[0].beatMode = wa::SAMPLE_BEAT_CUT;
		rig.in.markers[0].beatLength = 1.f;
		rig.in.markers[0].scanFade = 0.1f;
		rig.in.markers[0].loopMode = wa::PLAYER_LOOP;
		// The rig's clock runs at 120 bpm; another tempo needs its own clock steps
		long wraps = 0, firstWrap = -1, lastWrap = -1;
		float last = -1.f;
		for (int i = 0; i < 48000 * 4; i++) {
			if (i == 2400)
				rig.note();
			rig.clock.processInternal(bpm, 48000.f);
			float l, r;
			rig.engine->process(rig.in, &l, &r);
			rig.in.voiceStart[0] = false;
			float p = rig.engine->getReadPosition(0);
			if (last >= 0.f && p < last - 0.05f) {
				if (firstWrap < 0)
					firstWrap = i;
				lastWrap = i;
				wraps++;
			}
			last = p;
		}
		double period = wraps > 1 ? (double) (lastWrap - firstWrap) / (wraps - 1) / 48000.0 : 0.0;
		std::printf("  LOOP, 1 beat at %.0f bpm: a pass every %.4f s (want %.4f)\n", bpm, period, 60.0 / bpm);
		CHECK(std::fabs(period - 60.0 / bpm) < 0.0005, "loop pass %.4f s at %.0f bpm", period, bpm);
	}
	// LOOP point on the grid: a 1-beat slice has 32 places (1/128 notes); 0.3 lands on place 10 (0.3125 of the beat)
	{
		Rig rig(makeBank(noiseSource(3.0)));
		rig.in.markers[0].beatMode = wa::SAMPLE_BEAT_CUT;
		rig.in.markers[0].beatLength = 1.f;
		rig.in.markers[0].loopPoint = 0.3f;
		rig.in.markers[0].loopMode = wa::PLAYER_LOOP;
		rig.run(0.05);
		rig.note();
		float lo = 1.f;
		for (int i = 0; i < 48000 * 2; i++) {
			float l, r;
			rig.step(&l, &r);
			if (i > 30000)
				lo = std::min(lo, rig.engine->getReadPosition(0));
		}
		double from = lo * 3.0; // seconds into the sample (START 0)
		std::printf("  LOOP point 0.3 on a 1-beat slice (32 places, 1/128 notes): loops from %.4f s (want 0.1563, place 10)\n", from);
		CHECK(std::fabs(from - 0.15625) < 0.002, "loop from %.4f s", from);
	}
	// The note values: knob 0..1 goes from the shortest to the longest
	{
		float lo = wa::SAMPLE_DIVISIONS[wa::sampleBeatDivision(0.f, false, false)].beats;
		float hi = wa::SAMPLE_DIVISIONS[wa::sampleBeatDivision(1.f, false, false)].beats;
		float mid = wa::SAMPLE_DIVISIONS[wa::sampleBeatDivision(0.5f, false, false)].beats;
		std::printf("  LENGTH knob: %g .. %g beats (middle %g)\n", lo, hi, mid);
		CHECK(lo < 0.032f && hi >= 16.f && mid > lo && mid < hi, "note values %g .. %g", lo, hi);
	}
}
static void testHitsSlider() {
	std::printf("HITS slider\n");
	// The graded source (hits from 0.6 down to 0.004 over a tone and a noise bed): moving the slider up never finds
	// fewer, and the ends differ a lot.
	std::vector<long> at;
	std::vector<float> amp;
	for (int k = 0; k < 10; k++) {
		at.push_back((long) ((0.1 + 0.3 * k) * SRC_SR));
		amp.push_back(0.6f * std::pow(0.004f / 0.6f, k / 9.f));
	}
	std::vector<float> src = hitsSource(at, amp, 3.3);
	for (size_t i = 0; i < src.size(); i++)
		src[i] += 0.1f * (float) std::sin(2 * PI * 220.0 * i / SRC_SR) + 0.01f * (float) std::sin(i * 2.3999 + std::sin(i * 0.731) * 40.0);
	auto bank = makeBank(src);
	int prev = -1, first = 0, last = 0;
	bool monotonic = true;
	std::printf("  hits found from 0 %% to 100 %%:");
	for (int k = 0; k <= 10; k++) {
		int n = (int) wa::findTransients(bank->sample, k / 10.f).size();
		std::printf(" %d", n);
		if (k == 0)
			first = n;
		last = n;
		monotonic = monotonic && n >= prev;
		prev = n;
	}
	std::printf(" (never fewer going up; 0 %% keeps the strongest)\n");
	CHECK(monotonic, "the slider does not grade");
	CHECK(first >= 1 && first + 3 <= last, "0 %% finds %d, 100 %% %d", first, last);
	// Time for a long sample: 10 MB of 16-bit stereo is about 60 s; mono mixdown 2.6 M frames
	{
		auto big = makeBank(noiseSource(60.0));
		auto t0 = std::chrono::steady_clock::now();
		std::vector<long> found = wa::findTransients(big->sample, 0.5f);
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::printf("  60 s of sample analysed in %.1f ms (the slider re-analyses at most every 30 ms while dragged; limit 25)\n", ms);
		CHECK(ms < 25.0, "analysis takes %.1f ms", ms);
	}
}
static void testBeatPitch() {
	std::printf("BEAT keeps its note value at any pitch\n");
	// A user's settings: BEAT 1/32, PITCH -36, LOOP, loop point 0.331, FADE 0, at 129 bpm, retriggered on every 16th by
	// the sequencer. The loop must last a 1/32 note as heard (58 ms), so each 16th step hears it loop.
	const double bpm = 129.0, beat = 60.0 / bpm;
	for (int retrig = 0; retrig < 2; retrig++) {
		for (float pitch : {0.f, -36.f, 12.f}) {
			if (retrig && pitch != -36.f)
				continue;
			Rig rig(makeBank(noiseSource(4.0)));
			wa::GrainMarkerInput& M = rig.in.markers[0];
			M.beatMode = wa::SAMPLE_BEAT_CUT;
			M.beatLength = 0.125f;
			M.pitch = pitch;
			M.loopMode = wa::PLAYER_LOOP;
			M.loopPoint = 0.331f;
			M.scanFade = 0.f;
			long sixteenth = (long) std::lround(beat / 4.0 * 48000);
			long wraps = 0, first = -1, last = -1;
			float prev = -1.f;
			for (long i = 0; i < 48000 * 3; i++) {
				bool trig = i == 0 || (retrig && i % sixteenth == 0);
				if (trig)
					rig.note();
				rig.clock.processInternal((float) bpm, 48000.f);
				float l, r;
				rig.engine->process(rig.in, &l, &r);
				rig.in.voiceStart[0] = false;
				float at = rig.engine->getReadPosition(0);
				if (prev >= 0.f && at < prev - 1e-5f && !trig) {
					if (first < 0)
						first = i;
					last = i;
					wraps++;
				}
				prev = at;
			}
			double period = wraps > 1 ? (last - first) / (double) (wraps - 1) / 48000.0 : 0.0;
			if (!retrig) {
				// LOOP 0.331 on a 1/32 slice: place 3 of 8, so the loop is its last 5/8
				double want = beat / 8.0 * 5.0 / 8.0;
				std::printf("  PITCH %+3.0f, held: loops every %.4f s (want %.4f, 5/8 of a 1/32 note)\n", pitch, period, want);
				CHECK(std::fabs(period - want) < 0.0005, "pitch %g: loop %.4f s", pitch, period);
			}
			else {
				std::printf("  PITCH -36, a new note every 16th: %ld loop passes in 3 s (about %d: one in each step)\n", wraps, (int) (3.0 / (beat / 4.0)));
				CHECK(wraps >= (long) (3.0 / (beat / 4.0)) - 2, "retriggered: only %ld loop passes", wraps);
			}
		}
	}
}
static void testBeatShortest() {
	std::printf("BEAT down to 1/128\n");
	// 1/128 at 120 bpm is 15.6 ms; LOOP point 0.5 (place 4 of 8) loops the second half: a pass every 7.8 ms.
	Rig rig(makeBank(noiseSource(3.0)));
	wa::GrainMarkerInput& M = rig.in.markers[0];
	M.beatMode = wa::SAMPLE_BEAT_CUT;
	M.beatLength = 1.f / 32.f;
	M.loopMode = wa::PLAYER_LOOP;
	M.loopPoint = 0.5f;
	M.scanFade = 0.f;
	rig.run(0.05);
	rig.note();
	long wraps = 0, first = -1, last = -1;
	float prev = -1.f, lo = 1.f, hi = 0.f;
	for (long i = 0; i < 48000; i++) {
		float l, r;
		rig.step(&l, &r);
		float at = rig.engine->getReadPosition(0);
		if (prev >= 0.f && at < prev - 1e-6f) {
			if (first < 0)
				first = i;
			last = i;
			wraps++;
		}
		if (i > 4800) {
			lo = std::min(lo, at);
			hi = std::max(hi, at);
		}
		prev = at;
	}
	double period = wraps > 1 ? (last - first) / (double) (wraps - 1) / 48000.0 : 0.0;
	std::printf("  1/128, LOOP point 0.5: a pass every %.5f s (want 0.00781), reads %.2f..%.2f ms of the sample (want 7.8..15.6 + the crossfade)\n",
		period, lo * 3000.0, hi * 3000.0);
	CHECK(std::fabs(period - 0.0078125) < 0.0002, "1/128 loop %.5f s", period);
	CHECK(std::fabs(lo * 3.0 - 0.0078125) < 0.0005, "loops from %.2f ms", lo * 3000.0);
	CHECK(wa::sampleLoopSteps(1.0 / 32.0) == 8 && wa::sampleLoopSteps(1.0) == 32 && wa::sampleLoopSteps(16.0) == 64, "loop grid");
	std::printf("  LOOP places: 1/128 -> %d, 1 beat -> %d, 4 bars -> %d\n", wa::sampleLoopSteps(1.0 / 32.0), wa::sampleLoopSteps(1.0), wa::sampleLoopSteps(16.0));
}
/** Like a shaker loop (RainShake-70bpm, 16 beats): every beat a slow 40 ms swell of noise, and loud shakes on the 16ths
between (8ths 6 to 10 dB under the beats, 16ths 12 to 16 dB under; 15 ms swells) over a hiss. The beats' frames and
the count of shakes between them come back in `beatsOut` / `shakesOut`. */
static std::vector<float> shakerLoop(std::vector<long>* beatsOut, int* shakesOut) {
	const double beat = 60.0 / 70.0;
	std::vector<float> v((size_t) (16 * beat * SRC_SR), 0.f);
	uint32_t s = 777u;
	auto noise = [&]() {
		s = s * 1664525u + 1013904223u;
		return (s >> 8) * (1.f / 8388608.f) - 1.f;
	};
	auto shake = [&](double t, float amp, double attack, double decay) {
		long at = (long) std::lround(t * SRC_SR), up = (long) (attack * SRC_SR), len = up + (long) (6 * decay * SRC_SR);
		for (long i = 0; i < len && at + i < (long) v.size(); i++) {
			float env = i < up ? (float) i / (float) up : std::exp(-(float) (i - up) / (float) (decay * SRC_SR));
			v[(size_t) (at + i)] += amp * env * noise();
		}
	};
	std::vector<long> beats;
	int shakes = 0;
	for (int b = 0; b < 16; b++) {
		double t = b * beat;
		shake(t, 0.6f, 0.04, 0.08);
		beats.push_back((long) std::lround(t * SRC_SR));
		for (int k = 1; k < 4; k++) {
			shake(t + k * beat / 4, (k == 2 ? 0.2f : 0.1f) * (0.8f + 0.4f * (float) ((b * 3 + k) % 5) / 4.f), 0.015, 0.04);
			shakes++;
		}
	}
	for (size_t i = 0; i < v.size(); i++)
		v[i] += 0.005f * noise();
	*beatsOut = beats;
	*shakesOut = shakes;
	return v;
}

static void testShakerBeats() {
	std::printf("shaker beats (a 70 bpm shaker loop)\n");
	// The fewest setting finds the beats only, placed where the swell starts (not later: that cuts it); the most finds
	// the shakes too.
	std::vector<long> beats;
	int shakes = 0;
	auto bank = makeBank(shakerLoop(&beats, &shakes));
	const float settings[3] = {0.f, 0.5f, 1.f};
	int counts[3];
	for (int c = 0; c < 3; c++) {
		std::vector<long> found = wa::findTransients(bank->sample, settings[c]);
		counts[c] = (int) found.size();
		int got = 0;
		double earliest = 0.0, latest = -1e9;
		for (long b : beats) {
			double best = 1e9;
			for (long f : found)
				if (std::fabs((double) (f - b)) < std::fabs(best))
					best = (double) (f - b);
			if (std::fabs(best) < 0.03 * SRC_SR) {
				got++;
				earliest = std::min(earliest, best * 1000.0 / SRC_SR);
				latest = std::max(latest, best * 1000.0 / SRC_SR);
			}
		}
		std::printf("  %3.0f %%: %d found, %d of the 16 beats (placed %+.2f to %+.2f ms from the swell's start), %d shakes between\n",
			settings[c] * 100.f, counts[c], got, earliest, latest, shakes);
		CHECK(got == 16, "%.0f %%: %d of the 16 beats", settings[c] * 100.f, got);
		CHECK(latest < 4.0 && earliest > -4.0, "%.0f %%: beats placed %+.2f to %+.2f ms off", settings[c] * 100.f, earliest, latest);
	}
	CHECK(counts[0] == 16, "fewest finds %d, want the 16 beats", counts[0]);
	CHECK(counts[0] < counts[1] && counts[1] < counts[2] && counts[2] >= 16 + shakes * 9 / 10, "does not grade: %d %d %d (of %d)",
		counts[0], counts[1], counts[2], 16 + shakes);
}
static void testHitStarts() {
	std::printf("notes on hits start without a step (a shaker loop)\n");
	// Every hit of a shaker loop (100 %: the beats and every shake) played as a note. Noise has no quiet zero crossing:
	// a note starting at a hit at full level jumps by whatever the sample is there. With the fade-in, read from
	// PLAYER_ATTACK_SECONDS before START, the steps at the start stay those of the sound itself, and from START on the
	// hit plays exactly as it is in the sample.
	std::vector<long> beats;
	int shakes = 0;
	auto bank = makeBank(shakerLoop(&beats, &shakes));
	const wa::SampleBuffer& S = bank->sample;
	const std::vector<float>& d = S.levels[0];
	std::vector<long> hits = wa::findTransients(S, 1.f);
	const float attack = std::round((float) (wa::PLAYER_ATTACK_SECONDS * SRC_SR));
	const long a = (long) attack;
	double hardWorst = 0.0, hardMean = 0.0, fadeWorst = 0.0, fadeMean = 0.0, intact = 0.0;
	int n = 0;
	for (long hit : hits) {
		if (hit < 2 * a || hit + 4410 >= (long) S.frames)
			continue;
		wa::PlayerSettings p;
		p.start = (double) hit;
		p.end = (double) (hit + 4410);
		p.loop = wa::PLAYER_ONE;
		// At full level from START: the first sample is the jump
		double hard = std::fabs(d[(size_t) hit]);
		// With the fade: the largest step over the fade and the first 1 ms of the hit, minus the sample's own largest
		// step there (what is left is what the start adds)
		wa::PlayerVoice v;
		v.start(p, false, 240.f, attack);
		float prev = 0.f, l, r;
		double added = 0.0;
		for (long i = 0; i < 2 * a; i++) {
			v.process(S, p, &l, &r);
			long at = hit - a + i;
			double own = std::fabs(d[(size_t) at] - d[(size_t) (at - 1)]);
			added = std::max(added, std::fabs(l - prev) - own);
			prev = l;
			if (i >= a)
				intact = std::max(intact, (double) std::fabs(l - d[(size_t) at]));
		}
		hardWorst = std::max(hardWorst, hard);
		hardMean += hard;
		fadeWorst = std::max(fadeWorst, added);
		fadeMean += std::max(added, 0.0);
		n++;
	}
	hardMean /= std::max(n, 1);
	fadeMean /= std::max(n, 1);
	std::printf("  %d hits: a start at full level jumps %.4f on average (worst %.4f); with the fade the start adds %.4f (worst %.4f) to the sound's own steps\n",
		n, hardMean, hardWorst, fadeMean, fadeWorst);
	std::printf("  from START on: largest difference from the sample %.1e (limit 1e-6)\n", intact);
	CHECK(n > 40, "only %d hits", n);
	CHECK(hardWorst > 0.05, "the check cannot fail: a hard start only jumps %.4f", hardWorst);
	CHECK(fadeWorst < 0.1 * hardWorst, "the start adds %.4f (a hard start %.4f)", fadeWorst, hardWorst);
	CHECK(intact < 1e-6, "the hit is changed by %.2e", intact);
}
static void testRetriggerDecayed() {
	std::printf("a note over a decayed one (SEQ / POLY retrigger, SUSTAIN 0)\n");
	// The old note plays loud noise but has decayed to 0.1; the new one starts on a quiet 40 ms swell. The envelope
	// attacks from 0.1 back to 1 in 0.9 ms (as the ADSR does), but the old note fades out at its own level: no burst of
	// its tail while the new note is still quiet. Measured before the fix: the tail came back about 9x (+19 dB).
	std::vector<float> v((size_t) (3 * SRC_SR), 0.f);
	uint32_t s = 5u;
	for (size_t i = 0; i < v.size(); i++) {
		s = s * 1664525u + 1013904223u;
		double t = i / (double) SRC_SR;
		float env = t < 1.2 ? 0.5f : t < 1.5 ? 0.002f : (float) std::min(1.0, (t - 1.5) / 0.04) * 0.5f;
		v[i] = env * ((s >> 8) * (1.f / 8388608.f) - 1.f);
	}
	Rig rig(makeBank(v));
	rig.in.markers[0].gain = 0.3f;
	rig.in.markers[0].loopMode = wa::PLAYER_ONE;
	rig.in.markers[0].start = 0.1f; // 0.3 s: the loud part
	rig.in.voiceEnv[0] = 1.f;
	rig.in.voiceStart[0] = true;
	std::vector<float> before, after;
	long n = (long) (0.3 * 48000);
	for (long i = 0; i < n; i++) {
		rig.in.voiceEnv[0] = (float) std::exp(std::log(0.1) * i / n);
		float a, b;
		rig.step(&a, &b);
		if (i >= n - 96)
			before.push_back(a);
	}
	rig.in.markers[0].start = 0.5f; // 1.5 s: the swell
	rig.in.voiceStart[0] = true;
	float level = 0.1f;
	for (long i = 0; i < (long) (0.008 * 48000); i++) {
		level = std::min(1.f, level + 1.f / 48.f);
		rig.in.voiceEnv[0] = level;
		float a, b;
		rig.step(&a, &b);
		after.push_back(a);
	}
	double old = rms(before, 0, before.size()), worst = 0.0;
	for (size_t w = 0; w + 48 <= after.size(); w += 24)
		worst = std::max(worst, rms(after, w, w + 48));
	std::printf("  the old note just before: RMS %.4f; loudest 1 ms in the 8 ms after the retrigger %.4f (x%.2f, limit x1.5)\n",
		old, worst, worst / old);
	CHECK(worst < 1.5 * old, "the old tail comes back x%.2f", worst / old);
}
static void testGrainHitSnap() {
	std::printf("HIT snap on GRAIN\n");
	// The same as SAMPLE's: hits every 0.25 s, START at 0.4 s; with HIT the grains read from the nearest hit (0.5 s),
	// a scanning cloud's loop starts there too, and without it they read from 0.4 s.
	std::vector<float> v((size_t) (2 * SRC_SR), 0.f);
	uint32_t s = 99u;
	for (int k = 0; k < 8; k++)
		addHit(v, (long) (k * 0.25 * SRC_SR), 0.5f, 0.01, 0.04, 120.0, &s);
	for (int scan = 0; scan < 2; scan++) {
		double level[2] = {};
		for (int on = 0; on < 2; on++) {
			Rig rig(makeBank(v));
			rig.in.player = false;
			rig.in.markers[0].start = 0.2f; // 0.4 s of 2 s
			rig.in.markers[0].hitSnap = on == 1;
			rig.in.markers[0].scan = scan == 1 ? 0.3f : 0.f;
			rig.in.markers[0].scanLength = scan == 1 ? 0.05f : -1.f; // a 0.1 s loop
			std::vector<long> hits = wa::findTransients(rig.bank->sample, wa::TRANSIENT_DEFAULT_SENSITIVITY);
			for (auto& h : rig.in.hits)
				h = &hits;
			rig.note();
			std::vector<float> out;
			rig.run(0.3, &out);
			// Where the cloud reads now, and the loop the map shows (scanning)
			double at = rig.engine->getReadPosition(0) * 2.0;
			float sliceStart, sliceEnd;
			rig.engine->getScanSlice(0, &sliceStart, &sliceEnd);
			double from = scan == 1 ? sliceStart * 2.0 : at;
			std::printf("  %s, HIT %s: reads from %.4f s (loop %.4f .. %.4f s), sound RMS %.3f\n", scan ? "scanning" : "still   ",
				on ? "on " : "off", at, sliceStart * 2.0, sliceEnd * 2.0, rms(out, 0, out.size()));
			level[on] = rms(out, 0, out.size());
			if (on)
				CHECK(std::fabs(from - 0.5) < 0.002, "HIT reads from %.4f", from);
			else
				CHECK(std::fabs(from - 0.4) < 0.002, "unsnapped reads from %.4f", from);
			if (scan == 1)
				CHECK(at >= from - 0.001 && at <= from + 0.101, "the scan reads at %.4f, outside its loop from %.4f", at, from);
		}
		// 0.4 s is in the quiet tail between two hits: on the hit the grains are far louder (measured x40, x100)
		CHECK(level[1] > 10.0 * level[0], "HIT does not bring the grains onto the hit: RMS %.3f vs %.3f", level[1], level[0]);
	}
}
#include "LoopLayout.hpp"

// "Lay out the loop on the row": the tempo in a file name, the loop's beats, the row it takes, and the trigs (each on
// a hit, playing up to the next one, together covering the whole loop).
static void testLoopLayout() {
	std::printf("loop layout\n");
	{
		double a = wa::bpmFromName("RainShake-70bpm.wav"), b = wa::bpmFromName("Loop 128 BPM.wav"), c = wa::bpmFromName("bpm_90 kit.wav");
		double d = wa::bpmFromName("15 AT Tone Control Pad - Dreamy 130 Cm.wav"), e = wa::bpmFromName("kick 2.wav"), f = wa::bpmFromName("break 120.5bpm.wav");
		double g = wa::bpmFromName("take 5 bpm.wav");
		std::printf("  bpm from names: %g %g %g | none: %g %g | %g | out of range: %g\n", a, b, c, d, e, f, g);
		CHECK(a == 70.0 && b == 128.0 && c == 90.0 && d == 0.0 && e == 0.0 && f == 120.5 && g == 0.0, "bpm from file names");
	}
	{
		double sec = 4.0 * 60.0 / 70.0; // 4 beats at 70 bpm
		int named = wa::guessLoopBeats(sec, 70.0, 120.0), at120 = wa::guessLoopBeats(sec, 0.0, 120.0), at80 = wa::guessLoopBeats(sec, 0.0, 80.0);
		int pad = wa::guessLoopBeats(8.0 * 60.0 / 130.0, 0.0, 120.0);
		std::printf("  beats of a 4-beat 70 bpm loop: named %d, at 120 %d, at 80 %d; 8 beats at 130 (at 120): %d\n", named, at120, at80, pad);
		CHECK(named == 4 && at120 == 8 && at80 == 4 && pad == 8, "loop beats");
	}
	{
		struct Case {
			int beats, clock, steps;
		};
		const Case cases[] = {{8, 1, 16}, {4, 3, 16}, {2, 4, 16}, {1, 4, 8}, {16, 0, 16}, {128, -4, 16}, {3, 3, 12}, {6, 1, 12}};
		for (const Case& k : cases) {
			int ci = 99, st = 0;
			bool ok = wa::layoutRow(k.beats, 16, &ci, &st);
			CHECK(ok && ci == k.clock && st == k.steps, "%d beats: clock %d steps %d (want %d, %d)", k.beats, ci, st, k.clock, k.steps);
		}
		int ci, st;
		CHECK(!wa::layoutRow(256, 16, &ci, &st), "256 beats does not fit a row");
		std::printf("  rows: 8 beats -> 16 x2, 4 -> 16 x4, 1 -> 8 x8, 128 -> 16 /8, 3 -> 12 x4\n");
	}
	{
		// The loop: 16 steps of 1000 frames from frame 1000. A hit 2.4 steps in shares step 2 (the nearest wins); one at
		// 15.99 steps belongs to the next pass; one before the loop is not in it.
		std::vector<long> hits = {500, 1000, 3020, 3400, 8990, 16990};
		std::vector<wa::LayoutTrig> t = wa::layoutTrigs(hits, 1000, 16000, 16, false);
		long sum = 0;
		for (const wa::LayoutTrig& x : t)
			sum += x.length;
		std::printf("  on the hits: %d trigs, steps", (int) t.size());
		for (const wa::LayoutTrig& x : t)
			std::printf(" %d@%ld+%ld", x.step, x.start, x.length);
		std::printf(", lengths sum %ld of 16000\n", sum);
		CHECK(t.size() == 3 && t[0].step == 0 && t[0].start == 1000 && t[0].length == 2020 && t[1].step == 2 && t[1].start == 3020 &&
				t[1].length == 5970 && t[2].step == 8 && t[2].start == 8990 && t[2].length == 8010,
			"trigs on the hits");
		CHECK(sum == 16000, "the slices cover the loop: %ld", sum);
		// A soft hit 0.4 steps before the beat and the beat's own hit: the step takes the beat's
		std::vector<wa::LayoutTrig> v = wa::layoutTrigs({0, 1600, 2010}, 0, 16000, 16, false);
		CHECK(v.size() == 2 && v[1].step == 2 && v[1].start == 2010 && v[0].length == 2010, "the hit nearest the step (%ld)",
			v.size() > 1 ? v[1].start : -1L);
		// No hit at the start: step 1 still plays from the loop's start
		std::vector<wa::LayoutTrig> u = wa::layoutTrigs({5000}, 0, 16000, 16, false);
		CHECK(u.size() == 2 && u[0].step == 0 && u[0].start == 0 && u[0].length == 5000 && u[1].step == 5 && u[1].length == 11000,
			"step 1 from the loop's start (%d trigs)", (int) u.size());
		// Every step
		std::vector<wa::LayoutTrig> g = wa::layoutTrigs({}, 0, 1000, 4, true);
		CHECK(g.size() == 4 && g[1].start == 250 && g[3].start == 750 && g[3].length == 250, "grid trigs");
	}
}
// Shift + drag on the map: the part for LAY snaps its press to a hit and its length to whole beats at the file's tempo
static void testLaySelection() {
	std::printf("LAY selection (Shift + drag on the map)\n");
	const double sr = 48000.0, bpm = 120.0; // a beat: 24000 frames
	const long total = 48000 * 45;           // 45 s
	std::vector<long> hits = {100000, 500000, 900000};
	// Press 3000 frames after a hit (within the snap), drag about 7.6 beats right: 8 beats (2 bars) from the hit
	wa::LaySelection a = wa::laySelection(hits, total, sr, bpm, 503000, 503000 + 182000, 20000);
	std::printf("  right: from %ld, %ld frames, %d beats\n", a.from, a.frames, a.beats);
	CHECK(a.from == 500000 && a.frames == 8 * 24000 && a.beats == 8, "snapped to the hit and 8 beats");
	// Drag left from the press: the part ends at the (snapped) press; 5.2 beats -> 4
	wa::LaySelection b = wa::laySelection(hits, total, sr, bpm, 899000, 899000 - 125000, 20000);
	CHECK(b.from == 900000 - 4 * 24000 && b.frames == 4 * 24000 && b.beats == 4, "left: 4 beats ending at the hit (%ld, %ld, %d)", b.from,
		b.frames, b.beats);
	// Far from any hit: the press stays; near the end the beats halve until they fit
	long press = total - 3 * 24000;
	wa::LaySelection c = wa::laySelection(hits, total, sr, bpm, press, total, 20000);
	CHECK(c.from == press && c.beats == 2 && c.frames == 2 * 24000, "fits before the end: %d beats", c.beats);
	// No tempo: the drag as it is
	wa::LaySelection d = wa::laySelection(hits, total, sr, 0.0, 200000, 250000, 20000);
	CHECK(d.from == 200000 && d.frames == 50000 && d.beats == 0, "no tempo: the drag (%ld, %ld)", d.from, d.frames);
	// A click without a drag, with a tempo: one beat
	wa::LaySelection e = wa::laySelection(hits, total, sr, bpm, 100000, 100000, 20000);
	CHECK(e.from == 100000 && e.beats == 1 && e.frames == 24000, "a click: one beat");
	std::printf("  left: from %ld (%d beats); near the end: %d beats; no tempo: %ld frames; a click: %d beat\n", b.from, b.beats, c.beats,
		d.frames, e.beats);
}
// The tempo found in the audio (no file name needed): loops (a whole number of beats) and long files
static long tempoJitter(uint32_t* rng, long amount) {
	*rng = *rng * 1664525u + 1013904223u;
	return (long) ((double) (*rng >> 8) / 16777216.0 * 2.0 * amount) - amount;
}

/** An onset strength curve (one value per wa::ONSET_HOP frames) of a drum pattern: a strong kick / snare on each beat,
a softer hat on each 8th (swung: at `swing` of the beat), a soft ghost before every 4th beat, from `offset` frames,
+-`jitter` frames, over `frames` frames; plus a little noise everywhere */
static std::vector<float> tempoCurve(double bpm, double sr, int beats, double offset, double swing, long jitter, long frames, uint32_t seed) {
	long n = frames / wa::ONSET_HOP;
	std::vector<float> e((size_t) n, 0.f);
	uint32_t rng = seed;
	auto bump = [&](double frame, float amp) {
		double c = frame / wa::ONSET_HOP;
		for (long h = std::max(0L, (long) c - 8); h < std::min(n, (long) c + 9); h++)
			e[(size_t) h] += amp * (float) std::exp(-0.5 * (h - c) * (h - c) / 4.0);
	};
	double beat = 60.0 * sr / bpm;
	for (int b = 0; b < beats; b++) {
		double t0 = offset + b * beat;
		bump(t0 + tempoJitter(&rng, jitter), 1.f);
		bump(t0 + swing * beat + tempoJitter(&rng, jitter), 0.45f);
		if (b % 4 == 3)
			bump(t0 + 0.75 * beat + tempoJitter(&rng, jitter), 0.2f);
	}
	for (long h = 0; h < n; h++)
		e[(size_t) h] += 0.02f * (float) ((tempoJitter(&rng, 1000) + 1000) / 2000.0);
	return e;
}

static void testTempoFound() {
	std::printf("tempo found in the audio\n");
	const double sr = 44100.0;
	const int hop = wa::ONSET_HOP;
	{
		// 4 beats at 70 bpm, +-8 ms
		long frames = std::lround(4 * 60.0 / 70.0 * sr);
		std::vector<float> e = tempoCurve(70.0, sr, 4, 0.0, 0.5, 350, frames, 1);
		wa::FoundTempo a = wa::loopTempo(e, hop, 0, frames, sr, 120.0), b = wa::loopTempo(e, hop, 0, frames, sr, 80.0);
		std::printf("  loop 4 beats @ 70 (+-8 ms): at 120 -> %d beats %.1f bpm (score %.2f); at 80 -> %d beats %.1f bpm\n", a.beats, a.bpm,
			a.score, b.beats, b.bpm);
		CHECK(a.found && a.beats == 8 && b.found && b.beats == 4, "70 bpm loop: 8 beats near 120, 4 near 80");
	}
	{
		// 2 bars at 96 bpm, swung 8ths (2/3 of the beat)
		long frames = std::lround(8 * 60.0 / 96.0 * sr);
		std::vector<float> e = tempoCurve(96.0, sr, 8, 0.0, 2.0 / 3.0, 200, frames, 2);
		wa::FoundTempo a = wa::loopTempo(e, hop, 0, frames, sr, 100.0);
		std::printf("  swung loop 8 beats @ 96: %d beats %.1f bpm (score %.2f)\n", a.beats, a.bpm, a.score);
		CHECK(a.found && a.beats == 8, "swung 96 bpm loop: 8 beats (%d)", a.beats);
	}
	{
		// Noise only: no clear beat
		long frames = (long) (8.0 * sr);
		std::vector<float> e((size_t) (frames / hop));
		uint32_t rng = 9;
		for (float& v : e)
			v = (float) ((tempoJitter(&rng, 1000) + 1000) / 2000.0);
		wa::FoundTempo a = wa::loopTempo(e, hop, 0, frames, sr, 120.0), f = wa::fileTempo(e, hop, sr, 120.0);
		std::printf("  noise: loop %s (score %.2f), file %s (score %.2f)\n", a.found ? "FOUND" : "none", a.score, f.found ? "FOUND" : "none", f.score);
		CHECK(!a.found && !f.found, "noise has no tempo");
	}
	{
		// A 45 s song at 123 bpm starting 0.37 s in, +-5 ms
		long frames = (long) (45.0 * sr);
		std::vector<float> e = tempoCurve(123.0, sr, 90, 0.37 * sr, 0.5, 220, frames, 4);
		wa::FoundTempo f = wa::fileTempo(e, hop, sr, 120.0), g = wa::fileTempo(e, hop, sr, 70.0);
		std::printf("  song 45 s @ 123 from 0.37 s: %.2f bpm (score %.2f); near 70: %.2f\n", f.bpm, f.score, g.bpm);
		CHECK(f.found && std::fabs(f.bpm - 123.0) < 0.25, "song tempo %.2f", f.bpm);
		CHECK(g.found && std::fabs(g.bpm - 61.5) < 0.15, "the octave nearest 70: %.2f", g.bpm);
		// Swung, at 96
		std::vector<float> sw = tempoCurve(96.0, sr, 70, 0.2 * sr, 2.0 / 3.0, 220, frames, 5);
		wa::FoundTempo s = wa::fileTempo(sw, hop, sr, 100.0);
		std::printf("  swung song 45 s @ 96: %.2f bpm (score %.2f)\n", s.bpm, s.score);
		CHECK(s.found && std::fabs(s.bpm - 96.0) < 0.25, "swung song tempo %.2f", s.bpm);
	}
	{
		// Real audio through onsetStrength: a 2-bar loop at 100 bpm of decaying noise bursts (kick-like loud on the
		// beats, quiet hats on the 8ths) over a steady quiet hum
		double beat = 60.0 / 100.0;
		std::vector<float> src((size_t) (8 * beat * SRC_SR));
		uint32_t rng = 11;
		for (size_t i = 0; i < src.size(); i++) {
			double t = (double) i / SRC_SR;
			double inBeat = std::fmod(t, beat), in8 = std::fmod(t, beat / 2.0);
			float noise = (float) tempoJitter(&rng, 1000) / 1000.f;
			float kick = 0.8f * (float) std::exp(-inBeat / 0.06) * (float) std::sin(2 * PI * 60.0 * inBeat);
			float hat = 0.15f * (float) std::exp(-in8 / 0.02) * noise;
			src[i] = kick + hat + 0.05f * (float) std::sin(2 * PI * 110.0 * t);
		}
		auto bank = makeBank(src);
		std::vector<float> e = wa::onsetStrength(bank->sample);
		wa::FoundTempo a = wa::loopTempo(e, hop, 0, (long) bank->sample.frames, SRC_SR, 120.0);
		std::printf("  synthesized 2-bar loop @ 100: %d beats %.1f bpm (score %.2f)\n", a.beats, a.bpm, a.score);
		CHECK(a.found && a.beats == 8, "synthesized loop: 8 beats (%d)", a.beats);
	}
	CHECK(wa::sameTempo(70.0, 140.0) && wa::sameTempo(123.0, 61.6) && !wa::sameTempo(70.0, 105.0) && !wa::sameTempo(0.0, 70.0), "same tempo up to octaves");
}
// LAY's velocities: a hit's peak in the sample against the loop's loudest, gently in dB
static void testLayVelocity() {
	std::printf("LAY velocities from the hits\n");
	float v0 = wa::layVelocity(0.0), v12 = wa::layVelocity(12.0), v30 = wa::layVelocity(30.0), v60 = wa::layVelocity(60.0), vneg = wa::layVelocity(-3.0);
	std::printf("  0 dB under: %.2f, 12 dB: %.2f, 30 dB: %.2f, 60 dB: %.2f\n", v0, v12, v30, v60);
	CHECK(v0 == 1.f && std::fabs(v12 - 0.72f) < 0.005f && std::fabs(v30 - wa::LAY_VELOCITY_MIN) < 1e-6f && v60 == v30 && vneg == 1.f, "velocity mapping");
	// Peaks of a loud and a soft burst (a quarter of the level: 12 dB under)
	std::vector<float> src((size_t) SRC_SR, 0.f);
	for (int i = 0; i < 2000; i++) {
		src[(size_t) (1000 + i)] = 0.8f * (float) std::sin(2 * PI * 200.0 * i / SRC_SR);
		src[(size_t) (20000 + i)] = 0.2f * (float) std::sin(2 * PI * 200.0 * i / SRC_SR);
	}
	auto bank = makeBank(src);
	long w = (long) (wa::LAY_PEAK_SECONDS * SRC_SR);
	double loud = wa::hitPeakDb(bank->sample, 1000, w), soft = wa::hitPeakDb(bank->sample, 20000, w);
	std::printf("  bursts: loud %.1f dB, soft %.1f dB -> velocity %.2f\n", loud, soft, wa::layVelocity(loud - soft));
	CHECK(std::fabs((loud - soft) - 12.04) < 0.3, "the soft burst is 12 dB under (%.2f)", loud - soft);
	CHECK(wa::hitPeakDb(bank->sample, 40000, w) <= -199.0, "silence has no peak");
}
int main() {
	testLayVelocity();
	testTempoFound();
	testLaySelection();
	testLoopLayout();
	testGrainHitSnap();
	testRetriggerDecayed();
	testHitStarts();
	testShakerBeats();
	testBeatShortest();
	testBeatPitch();
	testHitsSlider();
	testBeatCut();
	testDenseHits();
	testHitSnap();
	testTransientSensitivity();
	testTransients();
	testZeroSnap();
	testBeat();
	testLoopPoint();
	testDrive();
	testVoiceExact();
	testEngineExact();
	testPitch();
	testLoopAndEnds();
	testNotes();
	testPitchSweep();
	testMorph();
	testCpu();
	if (failures)
		std::printf("%d FAILED\n", failures);
	else
		std::printf("all checks passed\n");
	return failures ? 1 : 0;
}
