// Offline tests for Nur Prisma's DSP (no Rack). Each check prints the measured value next to its limit.
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include "PrismaEngine.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace np;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* name, const char* fmt, double a, double b = 0.0) {
	checks++;
	if (!ok)
		failures++;
	std::printf("%s %-46s ", ok ? "ok  " : "FAIL", name);
	std::printf(fmt, a, b);
	std::printf("\n");
}

static const float SR = 48000.f;

static float cutoffKnob(float hz) {
	return (std::log2(hz) - LOG2_20) / LOG2_1000;
}

/** Frequency from upward zero crossings (linear interpolation) of x[start, end) */
static double zeroCrossFreq(const std::vector<float>& x, size_t start, size_t end) {
	double first = -1.0, last = -1.0;
	int count = 0;
	for (size_t n = start + 1; n < end && n < x.size(); n++) {
		if (x[n - 1] < 0.f && x[n] >= 0.f) {
			double t = (n - 1) + x[n - 1] / (double) (x[n - 1] - x[n]);
			if (first < 0.0)
				first = t;
			last = t;
			count++;
		}
	}
	if (count < 2)
		return 0.0;
	return (count - 1) * SR / (last - first);
}

/** Amplitude of the `hz` component of x[start, end) */
static double goertzel(const std::vector<float>& x, size_t start, size_t end, double hz) {
	double re = 0.0, im = 0.0;
	for (size_t n = start; n < end; n++) {
		double ph = 2.0 * M_PI * hz * n / SR;
		re += x[n] * std::cos(ph);
		im += x[n] * std::sin(ph);
	}
	return 2.0 * std::sqrt(re * re + im * im) / (end - start);
}

static double db(double x) {
	return 20.0 * std::log10(std::max(x, 1e-12));
}

/** An engine with the character and motion off: a clean set of four identical-ish cores. */
struct Rig {
	PrismaEngine e;
	EngineOutput o;
	Rig() {
		e.setSampleRate(SR);
		EngineParams& p = e.p;
		p.analog = 0.f;
		p.width = 0.f;
		p.noise = 0.f;
		p.digital = 0.f;
		p.drive = 0.f;
		p.strike = 0.f;
		p.levelWeight = 0;
	}
	void step(float l = 0.f, float r = 0.f) {
		float a[4] = {l, l, l, l}, b[4] = {r, r, r, r};
		e.process(a, b, false, l, r, o);
	}
	void stepPoly(const float* in) {
		e.process(in, in, true, 0.f, 0.f, o);
	}
};

// ---------------------------------------------------------------------------------------------------------------

static void testCore() {
	// BP unity at the centre (RES 0, Q 0.7, BP gain 1)
	{
		float w[4] = {0.f, 1.f, 0.f, 0.f};
		Core c;
		float fc = 1000.f;
		CoreCoefs cc = coreCoefs(fc, 0.f, 1.f, SR);
		std::vector<float> y;
		for (int n = 0; n < 48000; n++)
			y.push_back(c.process(std::sin(2.f * (float) M_PI * fc * n / SR), cc.g, cc.k, w, cc.bpGain, cc.fb, 0.f));
		double a = goertzel(y, 24000, 48000, fc);
		check(std::fabs(db(a)) < 0.1, "core: BP gain at centre (dB)", "%.3f (limit +-0.1)", db(a));
	}
	// LP passband stays at 1 whatever RES (resonance never takes level away)
	for (float res : {0.f, 0.5f, 0.85f}) {
		float w[4] = {1.f, 0.f, 0.f, 0.f};
		Core c;
		CoreCoefs cc = coreCoefs(5000.f, res, 1.f, SR);
		std::vector<float> y;
		for (int n = 0; n < 48000; n++)
			y.push_back(c.process(std::sin(2.f * (float) M_PI * 100.f * n / SR), cc.g, cc.k, w, cc.bpGain, cc.fb, 0.f));
		double a = goertzel(y, 24000, 48000, 100.0);
		char name[64];
		std::snprintf(name, sizeof name, "core: LP passband at RES %.2f (dB)", res);
		check(std::fabs(db(a)) < 0.3, name, "%.3f (limit +-0.3)", db(a));
	}
	// The integrator knee is exactly linear below 40% of the limit
	{
		bool lin = true;
		for (float x = -0.8f; x <= 0.8f; x += 0.01f)
			lin = lin && softLimit(x, 2.f) == x;
		check(lin, "core: knee linear below 40% of limit", "%.0f", lin ? 1.0 : 0.0);
	}
	// RES map: musical middle, 300 at the start of the feedback zone
	check(std::fabs(resQ(0.5f) - 4.2f) < 0.3f, "core: Q at RES 50%", "%.2f (expect ~4.2)", resQ(0.5f));
	check(std::fabs(resQ(RES_FB_START) - Q_MAX) < 0.5f, "core: Q at RES 92%", "%.2f (expect 300)", resQ(RES_FB_START));
}

// ---------------------------------------------------------------------------------------------------------------

/** Ping all cores (SPREAD 0: one pitch) and record CORES[0] */
static std::vector<float> pingRecord(float hz, float res, int samples, float pitchCv = 0.f) {
	Rig r;
	r.e.p.cutoff = cutoffKnob(hz);
	r.e.p.spread = 0.f;
	r.e.p.res = res;
	for (int i = 0; i < NUM_CORES; i++)
		r.e.p.pitchCv[i] = pitchCv;
	r.step();
	r.e.pingAll(1.f);
	std::vector<float> y;
	for (int n = 0; n < samples; n++) {
		r.step();
		y.push_back(r.o.cores[0]);
	}
	return y;
}

static void testPing() {
	// Ring pitch = CUTOFF within +-3 cents, 50 Hz .. 5 kHz
	double worst = 0.0;
	for (float hz : {50.f, 110.f, 220.f, 440.f, 1000.f, 2500.f, 5000.f}) {
		std::vector<float> y = pingRecord(hz, 0.9f, 48000);
		double f = zeroCrossFreq(y, 480, std::min<size_t>(48000, 480 + (size_t) (SR * 400.f / hz)));
		double cents = 1200.0 * std::log2(f / hz);
		worst = std::max(worst, std::fabs(cents));
	}
	check(worst < 3.0, "ping: ring pitch error, 50 Hz..5 kHz (cents)", "%.3f (limit 3)", worst);

	// V/OCT: +1 V = exactly one octave
	{
		std::vector<float> a = pingRecord(220.f, 0.9f, 24000);
		std::vector<float> b = pingRecord(220.f, 0.9f, 24000, 1.f);
		double fa = zeroCrossFreq(a, 480, 24000), fb = zeroCrossFreq(b, 480, 24000);
		double cents = 1200.0 * std::log2(fb / fa) - 1200.0;
		check(std::fabs(cents) < 0.5, "ping: V/OCT +1 V (cents off an octave)", "%.3f (limit 0.5)", cents);
	}

	// Ring loudness: the same over pitch and RES
	double lo = 1e9, hi = 0.0;
	for (float hz : {110.f, 440.f, 1760.f})
		for (float res : {0.75f, 0.85f, 0.92f}) {
			std::vector<float> y = pingRecord(hz, res, 2400);
			double peak = 0.0;
			for (float v : y)
				peak = std::max(peak, (double) std::fabs(v));
			lo = std::min(lo, peak);
			hi = std::max(hi, peak);
		}
	check(db(hi / lo) < 4.0, "ping: loudness spread over pitch and RES (dB)", "%.2f (limit 4)", db(hi / lo));
	check(std::fabs(db(hi / PrismaEngine::PING_LEVEL)) < 2.0, "ping: loudest ring vs PING_LEVEL (dB)", "%.2f (limit +-2)",
		db(hi / PrismaEngine::PING_LEVEL));

	// STRIKE: the felt darkens high notes smoothly; a mid note at the default STRIKE rings nearly as loud as a click
	{
		auto peakWith = [](float hz, float strike) {
			Rig r;
			r.e.p.cutoff = cutoffKnob(hz);
			r.e.p.spread = 0.f;
			r.e.p.res = 0.85f;
			r.e.p.strike = strike;
			r.step();
			r.e.pingAll(1.f);
			double peak = 0.0;
			for (int n = 0; n < 4800; n++) {
				r.step();
				peak = std::max(peak, (double) std::fabs(r.o.cores[0]));
			}
			return peak;
		};
		double mid = db(peakWith(630.f, 0.35f) / peakWith(630.f, 0.f));
		check(mid > -4.0, "ping: 630 Hz at STRIKE 35% vs click (dB)", "%.2f (limit -4)", mid);
		// Even, no dead notes: every semitone 400..1600 Hz within a smooth trend (neighbours within 1 dB)
		double worstJump = 0.0, prev = 0.0;
		for (int s = 0; s <= 24; s++) {
			double hz = 400.0 * std::exp2(s / 12.0);
			double d = db(peakWith((float) hz, 0.35f));
			if (s > 0)
				worstJump = std::max(worstJump, std::fabs(d - prev));
			prev = d;
		}
		check(worstJump < 1.0, "ping: STRIKE 35% level step between semitones (dB)", "%.3f (limit 1)", worstJump);
		double soft = db(peakWith(3000.f, 0.5f) / peakWith(3000.f, 0.f));
		check(soft < -6.0, "ping: soft STRIKE darkens 3 kHz (dB)", "%.2f (limit -6)", soft);
	}

	// RES 90% decays; RES 100% sustains (self-oscillation) at a safe level
	{
		std::vector<float> y = pingRecord(440.f, 0.9f, 4 * 48000);
		double tail = 0.0;
		for (size_t n = y.size() - 4800; n < y.size(); n++)
			tail = std::max(tail, (double) std::fabs(y[n]));
		check(tail < 0.01, "ping: RES 90% has died away after 4 s", "%.5f (limit 0.01)", tail);
	}
	{
		std::vector<float> y = pingRecord(440.f, 1.f, 3 * 48000);
		double tail = 0.0;
		for (size_t n = y.size() - 9600; n < y.size(); n++)
			tail = std::max(tail, (double) std::fabs(y[n]));
		check(tail > 0.1 && tail < 1.2, "ping: RES 100% sustains (peak, 1.0 = 5 V)", "%.3f (0.1 .. 1.2)", tail);
		double f = zeroCrossFreq(y, y.size() - 48000, y.size());
		double cents = 1200.0 * std::log2(f / 440.0);
		check(std::fabs(cents) < 5.0, "ping: self-oscillation pitch (cents)", "%.3f (limit 5)", cents);
	}

	// STRUM: cores start exactly one gap apart, in pitch order (up), or reversed (down)
	for (float strum : {1.f, -1.f}) {
		Rig r;
		r.e.p.shape = SHAPE_CHORD;
		r.e.p.chord = 5; // Major: 0 4 7 12
		r.e.p.spread = 0.f;
		r.e.p.res = 0.8f;
		r.e.p.strum = strum;
		r.step();
		r.e.pingAll(1.f);
		int onset[NUM_CORES] = {-1, -1, -1, -1};
		for (int n = 0; n < 48000; n++) {
			r.step();
			for (int i = 0; i < NUM_CORES; i++)
				if (onset[i] < 0 && std::fabs(r.o.cores[i]) > 1e-6f)
					onset[i] = n;
		}
		int gap = (int) (0.15f * SR);
		bool ok = true;
		for (int i = 0; i < NUM_CORES; i++) {
			int k = strum > 0.f ? i : NUM_CORES - 1 - i;
			ok = ok && onset[i] - onset[strum > 0.f ? 0 : 3] == k * gap;
		}
		check(ok, strum > 0.f ? "ping: strum up, exact spacing" : "ping: strum down, exact spacing", "%.0f %.0f",
			(double) (onset[1] - onset[0]), (double) gap);
	}

	// CYCLE: each trigger pings the next core in pitch order
	{
		Rig r;
		r.e.p.shape = SHAPE_CHORD;
		r.e.p.chord = 5;
		r.e.p.spread = 0.f;
		r.e.p.res = 0.4f;
		r.e.p.pingMode = PING_CYCLE;
		r.step();
		int got[5];
		for (int t = 0; t < 5; t++) {
			r.e.pingAll(1.f);
			double peak[NUM_CORES] = {};
			for (int n = 0; n < 4800; n++) {
				r.step();
				for (int i = 0; i < NUM_CORES; i++)
					peak[i] = std::max(peak[i], (double) std::fabs(r.o.cores[i]));
			}
			int best = 0;
			for (int i = 1; i < NUM_CORES; i++)
				if (peak[i] > peak[best])
					best = i;
			got[t] = best;
		}
		bool ok = got[0] == 0 && got[1] == 1 && got[2] == 2 && got[3] == 3 && got[4] == 0;
		check(ok, "ping: cycle mode order 1 2 3 4 1", "%.0f%.0f...", (double) got[0], (double) got[1]);
	}

	// Poly PING: channel i rings only core i
	{
		Rig r;
		r.e.p.res = 0.85f;
		r.e.p.spread = 0.5f;
		r.step();
		r.e.ping(2, 1.f);
		double peak[NUM_CORES] = {};
		for (int n = 0; n < 4800; n++) {
			r.step();
			for (int i = 0; i < NUM_CORES; i++)
				peak[i] = std::max(peak[i], (double) std::fabs(r.o.cores[i]));
		}
		check(peak[2] > 0.3 && peak[0] == 0.0 && peak[1] == 0.0 && peak[3] == 0.0, "ping: poly channel 3 rings core 3 only",
			"%.3f", peak[2]);
	}
}

// ---------------------------------------------------------------------------------------------------------------

static void testPlaces() {
	float rnd[4] = {};
	float o[4];
	spreadOffsets(SHAPE_CHORD, 0.f, 0, 5, rnd, o);
	check(std::fabs(o[1] * 12 - 4) < 1e-4 && std::fabs(o[2] * 12 - 7) < 1e-4 && std::fabs(o[3] * 12 - 12) < 1e-4,
		"shape: Chord major = 0 4 7 12", "%.2f %.2f", o[1] * 12, o[2] * 12);
	spreadOffsets(SHAPE_CHORD, 1.f / 8.f, 0, 5, rnd, o);
	check(std::fabs(o[0] * 12 - 12) < 1e-4, "shape: Chord first inversion lifts the root", "%.2f", o[0] * 12);
	spreadOffsets(SHAPE_SCALE, 2.f / 7.f, 0, 0, rnd, o);
	check(std::fabs(o[1] * 12 - 4) < 1e-4 && std::fabs(o[2] * 12 - 7) < 1e-4 && std::fabs(o[3] * 12 - 11) < 1e-4,
		"shape: Scale major, every 2nd degree = maj7", "%.2f %.2f", o[3] * 12, 11.0);
	spreadOffsets(SHAPE_HARMONIC, 0.5f, 0, 0, rnd, o);
	check(std::fabs(std::exp2(o[2]) - 3.0) < 1e-4, "shape: Harmonic at 50% = harmonic 3", "%.4f", std::exp2(o[2]));
	spreadOffsets(SHAPE_EVEN, 0.f, 0, 0, rnd, o);
	check(o[0] == 0.f && o[3] == 0.f, "shape: SPREAD 0 puts all cores on CUTOFF", "%.3f", o[3]);
	spreadOffsets(SHAPE_EVEN, 1.f, 0, 0, rnd, o);
	check(std::fabs(o[3] - o[0] - 5.f) < 1e-4, "shape: Even full SPREAD spans 5 octaves", "%.3f", o[3] - o[0]);
	check(std::fabs(quantizeToScale(0, 1.4f) - 2.f) < 1e-4, "scale: quantize 1.4 semis (major) -> 2", "%.2f",
		quantizeToScale(0, 1.4f));

	// Engine: CUTOFF and V/OCT reach the cores; poly V/OCT moves only its core
	Rig r;
	r.e.p.cutoff = cutoffKnob(440.f);
	r.e.p.spread = 0.f;
	r.step();
	check(std::fabs(std::exp2(r.e.oct[0]) - 440.0) < 0.01, "engine: CUTOFF -> core frequency (Hz)", "%.3f",
		std::exp2(r.e.oct[0]));
	r.e.p.pitchCv[2] = 1.f;
	for (int n = 0; n < PrismaEngine::BLOCK; n++)
		r.step();
	check(std::fabs(r.e.oct[2] - r.e.oct[0] - 1.f) < 1e-4 && r.e.oct[1] == r.e.oct[0], "engine: poly V/OCT moves only its core",
		"%.4f", r.e.oct[2] - r.e.oct[0]);

	// Each core's own spread: red 100%, yellow and green 0% (on CUTOFF), violet 50%, -100% mirrors
	{
		Rig q;
		q.e.p.cutoff = cutoffKnob(440.f);
		q.e.p.spread = 1.f;
		const float amounts[4] = {1.f, 0.f, 0.f, 0.5f};
		for (int i = 0; i < 4; i++)
			q.e.p.coreSpread[i] = amounts[i];
		q.step();
		float base = std::log2(440.f);
		bool ok = std::fabs(q.e.oct[0] - base + 2.5f) < 1e-4f && std::fabs(q.e.oct[1] - base) < 1e-4f &&
			std::fabs(q.e.oct[2] - base) < 1e-4f && std::fabs(q.e.oct[3] - base - 1.25f) < 1e-4f;
		check(ok, "core spread: red 100, yellow 0, green 0, violet 50", "%.3f %.3f oct", q.e.oct[0] - base,
			q.e.oct[3] - base);
		q.e.p.coreSpread[0] = -1.f;
		for (int n = 0; n < 48000; n++)
			q.step();
		check(std::fabs(q.e.oct[0] - base - 2.5f) < 1e-3f, "core spread: -100% mirrors to the other side", "%.3f oct",
			q.e.oct[0] - base);
	}

	// Voice routing: 6 voices wrap (voice 5 joins core 1)
	float v[6] = {1, 2, 3, 4, 10, 20}, c[4];
	routeVoices(v, 6, c);
	check(c[0] == 11 && c[1] == 22 && c[2] == 3 && c[3] == 4, "poly: 6 voices wrap onto cores 1 and 2", "%.0f %.0f", c[0], c[1]);
}

static void testPoly() {
	// Four voices, filters wide open: each core carries only its own voice
	Rig r;
	r.e.p.cutoff = 1.f;
	r.e.p.res = 0.f;
	r.e.p.spread = 0.f;
	const double hz[4] = {200.0, 300.0, 450.0, 700.0};
	std::vector<float> y[4];
	for (int n = 0; n < 48000; n++) {
		float in[4];
		for (int i = 0; i < 4; i++)
			in[i] = 0.5f * (float) std::sin(2.0 * M_PI * hz[i] * n / SR);
		r.stepPoly(in);
		for (int i = 0; i < 4; i++)
			y[i].push_back(r.o.cores[i]);
	}
	double worst = -1e9;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			if (i != j)
				worst = std::max(worst, db(goertzel(y[i], 24000, 48000, hz[j]) / goertzel(y[i], 24000, 48000, hz[i])));
	check(worst < -60.0, "poly: other voices in a core (dB)", "%.1f (limit -60)", worst);
}

// ---------------------------------------------------------------------------------------------------------------

static void testStereo() {
	// WIDTH 0 + mono input: L == R exactly, with everything else on
	{
		Rig r;
		EngineParams& p = r.e.p;
		p.width = 0.f;
		p.analog = 1.f;
		p.noise = 0.5f;
		p.digital = 0.3f;
		p.drive = 0.5f;
		p.res = 0.8f;
		p.spread = 0.6f;
		bool same = true;
		for (int n = 0; n < 96000; n++) {
			if (n % 9000 == 0)
				r.e.pingAll(0.8f);
			float x = 0.4f * std::sin(2.f * (float) M_PI * 330.f * n / SR);
			r.step(x, x);
			same = same && r.o.l == r.o.r;
		}
		check(same, "stereo: WIDTH 0 mono in -> L == R exactly", "%.0f", same ? 1.0 : 0.0);
	}
	// WIDTH 1: the mono sum keeps its level (no big cancellation)
	{
		double e[2] = {};
		for (int k = 0; k < 2; k++) {
			Rig r;
			r.e.p.width = (float) k;
			r.e.p.spread = 0.5f;
			r.e.p.res = 0.6f;
			Rng rng;
			for (int n = 0; n < 96000; n++) {
				float x = 0.3f * rng.uniform();
				r.step(x, x);
				if (n > 48000) {
					double m = 0.5 * (r.o.l + r.o.r);
					e[k] += m * m;
				}
			}
		}
		double loss = 10.0 * std::log10(e[1] / e[0]);
		check(loss > -3.0, "stereo: mono sum at WIDTH 1 vs 0 (dB)", "%.2f (limit -3)", loss);
	}
}

static void testStack() {
	// Level stays within +-3 dB from parallel to series (gain compensation), and the morph is click-free
	double rms[2] = {};
	for (int k = 0; k < 2; k++) {
		Rig r;
		r.e.p.cutoff = 0.75f;
		r.e.p.res = 0.35f;
		r.e.p.spread = 0.3f;
		r.e.p.stack = (float) k;
		Rng rng;
		double e = 0.0;
		for (int n = 0; n < 4 * 48000; n++) {
			float x = 0.3f * rng.uniform();
			r.step(x, x);
			if (n > 3 * 48000)
				e += r.o.l * r.o.l;
		}
		rms[k] = std::sqrt(e / 48000);
	}
	check(std::fabs(db(rms[1] / rms[0])) < 3.0, "stack: series vs parallel level (dB)", "%.2f (limit +-3)", db(rms[1] / rms[0]));

	// Morph sweep on a sine: no step bigger than the largest seen at either end
	Rig r;
	r.e.p.cutoff = 0.7f;
	r.e.p.spread = 0.4f;
	r.e.p.res = 0.5f;
	float prev = 0.f, steady = 0.f, sweep = 0.f;
	for (int n = 0; n < 3 * 48000; n++) {
		float t = (n - 48000) / 48000.f;
		r.e.p.stack = std::min(std::max(t, 0.f), 1.f);
		float x = 0.5f * std::sin(2.f * (float) M_PI * 220.f * n / SR);
		r.step(x, x);
		float d = std::fabs(r.o.l - prev);
		prev = r.o.l;
		if (n > 24000 && n < 48000)
			steady = std::max(steady, d);
		if (n >= 48000 && n < 96000)
			sweep = std::max(sweep, d);
	}
	check(sweep < 2.f * steady, "stack: morph step vs steady step", "%.4f vs %.4f", sweep, steady);
}

// ---------------------------------------------------------------------------------------------------------------

static void testNoise() {
	// Each colour about the same loudness (RMS 0.3 +-2 dB) and the right slope
	double rms[4];
	double slope[4];
	for (int c = 0; c < 4; c++) {
		ColoredNoise nz;
		std::vector<float> y;
		for (int n = 0; n < 10 * 48000; n++)
			y.push_back(nz.process(c / 3.f));
		double e = 0.0;
		for (float v : y)
			e += v * v;
		rms[c] = std::sqrt(e / y.size());
		// Band levels at 250 Hz and 2 kHz (averaged over 0.1 s blocks)
		double a = 0.0, b = 0.0;

		for (size_t s = 0; s + 4800 <= y.size(); s += 4800) {
			double ga = goertzel(y, s, s + 4800, 250.0), gb = goertzel(y, s, s + 4800, 2000.0);
			a += ga * ga;
			b += gb * gb;
		}
		slope[c] = 10.0 * std::log10(b / a) / 3.0; // dB per octave
	}
	const char* names[4] = {"white", "pink", "brown", "crackle"};
	for (int c = 0; c < 4; c++) {
		char name[64];
		std::snprintf(name, sizeof name, "noise: %s RMS (dB re 0.3)", names[c]);
		check(std::fabs(db(rms[c] / 0.3)) < 2.0, name, "%.2f (limit +-2)", db(rms[c] / 0.3));
	}
	check(std::fabs(slope[0]) < 1.0, "noise: white slope (dB/oct)", "%.2f (expect 0)", slope[0]);
	check(std::fabs(slope[1] + 3.0) < 1.0, "noise: pink slope (dB/oct)", "%.2f (expect -3)", slope[1]);
	check(std::fabs(slope[2] + 6.0) < 1.0, "noise: brown slope (dB/oct)", "%.2f (expect -6)", slope[2]);
}

static void testSafety() {
	// Everything at full, loud input, pings, poly: never above 10 V, never NaN
	for (float dig : {0.f, 1.f}) {
		PrismaEngine e;
		e.setSampleRate(SR);
		EngineParams& p = e.p;
		p.res = 1.f;
		p.drive = 1.f;
		p.noise = 1.f;
		p.analog = 1.f;
		p.digital = dig;
		p.width = 1.f;
		p.stack = 0.5f;
		p.type = 1.5f;
		EngineOutput o;
		double peak = 0.0;
		bool finite = true;
		for (int n = 0; n < 4 * 48000; n++) {
			if (n % 3000 == 0)
				e.pingAll(1.f);
			float s = (n / 50) % 2 ? 2.f : -2.f;
			float in[4] = {s, -s, s, s};
			e.process(in, in, true, s, s, o);
			finite = finite && std::isfinite(o.l) && std::isfinite(o.r);
			peak = std::max(peak, (double) std::max(std::fabs(o.l), std::fabs(o.r)));
		}
		check(finite && peak <= 2.0, dig > 0.f ? "safety: all max + DIGITAL, peak (V)" : "safety: all max, peak (V)",
			"%.3f (limit 10)", peak * 5.0);
	}
	// Same seeds, same sound
	{
		double diff = 0.0;
		PrismaEngine a, b;
		a.setSampleRate(SR);
		b.setSampleRate(SR);
		a.seedCharacter(42);
		b.seedCharacter(42);
		a.p.analog = b.p.analog = 1.f;
		EngineOutput oa, ob;
		for (int n = 0; n < 48000; n++) {
			float x = 0.3f * std::sin(0.05f * n);
			float in[4] = {x, x, x, x};
			a.process(in, in, false, x, x, oa);
			b.process(in, in, false, x, x, ob);
			diff = std::max(diff, (double) std::fabs(oa.l - ob.l));
		}
		check(diff == 0.0, "character: same seed, identical output", "%.3g", diff);
	}
	// Drive latency is the same as the plain delay it stands in for
	{
		DriveStage d;
		int at = -1;
		float best = 0.f;
		for (int n = 0; n < 2000; n++) {
			float x = (n == 1000) ? 0.05f : 0.f;
			float y = d.process(x, 0.3f, 48000);
			if (n >= 1000 && std::fabs(y) > best) {
				best = std::fabs(y);
				at = n - 1000;
			}
		}
		check(at == DriveStage::DRIVE_LATENCY, "drive: latency (samples)", "%.0f (expect %.0f)", at, DriveStage::DRIVE_LATENCY);
	}
}

/** Gain of core 1 (CORES out) for a sine at `hz`, cores on 1 kHz (TYPE BP, RES 0), BAND `band` */
static double bandResponse(float band, double hz) {
	Rig r;
	r.e.p.cutoff = cutoffKnob(1000.f);
	r.e.p.spread = 0.f;
	r.e.p.res = 0.f;
	r.e.p.type = 1.f;
	r.e.p.band = band;
	std::vector<float> y;
	for (int n = 0; n < 48000; n++) {
		float x = 0.2f * (float) std::sin(2.0 * M_PI * hz * n / SR);
		r.step(x, x);
		y.push_back(r.o.cores[0]);
	}
	return goertzel(y, 24000, 48000, hz) / 0.2;
}

static void testBand() {
	for (float band : {0.5f, 1.f}) {
		double oct = bandOctaves(band);
		double centre = bandResponse(band, 1000.0);
		double lo = bandResponse(band, 1000.0 * std::exp2(-oct / 2)), hi = bandResponse(band, 1000.0 * std::exp2(oct / 2));
		char name[80];
		std::snprintf(name, sizeof name, "band %.0f%%: centre gain vs make-up (dB)", band * 100);
		check(std::fabs(db(centre / bandMakeup((float) oct))) < 0.5, name, "%.2f (limit +-0.5)", db(centre / bandMakeup((float) oct)));
		std::snprintf(name, sizeof name, "band %.0f%%: edges of a %.3f-oct band (dB)", band * 100, oct);
		check(std::fabs(db(lo / centre) + 3.0) < 1.0 && std::fabs(db(hi / centre) + 3.0) < 1.0, name, "%.2f / %.2f (expect -3)",
			db(lo / centre), db(hi / centre));
	}
	double rej = db(bandResponse(1.f, 2000.0) / bandResponse(1.f, 1000.0));
	check(rej < -40.0, "band 100%: one octave away (dB)", "%.1f (limit -40)", rej);
	double wide = db(bandResponse(0.f, 2000.0) / bandResponse(0.f, 1000.0));
	check(wide > -12.0, "band 0: the plain core one octave away (dB)", "%.1f (gentle, > -12)", wide);

	// A ping still rings through the narrowest band (the ring is at its centre; the band builds up over ~16 ms, so the
	// first peak is a little lower)
	double peak[2];
	for (int k = 0; k < 2; k++) {
		Rig r;
		r.e.p.cutoff = cutoffKnob(440.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.85f;
		r.e.p.band = (float) k;
		r.step();
		r.e.pingAll(1.f);
		peak[k] = 0.0;
		for (int n = 0; n < 24000; n++) {
			r.step();
			peak[k] = std::max(peak[k], (double) std::fabs(r.o.cores[0]));
		}
	}
	check(std::fabs(db(peak[1] / peak[0])) < 5.0, "band 100%: ping rings about as loud (dB)", "%.2f (limit +-5)",
		db(peak[1] / peak[0]));
}

/** Output vs input RMS (dB) on pink noise after the compensation has settled */
static double levelChange(void (*setup)(EngineParams&), int comp) {
	Rig r;
	r.e.p.levelComp = comp;
	setup(r.e.p);
	ColoredNoise nl, nr;
	nr.rng.seed(77);
	double ein = 0.0, eout = 0.0;
	for (int n = 0; n < 5 * 48000; n++) {
		float a = nl.process(1.f / 3.f), b = nr.process(1.f / 3.f);
		r.step(a, b);
		if (n > 3 * 48000) {
			ein += 0.5 * (a * a + b * b);
			eout += 0.5 * (r.o.l * r.o.l + r.o.r * r.o.r);
		}
	}
	return 10.0 * std::log10(eout / ein);
}

static void testLevel() {
	struct Case {
		const char* name;
		void (*setup)(EngineParams&);
	};
	const Case cases[] = {
		{"BAND 100% (BP, spread)", [](EngineParams& p) { p.type = 1.f; p.band = 1.f; p.spread = 0.5f; }},
		{"BAND 60% on LP", [](EngineParams& p) { p.band = 0.6f; p.spread = 0.3f; }},
		{"narrow BP (RES 90%)", [](EngineParams& p) { p.type = 1.f; p.res = 0.9f; p.spread = 0.4f; }},
		{"notch, RES 80%", [](EngineParams& p) { p.type = 3.f; p.res = 0.8f; }},
		{"HP high (5 kHz)", [](EngineParams& p) { p.type = 2.f; p.cutoff = cutoffKnob(5000.f); p.spread = 0.f; }},
		{"LP low (200 Hz)", [](EngineParams& p) { p.cutoff = cutoffKnob(200.f); p.spread = 0.f; }},
	};
	for (const Case& c : cases) {
		double d = levelChange(c.setup, LEVEL_FULL);
		char name[80];
		std::snprintf(name, sizeof name, "level (full): %s (dB)", c.name);
		check(std::fabs(d) < 2.0, name, "%.2f (limit +-2)", d);
	}
	double off = levelChange(cases[0].setup, LEVEL_OFF);
	check(off < -8.0, "level (off): BAND 100% really loses level (dB)", "%.2f (expect < -8)", off);
	double gentle = levelChange(cases[0].setup, LEVEL_GENTLE);
	check(gentle > off + 5.5 && gentle < off + 6.5, "level (gentle): BAND 100% gets +6 dB (dB)", "%.2f (off %.2f)", gentle, off);

	// After the input stops, the gain holds through a gap, then goes back to 0 dB (noise alone is not left boosted)
	Rig r;
	r.e.p.type = 1.f;
	r.e.p.band = 1.f;
	r.e.p.spread = 0.5f;
	ColoredNoise nz;
	for (int n = 0; n < 3 * 48000; n++) {
		float a = nz.process(1.f / 3.f);
		r.step(a, a);
	}
	float boosted = r.e.levelCompDb();
	for (int n = 0; n < 48000; n++)
		r.step();
	float held = r.e.levelCompDb();
	for (int n = 0; n < 7 * 48000; n++)
		r.step();
	float relaxed = r.e.levelCompDb();
	check(boosted > 6.f && std::fabs(held - boosted) < 1.f && std::fabs(relaxed) < 1.f,
		"level: holds 1 s of silence, then back to 0 dB", "%.2f -> %.2f dB", boosted, held);
	std::printf("     (after 8 s of silence: %.2f dB)\n", relaxed);
}

static void testPerCore() {
	// Own RES: core 2 rings long (90%) while the others follow the global 50%
	{
		Rig r;
		r.e.p.cutoff = cutoffKnob(440.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.5f;
		r.e.p.ownRes[1] = true;
		r.e.p.coreRes[1] = 0.9f;
		r.step();
		r.e.pingAll(1.f);
		double tail[4] = {};
		for (int n = 0; n < 24000; n++) {
			r.step();
			if (n > 21600)
				for (int i = 0; i < 4; i++)
					tail[i] = std::max(tail[i], (double) std::fabs(r.o.cores[i]));
		}
		check(tail[1] > 0.02 && tail[0] < 1e-3 && tail[2] < 1e-3 && tail[3] < 1e-3, "per core: own RES rings on, others follow",
			"%.3f vs %.5f", tail[1], tail[0]);
	}
	// Own TYPE: core 4 high-pass, the others low-pass: a 100 Hz tone through 2 kHz cores
	{
		Rig r;
		r.e.p.cutoff = cutoffKnob(2000.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.f;
		r.e.p.ownType[3] = true;
		r.e.p.coreType[3] = 2.f;
		std::vector<float> y0, y3;
		for (int n = 0; n < 48000; n++) {
			float x = 0.4f * (float) std::sin(2.0 * M_PI * 100.0 * n / SR);
			r.step(x, x);
			y0.push_back(r.o.cores[0]);
			y3.push_back(r.o.cores[3]);
		}
		double d = db(goertzel(y3, 24000, 48000, 100.0) / goertzel(y0, 24000, 48000, 100.0));
		check(d < -40.0, "per core: own TYPE (HP core vs LP cores at 100 Hz)", "%.1f dB (limit -40)", d);
	}
	// LEVEL 0 silences a core; PAN -1 puts a core on the left only
	{
		Rig r;
		r.e.p.coreLevel[2] = 0.f;
		r.e.p.coreLevel[1] = r.e.p.coreLevel[3] = 0.f;
		r.e.p.ownPan[0] = true;
		r.e.p.corePan[0] = -1.f;
		double e2 = 0.0, el = 0.0, er = 0.0;
		for (int n = 0; n < 48000; n++) {
			float x = 0.4f * (float) std::sin(2.0 * M_PI * 300.0 * n / SR);
			r.step(x, x);
			if (n > 24000) {
				e2 += r.o.cores[2] * r.o.cores[2];
				el += r.o.l * r.o.l;
				er += r.o.r * r.o.r;
			}
		}
		check(e2 == 0.0, "per core: LEVEL 0 silences the core", "%.3g", e2);
		check(10.0 * std::log10((er + 1e-20) / el) < -60.0, "per core: PAN -100% = left only (R vs L dB)", "%.1f",
			10.0 * std::log10((er + 1e-20) / el));
	}
	// Own PAN replaces WIDTH's place: violet (WIDTH puts it right) goes hard left; a stereo core moves as a whole
	{
		Rig r;
		r.e.p.width = 1.f;
		r.e.p.cutoff = 1.f;
		r.e.p.res = 0.f;
		r.e.p.coreLevel[0] = r.e.p.coreLevel[1] = r.e.p.coreLevel[2] = 0.f;
		r.e.p.ownPan[3] = true;
		r.e.p.corePan[3] = -1.f;
		std::vector<float> yl, yr;
		for (int n = 0; n < 48000; n++) {
			float a = 0.4f * (float) std::sin(2.0 * M_PI * 300.0 * n / SR);
			float b = 0.4f * (float) std::sin(2.0 * M_PI * 500.0 * n / SR);
			r.step(a, b);
			yl.push_back(r.o.l);
			yr.push_back(r.o.r);
		}
		double rLevel = db(goertzel(yr, 24000, 48000, 300.0) + goertzel(yr, 24000, 48000, 500.0));
		double lFromR = db(goertzel(yl, 24000, 48000, 500.0) / 0.4);
		check(rLevel < -60.0, "per core: own PAN -100% overrides WIDTH (R level dB)", "%.1f (limit -60)", rLevel);
		check(lFromR > -9.0, "per core: hard-left core keeps its right channel (dB)", "%.1f (limit -9)", lFromR);
	}
	// Own CUTOFF: the core sits at its own frequency; global CUTOFF and SPREAD no longer move it; taking it is seamless
	{
		Rig r;
		r.e.p.cutoff = cutoffKnob(440.f);
		r.e.p.spread = 0.6f;
		for (int n = 0; n < 4800; n++)
			r.step();
		float before = r.e.oct[1];
		r.e.p.ownCutoff[1] = true;
		r.e.p.coreCutoff[1] = r.e.place[1];
		for (int n = 0; n < 4800; n++)
			r.step();
		check(std::fabs(r.e.oct[1] - before) < 1e-4f, "per core: taking own CUTOFF does not jump (oct)", "%.6f",
			r.e.oct[1] - before);
		r.e.p.coreCutoff[1] = cutoffKnob(1000.f);
		r.e.p.cutoff = cutoffKnob(100.f);
		for (int n = 0; n < 48000; n++)
			r.step();
		check(std::fabs(std::exp2(r.e.oct[1]) - 1000.0) < 0.5 && std::fabs(r.e.oct[0] - r.e.oct[1]) > 1.f,
			"per core: own CUTOFF 1 kHz, global moved to 100 Hz", "%.2f Hz", std::exp2(r.e.oct[1]));
	}
	// Own BAND: core 1 narrowed, the others not
	{
		Rig r;
		r.e.p.cutoff = cutoffKnob(1000.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.f;
		r.e.p.type = 1.f;
		r.e.p.ownBand[0] = true;
		r.e.p.coreBand[0] = 1.f;
		std::vector<float> y0, y1;
		for (int n = 0; n < 48000; n++) {
			float x = 0.2f * (float) std::sin(2.0 * M_PI * 2000.0 * n / SR);
			r.step(x, x);
			y0.push_back(r.o.cores[0]);
			y1.push_back(r.o.cores[1]);
		}
		double d = db(goertzel(y0, 24000, 48000, 2000.0) / goertzel(y1, 24000, 48000, 2000.0));
		check(d < -40.0, "per core: own BAND narrows only that core (1 oct away)", "%.1f dB (limit -40)", d);
	}
}

/** Click at a ping's onset: the largest sample step in the first ms vs the largest step of the ring itself later on
(a ring that starts from zero has an onset no steeper than its own slope). TYPE `type`, RES 90%, STRIKE 0 (hardest). */
static double pingClick(float type, bool repingRinging) {
	Rig r;
	r.e.p.cutoff = cutoffKnob(440.f);
	r.e.p.spread = 0.f;
	r.e.p.res = 0.9f;
	r.e.p.type = type;
	r.e.p.strike = 0.f;
	r.step();
	std::vector<float> y;
	int at = 0;
	if (repingRinging) {
		r.e.pingAll(1.f);
		for (int n = 0; n < 4801; n++) {
			r.step();
			y.push_back(r.o.cores[0]);
		}
		at = (int) y.size();
	}
	r.e.pingAll(1.f);
	for (int n = 0; n < 9600; n++) {
		r.step();
		y.push_back(r.o.cores[0]);
	}
	double onset = 0.0, ring = 0.0;
	for (size_t n = at + 1; n < y.size(); n++) {
		double d = std::fabs(y[n] - y[n - 1]);
		if ((int) n < at + 48)
			onset = std::max(onset, d);
		else if ((int) n > at + 480)
			ring = std::max(ring, d);
	}
	return onset / ring;
}

static void testClicks() {
	const char* names[3] = {"LP", "BP", "HP"};
	for (int t = 0; t < 3; t++) {
		char name[80];
		double c = pingClick((float) t, false);
		std::snprintf(name, sizeof name, "click: ping onset on %s, STRIKE 0 (x ring slope)", names[t]);
		check(c < 2.0, name, "%.2f (limit 2)", c);
		c = pingClick((float) t, true);
		std::snprintf(name, sizeof name, "click: re-ping a ringing %s core (x ring slope)", names[t]);
		check(c < 2.0, name, "%.2f (limit 2)", c);
	}
	// The old way (all into the band state) on BP, for reference: it must have been a click
	Core core;
	float w[4] = {0.f, 1.f, 0.f, 0.f};
	CoreCoefs cc = coreCoefs(440.f, 0.9f, 1.f, SR);
	float prev = core.process(0.f, cc.g, cc.k, w, cc.bpGain, 0.f, 0.f);
	core.excite(0.9f / pingResponse(w, cc.k, cc.bpGain), 0.f);
	float first = core.process(0.f, cc.g, cc.k, w, cc.bpGain, 0.f, 0.f);
	double ringSlope = 0.9 * 2.0 * M_PI * 440.0 / SR;
	check(std::fabs(first - prev) / ringSlope > 2.0, "click: old excitation on BP was a step (x ring slope)", "%.1f",
		std::fabs(first - prev) / ringSlope);
}

/** Peak gain (dB) of core 1 at its own frequency for a sine of `amp`, LP RES 80% at 1 kHz, ANALOG `an` */
static double resonantGain(float an, float amp) {
	Rig r;
	r.e.p.cutoff = cutoffKnob(1000.f);
	r.e.p.spread = 0.f;
	r.e.p.res = 0.8f;
	r.e.p.analog = an;
	r.e.seedCharacter(1);
	std::vector<float> y;
	for (int n = 0; n < 48000; n++) {
		float x = amp * (float) std::sin(2.0 * M_PI * 1000.0 * n / SR);
		r.step(x, x);
		y.push_back(r.o.cores[0]);
	}
	return db(goertzel(y, 24000, 48000, 1000.0) / amp);
}

static void testResonanceSaturation() {
	// Quiet signals ring as before; loud ones squash the resonant peak (ANALOG), like an OTA / ladder loop
	double q0 = resonantGain(0.f, 0.002f), q1 = resonantGain(1.f, 0.002f);
	double l0 = resonantGain(0.f, 0.2f), l1 = resonantGain(1.f, 0.2f);
	std::printf("     resonant gain: quiet %.1f / %.1f dB, loud %.1f / %.1f dB (ANALOG 0 / 1)\n", q0, q1, l0, l1);
	// (ANALOG 1 also detunes the cores slightly (mismatch), which moves the peak: compare quiet vs loud within a setting)
	double qq1 = resonantGain(1.f, 0.0005f);
	check(std::fabs(q1 - qq1) < 0.5, "resonance: quiet signals stay linear at ANALOG 1 (dB)", "%.2f (limit +-0.5)", q1 - qq1);
	double extra = (l1 - q1) - (l0 - q0);
	check(extra < -6.0, "resonance: loud input squashes the peak (extra vs ANALOG 0, dB)", "%.2f (limit < -6)", extra);

	// A hard ping at ANALOG 1 settles faster at first (bloom), stays in tune
	auto pingDecay = [](float an, double* cents) {
		Rig r;
		r.e.p.cutoff = cutoffKnob(440.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.88f;
		r.e.p.analog = an;
		r.e.seedCharacter(1);
		r.step();
		r.e.pingAll(1.f);
		std::vector<float> y;
		for (int n = 0; n < 3 * 48000; n++) {
			r.step();
			y.push_back(r.o.cores[0]);
		}
		double peak = 0.0;
		for (int n = 0; n < 2400; n++)
			peak = std::max(peak, (double) std::fabs(y[n]));
		int t20 = -1;
		for (size_t n = 2400; n < y.size(); n += 48) {
			double m = 0.0;
			for (size_t k = n; k < n + 480 && k < y.size(); k++)
				m = std::max(m, (double) std::fabs(y[k]));
			if (m < peak * 0.1) {
				t20 = (int) n;
				break;
			}
		}
		double f = zeroCrossFreq(y, 48000, 96000);
		*cents = 1200.0 * std::log2(f / 440.0);
		return t20 / SR;
	};
	double c0, c1;
	double t0 = pingDecay(0.f, &c0), t1 = pingDecay(1.f, &c1);
	std::printf("     hard ping, time to -20 dB: %.3f s (ANALOG 0) vs %.3f s (ANALOG 1); pitch %.2f / %.2f cents\n", t0, t1, c0,
		c1);
	check(t1 < t0, "resonance: a hard ping blooms (settles faster at ANALOG 1)", "%.3f vs %.3f s", t1, t0);
	check(std::fabs(c1) < 10.0, "resonance: ping still in tune at ANALOG 1 (cents)", "%.2f (limit 10)", c1);

	// Self-oscillation at ANALOG 1 stays bounded and alive
	Rig r;
	r.e.p.cutoff = cutoffKnob(440.f);
	r.e.p.spread = 0.f;
	r.e.p.res = 1.f;
	r.e.p.analog = 1.f;
	r.step();
	r.e.pingAll(0.2f);
	double tail = 0.0;
	for (int n = 0; n < 3 * 48000; n++) {
		r.step();
		if (n > 2 * 48000)
			tail = std::max(tail, (double) std::fabs(r.o.cores[0]));
	}
	check(tail > 0.05 && tail < 1.2, "resonance: self-oscillation at ANALOG 1 (peak)", "%.3f (0.05 .. 1.2)", tail);
}

static void testDrive() {
	// DRIVE has audible character: rich odd and even harmonics at full, still clean at 0
	auto harm = [](float drive, double hz) {
		WarmDrive w;
		std::vector<float> y;
		for (int n = 0; n < 72000; n++)
			y.push_back(w.process(0.5f * (float) std::sin(2.0 * M_PI * 220.0 * n / SR), drive));
		return goertzel(y, 24000, 72000, hz);
	};
	double h1 = harm(1.f, 220.0);
	double h2 = db(harm(1.f, 440.0) / h1), h3 = db(harm(1.f, 660.0) / h1);
	check(h3 > -15.0 && h2 > -25.0, "drive: full DRIVE harmonics H2 / H3 (dB)", "%.1f / %.1f (H3 > -15, H2 > -25)", h2, h3);
	double mid = db(harm(0.42f, 660.0) / harm(0.42f, 220.0));
	check(mid > -25.0, "drive: 42% DRIVE is audible (H3 dB)", "%.1f (limit -25)", mid);
	double lvl = db(h1 / 0.5);
	check(std::fabs(lvl) < 6.0, "drive: full DRIVE level vs input (dB)", "%.1f (limit +-6)", lvl);
}

static void testNewShapes() {
	float rnd[4] = {};
	float o[4];
	double worst = 0.0;
	// Bell: exact ratios at SPREAD 50%
	const float BELL[4] = {1.f, 2.f, 2.4f, 4.f};
	spreadOffsets(SHAPE_BELL, 0.5f, 0, 0, rnd, o);
	for (int i = 0; i < 4; i++)
		worst = std::max(worst, std::fabs(1200.0 * (o[i] - std::log2(BELL[i]))));
	check(worst < 0.01, "shape: Bell exact ratios at SPREAD 50% (cents)", "%.4f", worst);
	// Vowel: SPREAD 0 = A, 25% = E (offsets from 1 kHz)
	spreadOffsets(SHAPE_VOWEL, 0.f, 0, 0, rnd, o);
	double a1 = 1000.0 * std::exp2(o[0]), a2 = 1000.0 * std::exp2(o[1]);
	spreadOffsets(SHAPE_VOWEL, 0.25f, 0, 0, rnd, o);
	double e2 = 1000.0 * std::exp2(o[1]);
	check(std::fabs(a1 - 600.0) < 0.1 && std::fabs(a2 - 1050.0) < 0.1 && std::fabs(e2 - 1600.0) < 0.1,
		"shape: Vowel A (600 / 1050 Hz), E (F2 1600 Hz)", "%.1f / %.1f", a1, e2);
	// Just: major chord third = 5/4 (-13.7 cents from 12-TET), dominant 7 seventh = 7/4, scale maj7 = 15/8
	spreadOffsets(SHAPE_CHORD, 0.f, 0, 5, rnd, o, true);
	double third = 1200.0 * o[1] - 400.0;
	check(std::fabs(std::exp2(o[1]) - 1.25) < 1e-5 && std::fabs(std::exp2(o[2]) - 1.5) < 1e-5,
		"tuning: just major chord 5/4, 3/2", "%.2f cents vs 12-TET third", third);
	spreadOffsets(SHAPE_CHORD, 0.f, 0, 2, rnd, o, true);
	check(std::fabs(std::exp2(o[3]) - 1.75) < 1e-5, "tuning: just dominant 7 uses 7/4", "%.5f", std::exp2(o[3]));
	spreadOffsets(SHAPE_SCALE, 2.f / 7.f, 0, 0, rnd, o, true);
	check(std::fabs(std::exp2(o[3]) - 1.875) < 1e-5, "tuning: just major scale, 7th degree = 15/8", "%.5f", std::exp2(o[3]));
	// Equal tuning unchanged: the major third is 400 cents
	spreadOffsets(SHAPE_CHORD, 0.f, 0, 5, rnd, o, false);
	check(std::fabs(1200.0 * o[1] - 400.0) < 1e-3, "tuning: equal major third = 400 cents", "%.4f", 1200.0 * o[1]);

	// Random re-roll (the rainbow LED) glides to the new places (~40 ms) instead of jumping
	{
		Rig r;
		r.e.p.shape = SHAPE_RANDOM;
		r.e.p.spread = 1.f;
		r.e.seedRandomShape(1);
		for (int n = 0; n < 4800; n++)
			r.step();
		float b4[4], e4[4];
		for (int i = 0; i < 4; i++)
			b4[i] = r.e.oct[i];
		r.e.seedRandomShape(99, true);
		for (int n = 0; n < 2 * PrismaEngine::BLOCK; n++)
			r.step();
		for (int i = 0; i < 4; i++)
			e4[i] = r.e.oct[i];
		for (int n = 0; n < 24000; n++)
			r.step();
		// The core that moves furthest
		int k = 0;
		for (int i = 1; i < 4; i++)
			if (std::fabs(r.e.oct[i] - b4[i]) > std::fabs(r.e.oct[k] - b4[k]))
				k = i;
		float before = b4[k], early = e4[k], after = r.e.oct[k];
		double share = std::fabs(early - before) / std::max(1e-6f, std::fabs(after - before));
		check(std::fabs(after - before) > 0.05f && share < 0.3, "shape: Random re-roll glides (share after 2 blocks)",
			"%.3f (limit 0.3)", share);
	}

	// A pinged Bell rings its tierce at 2.4 x CUTOFF (core 3), in tune
	{
		Rig r;
		r.e.p.cutoff = cutoffKnob(220.f);
		r.e.p.shape = SHAPE_BELL;
		r.e.p.spread = 0.5f;
		r.e.p.res = 0.9f;
		r.step();
		r.e.pingAll(1.f);
		std::vector<float> y;
		for (int n = 0; n < 24000; n++) {
			r.step();
			y.push_back(r.o.cores[2]);
		}
		double f = zeroCrossFreq(y, 480, 24000);
		double cents = 1200.0 * std::log2(f / (220.0 * 2.4));
		check(std::fabs(cents) < 3.0, "shape: pinged Bell tierce at 2.4 x CUTOFF (cents)", "%.3f (limit 3)", cents);
	}
}

static void testLpg() {
	// A ping through the gate: time for the ring's envelope to fall 20 dB and 40 dB (ms), per setting
	auto ringTimes = [](float depth, float decay, double* t20, double* t40, double* peakOut) {
		Rig r;
		r.e.p.cutoff = cutoffKnob(440.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.85f;
		r.e.p.lpg = depth;
		r.e.p.lpgDecay = decay;
		r.e.p.lpgSource = 0; // ping only
		r.step();
		r.e.pingAll(1.f);
		std::vector<double> env;
		double peak = 0.0;
		for (int n = 0; n < 2 * 48000; n++) {
			r.step();
			double a = std::fabs(r.o.l);
			peak = std::max(peak, a);
			if (n % 48 == 0)
				env.push_back(0.0);
			env.back() = std::max(env.back(), a);
		}
		*peakOut = peak;
		*t20 = *t40 = -1.0;
		for (size_t k = 0; k < env.size(); k++) {
			if (*t20 < 0.0 && k > 5 && env[k] < peak * 0.1)
				*t20 = k;
			if (*t40 < 0.0 && k > 5 && env[k] < peak * 0.01)
				*t40 = k;
		}
	};
	double a20, a40, ap, b20, b40, bp, c20, c40, cp;
	ringTimes(0.f, 0.5f, &a20, &a40, &ap);
	ringTimes(1.f, 0.2f, &b20, &b40, &bp);
	ringTimes(1.f, 0.8f, &c20, &c40, &cp);
	std::printf("     ring to -20 / -40 dB (ms): no gate %.0f / %.0f, gate fast %.0f / %.0f, gate slow %.0f / %.0f\n", a20, a40,
		b20, b40, c20, c40);
	check(b20 > 0 && b20 < a20 * 0.6, "lpg: the gate shortens a ring (fast gate vs none, -20 dB time)", "%.0f vs %.0f ms", b20, a20);
	check(c20 > b20 * 1.5, "lpg: DECAY lengthens the release (slow vs fast, -20 dB time)", "%.0f vs %.0f ms", c20, b20);
	double slowdown = (c40 - c20) / std::max(1.0, c20);
	check(slowdown > 0.5, "lpg: the release slows as it closes (-20..-40 dB takes longer than 0..-20)", "%.2f x", slowdown);
	check(std::fabs(db(bp / ap)) < 6.0, "lpg: the ping keeps its loudness (peak vs no gate, dB)", "%.2f (limit +-6)", db(bp / ap));

	// A closed gate with the input source: silent without input, opens with input, closes after it
	{
		Rig r;
		r.e.p.cutoff = cutoffKnob(1000.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.2f;
		r.e.p.lpg = 1.f;
		r.e.p.lpgDecay = 0.2f;
		r.e.p.lpgSource = 1;
		double before = 0.0, during = 0.0, after = 0.0;
		for (int n = 0; n < 4 * 48000; n++) {
			float x = (n > 48000 && n < 2 * 48000) ? 0.3f * (float) std::sin(2.0 * M_PI * 1000.0 * n / SR) : 0.f;
			r.step(x, x);
			double a = std::fabs(r.o.l);
			if (n < 48000)
				before = std::max(before, a);
			else if (n > 1.5 * 48000 && n < 2 * 48000)
				during = std::max(during, a);
			else if (n > 3.5 * 48000)
				after = std::max(after, a);
		}
		std::printf("     gate on input: before %.4f, during %.3f, after %.5f\n", before, during, after);
		check(before == 0.0 && during > 0.1 && after < 0.003, "lpg: input opens the gate and it closes after",
			"%.3f open, %.5f closed", during, after);
	}

	// The level compensation must not boost a closed gate
	{
		Rig r;
		r.e.p.res = 0.4f;
		r.e.p.lpg = 1.f;
		r.e.p.lpgSource = 0;
		r.e.p.levelComp = LEVEL_FULL;
		ColoredNoise nz;
		for (int n = 0; n < 4 * 48000; n++) {
			float a = nz.process(1.f / 3.f);
			r.step(a, a);
		}
		check(std::fabs(r.e.levelCompDb()) < 1.f, "lpg: level compensation stays at 0 dB with the gate on", "%.2f dB",
			r.e.levelCompDb());
	}

	// Click-free: the gate's onset on a ping is no steeper than the ring's own slope x 2
	{
		Rig r;
		r.e.p.cutoff = cutoffKnob(440.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.85f;
		r.e.p.lpg = 1.f;
		r.e.p.lpgSource = 0;
		r.e.p.strike = 0.f;
		r.step();
		r.e.pingAll(1.f);
		float prev = 0.f, maxStep = 0.f;
		for (int n = 0; n < 4800; n++) {
			r.step();
			maxStep = std::max(maxStep, std::fabs(r.o.l - prev));
			prev = r.o.l;
		}
		double ringSlope = 0.9 * 2.0 * M_PI * 440.0 / SR;
		check(maxStep < 2.0 * ringSlope, "lpg: onset is click-free (max step vs ring slope)", "%.4f vs %.4f", maxStep, ringSlope);
	}
}

static void testQuantizeSpread() {
	// Quantize to the scale: every spread core lands on a note of the scale (C major), not only CUTOFF
	const int MAJOR[7] = {0, 2, 4, 5, 7, 9, 11};
	for (int shape : {(int) SHAPE_EVEN, (int) SHAPE_RANDOM, (int) SHAPE_HARMONIC}) {
		double worst = 0.0;
		for (float spread : {0.2f, 0.45f, 0.7f, 1.f}) {
			Rig r;
			r.e.p.quantizeCutoff = true;
			r.e.p.scale = 0;
			r.e.p.shape = shape;
			r.e.p.spread = spread;
			r.e.p.cutoff = 0.6f;
			r.e.seedRandomShape(5);
			for (int n = 0; n < 4800; n++)
				r.step();
			for (int i = 0; i < 4; i++) {
				double semis = (r.e.oct[i] - std::log2(261.6256)) * 12.0;
				double within = semis - 12.0 * std::floor(semis / 12.0);
				double best = 99.0;
				for (int k = 0; k < 7; k++)
					best = std::min(best, std::fabs(within - MAJOR[k]));
				best = std::min(best, std::fabs(within - 12.0));
				worst = std::max(worst, best * 100.0);
			}
		}
		char name[80];
		std::snprintf(name, sizeof name, "quantize: spread cores on scale notes, shape %d (cents off)", shape);
		check(worst < 0.5, name, "%.3f (limit 0.5)", worst);
	}
	// Off: the spread cores stay free
	{
		Rig r;
		r.e.p.quantizeCutoff = false;
		r.e.p.spread = 0.45f;
		r.e.p.cutoff = 0.6f;
		for (int n = 0; n < 4800; n++)
			r.step();
		double semis = (r.e.oct[1] - std::log2(261.6256)) * 12.0;
		check(std::fabs(semis - std::round(semis)) > 0.05, "quantize: off leaves the spread free", "%.3f semitones", semis);
	}
	// A Chord keeps its intervals with quantize on (C major scale, chord from a D root)
	{
		Rig r;
		r.e.p.quantizeCutoff = true;
		r.e.p.shape = SHAPE_CHORD;
		r.e.p.chord = 5; // major 0 4 7 12
		r.e.p.spread = 0.f;
		r.e.p.cutoff = cutoffKnob(293.66f); // D4
		for (int n = 0; n < 4800; n++)
			r.step();
		double third = 12.0 * (r.e.oct[1] - r.e.oct[0]);
		check(std::fabs(third - 4.0) < 0.01, "quantize: Chord keeps its major third in another key", "%.3f semitones", third);
	}
}

static void testSeriesLevel() {
	// Four LP cores on ONE pitch (SPREAD 0), RES 72%: in series their resonances multiply (Q^4). A ping, the noise and
	// the per-core LEVEL / BAND make-up must not pile up: series is about as loud as parallel
	auto run = [](float stack, float levels, float band, bool noise, double* peakOut) {
		Rig r;
		r.e.p.cutoff = 0.589f;
		r.e.p.spread = 0.f;
		r.e.p.res = 0.716f;
		r.e.p.stack = stack;
		r.e.p.band = band;
		r.e.p.strike = 0.32f;
		r.e.p.noise = noise ? 0.328f : 0.f;
		r.e.p.color = 0.08f;
		for (int i = 0; i < 4; i++)
			r.e.p.coreLevel[i] = levels;
		r.step();
		r.e.pingAll(1.f);
		double peak = 0.0, e = 0.0;
		for (int n = 0; n < 2 * 48000; n++) {
			r.step();
			peak = std::max(peak, (double) std::fabs(r.o.l));
			if (n > 24000)
				e += r.o.l * r.o.l;
		}
		*peakOut = peak;
		return std::sqrt(e / 24000.0);
	};
	double pk;
	double par = run(0.f, 1.f, 0.f, false, &pk), parPeak = pk;
	double ser = run(1.f, 1.f, 0.f, false, &pk), serPeak = pk;
	std::printf("     ping, same pitch: parallel peak %.2f, series peak %.2f (engine units, x5 V)\n", parPeak, serPeak);
	check(db(serPeak / parPeak) < 4.0, "series: a ping rings about as loud as in parallel (dB)", "%.2f (limit 4)", db(serPeak / parPeak));
	// LEVEL 160% on all four cores does not multiply through the chain (1.6^4 = +12 dB)
	run(1.f, 1.6f, 0.f, false, &pk);
	check(db(pk / serPeak) < 5.0, "series: LEVEL 160% x 4 does not compound (dB vs 100%)", "%.2f (limit 5)", db(pk / serPeak));
	// BAND 100% make-up does not multiply through the chain (about +47 dB before)
	run(1.f, 1.f, 1.f, false, &pk);
	check(db(pk / serPeak) < 8.0, "series: BAND 100% make-up does not compound (dB vs BAND 0)", "%.2f (limit 8)", db(pk / serPeak));
	// Noise in series is about as loud as in parallel
	double nPar = run(0.f, 1.f, 0.f, true, &pk), nSer = run(1.f, 1.f, 0.f, true, &pk);
	check(db(nSer / nPar) < 6.0, "series: noise rings about as loud as in parallel (rms dB)", "%.2f (limit 6)", db(nSer / nPar));
	(void) par;
	(void) ser;
}

static void testSlope24() {
	// A 4-pole low-pass: two octaves above the cutoff the 24 dB core is ~24 dB further down than the 12 dB one
	auto gainAt = [](bool s24, float res, float an, double hz) {
		Rig r;
		r.e.p.cutoff = cutoffKnob(1000.f);
		r.e.p.spread = 0.f;
		r.e.p.res = res;
		r.e.p.analog = an;
		r.e.p.levelComp = LEVEL_OFF;
		r.e.seedCharacter(1);
		for (int i = 0; i < 4; i++)
			r.e.p.slope24[i] = s24;
		std::vector<float> y;
		for (int n = 0; n < 48000; n++) {
			float x = 0.05f * (float) std::sin(2.0 * M_PI * hz * n / SR);
			r.step(x, x);
			y.push_back(r.o.cores[0]);
		}
		return db(goertzel(y, 24000, 48000, hz) / 0.05);
	};
	double slope12 = gainAt(false, 0.f, 0.f, 4000.0), slope24 = gainAt(true, 0.f, 0.f, 4000.0);
	std::printf("     2 octaves above the cutoff: 12 dB core %.1f dB, 24 dB core %.1f dB\n", slope12, slope24);
	check(slope24 < slope12 - 15.0, "slope: 24 dB core is steeper (2 oct above cutoff, dB further down)", "%.1f (limit 15)", slope12 - slope24);
	double pass12 = gainAt(false, 0.f, 0.f, 100.0), pass24 = gainAt(true, 0.f, 0.f, 100.0);
	check(std::fabs(pass24 - pass12) < 0.5, "slope: passband unchanged at ANALOG 0 (100 Hz, dB)", "%.2f (limit 0.5)", pass24 - pass12);
	// Ladder trait: at high RES and ANALOG the bass thins
	double bass0 = gainAt(true, 0.88f, 0.f, 100.0), bass1 = gainAt(true, 0.88f, 1.f, 100.0);
	check(bass1 < bass0 - 4.0, "slope: ANALOG + RES thins the bass of a 24 dB core (100 Hz, dB)", "%.2f vs %.2f", bass1, bass0);
	// A 12 dB core is not affected by ANALOG's bass loss
	double b12 = gainAt(false, 0.88f, 1.f, 100.0);
	check(std::fabs(b12) < 1.0, "slope: a 12 dB core keeps its bass (100 Hz, dB)", "%.2f", b12);
	// A pinged 24 dB core rings in tune and as loud as a 12 dB one
	auto ping = [](bool s24, double* cents) {
		Rig r;
		r.e.p.cutoff = cutoffKnob(440.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.85f;
		for (int i = 0; i < 4; i++)
			r.e.p.slope24[i] = s24;
		r.step();
		r.e.pingAll(1.f);
		std::vector<float> y;
		double peak = 0.0;
		for (int n = 0; n < 24000; n++) {
			r.step();
			y.push_back(r.o.cores[0]);
			peak = std::max(peak, (double) std::fabs(r.o.cores[0]));
		}
		*cents = 1200.0 * std::log2(zeroCrossFreq(y, 960, 24000) / 440.0);
		return peak;
	};
	double c12, c24;
	double p12 = ping(false, &c12), p24 = ping(true, &c24);
	check(std::fabs(c24 - c12) < 15.0, "slope: a pinged 24 dB core stays in tune (cents vs 12 dB)", "%.2f vs %.2f", c24, c12);
	check(std::fabs(db(p24 / p12)) < 4.0, "slope: a pinged 24 dB core rings about as loud (dB)", "%.2f (limit +-4)", db(p24 / p12));
	// BP is left alone: the second stage is only for the low-pass share
	double bp12 = 0.0, bp24 = 0.0;
	for (int k = 0; k < 2; k++) {
		Rig r;
		r.e.p.cutoff = cutoffKnob(1000.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.f;
		r.e.p.type = 1.f;
		r.e.p.levelComp = LEVEL_OFF;
		for (int i = 0; i < 4; i++)
			r.e.p.slope24[i] = k == 1;
		std::vector<float> y;
		for (int n = 0; n < 48000; n++) {
			float x = 0.05f * (float) std::sin(2.0 * M_PI * 1000.0 * n / SR);
			r.step(x, x);
			y.push_back(r.o.cores[0]);
		}
		(k ? bp24 : bp12) = goertzel(y, 24000, 48000, 1000.0);
	}
	check(std::fabs(db(bp24 / bp12)) < 0.1, "slope: TYPE BP is unaffected (dB)", "%.3f", db(bp24 / bp12));
}

static void testPingDriveAnalog() {
	// A ping's ring: level at the core's own frequency, for a bright core (5 kHz) where the strike's high end matters
	auto ring = [](float drive, float analog, int pos, double hz, uint32_t seed, double* peakOut) {
		Rig r;
		r.e.p.cutoff = cutoffKnob((float) hz);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.85f;
		r.e.p.drive = drive;
		r.e.p.drivePos = pos;
		r.e.p.analog = analog;
		r.e.p.strike = 0.4f; // felt
		r.e.seedCharacter(seed);
		for (int n = 0; n < 2000; n++)
			r.step();
		r.e.pingAll(1.f);
		std::vector<float> y;
		double peak = 0.0;
		for (int n = 0; n < 12000; n++) {
			r.step();
			y.push_back(r.o.cores[0]);
			peak = std::max(peak, (double) std::fabs(r.o.cores[0]));
		}
		if (peakOut)
			*peakOut = peak;
		return goertzel(y, 1200, 12000, hz);
	};
	// DRIVE saturates the strike: more high-frequency energy in the hit (brighter ring on a high core), same loudness
	// on a low core (energy matched)
	double hi0 = ring(0.f, 0.f, DRIVE_PRE, 5000.0, 1, nullptr), hi1 = ring(1.f, 0.f, DRIVE_PRE, 5000.0, 1, nullptr);
	double lo0 = ring(0.f, 0.f, DRIVE_PRE, 200.0, 1, nullptr), lo1 = ring(1.f, 0.f, DRIVE_PRE, 200.0, 1, nullptr);
	std::printf("     ping ring, DRIVE 0 -> 100%%: 5 kHz core %.1f dB, 200 Hz core %.1f dB\n", db(hi1 / hi0), db(lo1 / lo0));
	check(db(hi1 / hi0) > 3.0, "ping: DRIVE brightens the strike (5 kHz ring, dB)", "%.2f (limit > 3)", db(hi1 / hi0));
	check(std::fabs(db(lo1 / lo0)) < 3.0, "ping: DRIVE keeps a low ring about as loud (dB)", "%.2f (limit +-3)", db(lo1 / lo0));
	// ANALOG: every ping a little different (a few % of strength), none at ANALOG 0
	double mn = 1e9, mx = 0.0, mn0 = 1e9, mx0 = 0.0;
	{
		Rig a, z;
		a.e.p.cutoff = z.e.p.cutoff = cutoffKnob(440.f);
		a.e.p.spread = z.e.p.spread = 0.f;
		a.e.p.res = z.e.p.res = 0.85f;
		a.e.p.analog = 1.f;
		z.e.p.analog = 0.f;
		a.e.seedCharacter(3);
		z.e.seedCharacter(3);
		a.step();
		z.step();
		for (int k = 0; k < 12; k++) {
			a.e.pingAll(1.f);
			z.e.pingAll(1.f);
			double pa = 0.0, pz = 0.0;
			for (int n = 0; n < 24000; n++) {
				a.step();
				z.step();
				if (n < 4800) {
					pa = std::max(pa, (double) std::fabs(a.o.cores[0]));
					pz = std::max(pz, (double) std::fabs(z.o.cores[0]));
				}
			}
			mn = std::min(mn, pa);
			mx = std::max(mx, pa);
			mn0 = std::min(mn0, pz);
			mx0 = std::max(mx0, pz);
		}
	}
	std::printf("     12 pings, spread of the ring peak: ANALOG 1 %.2f dB, ANALOG 0 %.2f dB\n", db(mx / mn), db(mx0 / mn0));
	check(db(mx / mn) > 0.3 && db(mx0 / mn0) < 0.05, "ping: ANALOG makes every ping a little different", "%.2f dB (ANALOG 0: %.3f dB)", db(mx / mn), db(mx0 / mn0));
}

static void testDigitalAnalogLife() {
	// Shift-register noise: every register length has the full period 2^n - 1
	{
		bool ok = true;
		int worstN = 0;
		for (int i = 0; i < ShiftNoise::LENGTHS; i++) {
			int n = ShiftNoise::bitsFor(i);
			ShiftNoise sn;
			sn.idx = i;
			sn.reg = 1;
			uint32_t start = 1;
			int period = 0;
			for (int k = 1; k <= (1 << n) + 2; k++) {
				float c = (i + 0.5f) / ShiftNoise::LENGTHS;
				sn.process(c);
				if (sn.reg == start) {
					period = k;
					break;
				}
			}
			if (period != (1 << n) - 1) {
				ok = false;
				worstN = n;
			}
		}
		check(ok, "digital noise: every register has the maximal period 2^n - 1", "%.0f (bad length, 0 = none)", (double) worstN);
	}
	// A 3-bit register repeats every 7 samples: a pitched buzz at sample rate / 7; 17 bits is flat; RMS 0.3
	{
		ShiftNoise lo, hi;
		std::vector<float> yl, yh;
		for (int n = 0; n < 48000; n++) {
			yl.push_back(lo.process(0.f));
			yh.push_back(hi.process(0.99f));
		}
		double f7 = SR / 7.0;
		double peak = goertzel(yh, 0, 48000, f7), off = goertzel(yh, 0, 48000, f7 * 0.8);
		double flat = goertzel(yl, 0, 48000, f7) / std::max(1e-9, goertzel(yl, 0, 48000, f7 * 0.8));
		double rms = 0.0;
		for (float v : yl)
			rms += v * v;
		rms = std::sqrt(rms / yl.size());
		std::printf("     shift noise: 3-bit tone %.4f vs off-tone %.5f; 17-bit ratio %.2f, RMS %.3f\n", peak, off, flat, rms);
		check(peak > 20.0 * off && flat < 3.0 && std::fabs(rms - 0.3) < 0.01, "digital noise: short register pitched, long one flat, RMS 0.3",
			"%.1f x peak, flat ratio %.2f", peak / std::max(1e-9, off), flat);
	}
	// The engine: NOISE type digital reaches the cores
	{
		Rig r;
		r.e.p.noise = 0.8f;
		r.e.p.noiseType = 1;
		r.e.p.color = 0.99f;
		r.e.p.cutoff = 1.f;
		r.e.p.res = 0.f;
		double e = 0.0;
		for (int n = 0; n < 24000; n++) {
			r.step();
			if (n > 4800)
				e += r.o.l * r.o.l;
		}
		check(e > 0.01, "digital noise: reaches the output (engine, type digital)", "%.3f", e);
	}
	// PCM gate at the top of DIGITAL: a quiet sine loses many samples entirely; below 75% there is no gate
	{
		auto zeros = [](float digital) {
			Rig r;
			r.e.p.digital = digital;
			r.e.p.cutoff = 1.f;
			r.e.p.res = 0.f;
			r.e.p.levelComp = LEVEL_OFF;
			int z = 0, n2 = 0;
			for (int n = 0; n < 24000; n++) {
				float x = 0.05f * (float) std::sin(2.0 * M_PI * 220.0 * n / SR);
				r.step(x, x);
				if (n > 4800) {
					n2++;
					z += r.o.cores[0] == 0.f;
				}
			}
			return (double) z / n2;
		};
		double g1 = zeros(1.f), g7 = zeros(0.7f);
		check(g1 > 0.2 && g7 < 0.05, "digital: PCM gate zeroes samples at the top of the knob (fraction)", "%.2f at 100%%, %.2f at 70%%", g1, g7);
	}
	// Thermal drift (ANALOG): a core wanders a few cents over tens of seconds; none at ANALOG 0
	{
		double span[2];
		for (int k = 0; k < 2; k++) {
			Rig r;
			r.e.p.analog = k ? 1.f : 0.f;
			r.e.p.spread = 0.f;
			r.e.seedCharacter(11);
			double mn = 1e9, mx = -1e9;
			for (int n = 0; n < 60 * 48000; n++) {
				r.step();
				if (n % 4800 == 0 && n > 48000) {
					double c = 1200.0 * r.e.oct[0];
					mn = std::min(mn, c);
					mx = std::max(mx, c);
				}
			}
			span[k] = mx - mn;
		}
		std::printf("     thermal drift over 60 s: ANALOG 0 %.3f cents, ANALOG 1 %.2f cents\n", span[0], span[1]);
		check(span[0] < 0.001 && span[1] > 1.0 && span[1] < 10.0, "analog: slow thermal drift of a few cents", "%.2f cents (ANALOG 0: %.3f)", span[1], span[0]);
	}
	// Level compensation that hears: output and input match in the weighted (ear) sense, not only in flat RMS
	{
		struct Case {
			const char* name;
			float cutoffHz;
			float band;
			float type;
		};
		const Case cases[3] = {{"LP 200 Hz", 200.f, 0.f, 0.f}, {"HP 5 kHz", 5000.f, 0.f, 2.f}, {"BAND 80%", 1000.f, 0.8f, 1.f}};
		for (int ci = 0; ci < 3; ci++) {
			double diff[2];
			for (int mode = 0; mode < 2; mode++) {
				Rig r;
				r.e.p.levelWeight = mode;
				r.e.p.levelComp = LEVEL_FULL;
				r.e.p.cutoff = cutoffKnob(cases[ci].cutoffHz);
				r.e.p.spread = 0.2f;
				r.e.p.band = cases[ci].band;
				r.e.p.type = cases[ci].type;
				r.e.p.res = 0.5f;
				ColoredNoise nl, nr;
				nr.rng.seed(77);
				LoudnessWeight wi[2], wo[2];
				for (int c = 0; c < 2; c++) {
					wi[c].setSampleRate(SR);
					wo[c].setSampleRate(SR);
				}
				double ei = 0.0, eo = 0.0;
				for (int n = 0; n < 6 * 48000; n++) {
					float a = nl.process(1.f / 3.f), b = nr.process(1.f / 3.f);
					r.step(a, b);
					float xa = wi[0].process(a), xb = wi[1].process(b), ya = wo[0].process(r.o.l), yb = wo[1].process(r.o.r);
					if (n > 4 * 48000) {
						ei += xa * xa + xb * xb;
						eo += ya * ya + yb * yb;
					}
				}
				diff[mode] = 10.0 * std::log10(eo / ei);
			}
			char name[96];
			std::snprintf(name, sizeof name, "level (ear): %s matches the input loudness (weighted dB)", cases[ci].name);
			check(std::fabs(diff[1]) < 1.5, name, "%.2f (flat mode: %.2f)", diff[1], diff[0]);
		}
	}
}

static void testIdle() {
	// Nothing connected, NOISE 0, high ANALOG + RES: silence (the noise floor follows activity)
	auto idleLevel = [](float res, float analog, int seconds = 3) {
		Rig r;
		r.e.p.res = res;
		r.e.p.analog = analog;
		r.e.p.spread = 0.56f;
		double e = 0.0, peak = 0.0;
		for (int n = 0; n < seconds * 48000; n++) {
			r.step();
			if (n > (seconds - 1) * 48000) {
				e += r.o.l * r.o.l;
				peak = std::max(peak, (double) std::fabs(r.o.l));
			}
		}
		return std::make_pair(10.0 * std::log10(e / 48000 + 1e-30), peak);
	};
	double quiet = idleLevel(0.85f, 0.84f).first;
	check(quiet < -120.0, "idle: nothing connected, ANALOG + RES high (dB)", "%.1f (limit -120)", quiet);
	// RES at the top still self-oscillates on its own (the noise floor stays on to start it)
	// (it builds up slowly from the noise floor: the loop gain at the top is only 1.12)
	double osc = idleLevel(1.f, 0.5f, 8).second;
	check(osc > 0.3, "idle: RES 100% self-oscillates with nothing connected (peak at 8 s)", "%.3f (> 0.3)", osc);
	// Under signal the noise floor is there (gate open): output with a quiet input differs from ANALOG 0 by hiss only
	{
		Rig r;
		r.e.p.res = 0.85f;
		r.e.p.analog = 0.84f;
		for (int n = 0; n < 48000; n++) {
			float x = 0.3f * (float) std::sin(2.0 * M_PI * 220.0 * n / SR);
			r.step(x, x);
		}
		check(r.e.levelCompDb() > -20.f, "idle: engine runs normally under signal", "%.2f dB comp", r.e.levelCompDb());
	}
}

static void testDriveVoicings() {
	// Each voicing on a 220 Hz sine at full DRIVE: harmonic profile and level
	auto harm = [](int voice, double hz, float amount) {
		WarmDrive w;
		w.drive.voice = voice;
		w.sampleRate = SR;
		std::vector<float> y;
		for (int n = 0; n < 72000; n++)
			y.push_back(w.process(0.5f * (float) std::sin(2.0 * M_PI * 220.0 * n / SR), amount));
		return goertzel(y, 24000, 72000, hz);
	};
	const char* names[3] = {"warm", "diode", "fold"};
	double h2[3], h5[3], lvl[3];
	for (int v = 0; v < 3; v++) {
		double h1 = harm(v, 220.0, 1.f);
		h2[v] = db(harm(v, 440.0, 1.f) / h1);
		h5[v] = db(harm(v, 1100.0, 1.f) / h1);
		lvl[v] = db(h1 / 0.5);
		std::printf("     drive %s: H2 %.1f dB, H5 %.1f dB, fundamental %.1f dB vs input\n", names[v], h2[v], h5[v], lvl[v]);
	}
	check(h2[1] > h2[0] + 3.0, "drive: diode has more even harmonics than warm (H2 dB)", "%.1f vs %.1f", h2[1], h2[0]);
	check(h5[2] > h5[0] + 6.0, "drive: fold is richer up high than warm (H5 dB)", "%.1f vs %.1f", h5[2], h5[0]);
	check(std::fabs(lvl[1]) < 6.0 && std::fabs(lvl[2]) < 9.0, "drive: diode / fold level vs input (dB)", "%.1f / %.1f",
		lvl[1], lvl[2]);

	// After the cores, the harmonics survive a low cutoff; before the cores, the cores filter them away
	auto h3Out = [](int pos) {
		Rig r;
		r.e.p.cutoff = cutoffKnob(1500.f);
		r.e.p.spread = 0.f;
		r.e.p.res = 0.f;
		r.e.p.drive = 1.f;
		r.e.p.driveVoice = VOICE_DIODE;
		r.e.p.drivePos = pos;
		std::vector<float> y;
		for (int n = 0; n < 72000; n++) {
			float x = 0.5f * (float) std::sin(2.0 * M_PI * 1000.0 * n / SR);
			r.step(x, x);
			y.push_back(r.o.l);
		}
		return db(goertzel(y, 36000, 72000, 3000.0) / goertzel(y, 36000, 72000, 1000.0));
	};
	double pre = h3Out(DRIVE_PRE), post = h3Out(DRIVE_POST);
	check(post > pre + 10.0, "drive: after the cores keeps the harmonics (H3 dB)", "%.1f vs %.1f before", post, pre);

	// DRIVE after the cores adds exactly one DRIVE latency to both the wet and the dry path (MIX stays aligned):
	// a click at MIX 50% peaks exactly DRIVE_LATENCY later than with DRIVE before the cores
	{
		int at[2];
		for (int k = 0; k < 2; k++) {
			Rig r;
			r.e.p.cutoff = 1.f;
			r.e.p.res = 0.f;
			r.e.p.spread = 0.f;
			r.e.p.drive = 0.02f;
			r.e.p.drivePos = k ? DRIVE_POST : DRIVE_PRE;
			r.e.p.mix = 0.5f;
			for (int n = 0; n < 2000; n++)
				r.step();
			double best = 0.0;
			at[k] = -1;
			for (int n = 0; n < 400; n++) {
				float x = (n == 10) ? 0.5f : 0.f;
				r.step(x, x);
				if (std::fabs(r.o.l) > best) {
					best = std::fabs(r.o.l);
					at[k] = n;
				}
			}
		}
		check(at[1] - at[0] == DriveStage::DRIVE_LATENCY, "drive: after the cores, MIX stays aligned (extra latency)",
			"%.0f (expect %.0f)", at[1] - at[0], DriveStage::DRIVE_LATENCY);
	}

	// Switching position fades: no step bigger than the steady signal's own
	{
		Rig r;
		r.e.p.drive = 0.6f;
		float prev = 0.f, steady = 0.f, during = 0.f, after = 0.f;
		for (int n = 0; n < 48000; n++) {
			if (n == 24000)
				r.e.p.drivePos = DRIVE_POST;
			float x = 0.4f * (float) std::sin(2.0 * M_PI * 200.0 * n / SR);
			r.step(x, x);
			float d = std::fabs(r.o.l - prev);
			prev = r.o.l;
			if (n > 12000 && n < 24000)
				steady = std::max(steady, d);
			if (n >= 24000 && n < 26000)
				during = std::max(during, d);
			if (n > 36000)
				after = std::max(after, d);
		}
		check(during < 1.2f * std::max(steady, after), "drive: position switch is click-free",
			"%.4f vs steady %.4f", during, std::max(steady, after));
	}
}

static void testSeriesNoise() {
	// NOISE alone, STACK fully series: the last core still gets noise of its own (not only what survives the chain)
	double rms[2][4] = {};
	for (int k = 0; k < 2; k++) {
		Rig r;
		r.e.p.noise = 0.7f;
		r.e.p.stack = (float) k;
		r.e.p.type = 1.f;
		r.e.p.res = 0.6f;
		r.e.p.spread = 0.8f;
		for (int n = 0; n < 2 * 48000; n++) {
			r.step();
			if (n > 48000)
				for (int i = 0; i < 4; i++)
					rms[k][i] += r.o.cores[i] * r.o.cores[i];
		}
	}
	double d = 10.0 * std::log10(rms[1][3] / rms[0][3]);
	check(d > -6.0, "stack: series keeps NOISE in the last core (vs parallel, dB)", "%.1f (limit -6)", d);
}

int main() {
	testCore();
	testPing();
	testClicks();
	testPlaces();
	testNewShapes();
	testPoly();
	testStereo();
	testStack();
	testSeriesNoise();
	testDrive();
	testDriveVoicings();
	testResonanceSaturation();
	testIdle();
	testDigitalAnalogLife();
	testPingDriveAnalog();
	testSlope24();
	testSeriesLevel();
	testQuantizeSpread();
	testLpg();
	testBand();
	testLevel();
	testPerCore();
	testNoise();
	testSafety();
	std::printf("\n%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
