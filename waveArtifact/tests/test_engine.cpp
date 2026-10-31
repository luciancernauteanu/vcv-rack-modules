// Offline click/NaN tests for MorphEngine (no Rack needed).
// Build and run: cd tests && make run
//
// Method: build banks from a few low harmonics, so the steepest legitimate sample-to-sample step
// has a known analytic bound (sum of k*|a_k| * 2*pi*f/sr). Then run the engine through fast
// position sweeps, position jumps, table changes (including one mid-fade), marker add/remove and a
// bank swap. Any step above the bound is a click.

#include "MorphEngine.hpp"

#include <algorithm>
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
static const int HARMONICS = 4;

/** Bank where table t has harmonic amplitudes amps[t][k]. Returns the analytic slope factor
max_t(sum k*|a_k| / peak_t), i.e. max step per sample = factor * 2*pi*f/sr. */
static wa::WavetableBank* makeBank(const std::vector<std::vector<double>>& amps, double* slopeFactor) {
	wa::WavetableBank* bank = new wa::WavetableBank;
	bank->numTables = (int) amps.size();
	bank->data.assign((size_t) bank->numTables * wa::NUM_LEVELS * wa::FRAME_SIZE, 0.f);
	bank->brightness.assign(bank->numTables, 0.f);
	bank->pitched.assign(bank->numTables, 1);
	*slopeFactor = 0.0;
	for (int t = 0; t < bank->numTables; t++) {
		double peak = 0.0, sumKA = 0.0;
		for (int l = 0; l < wa::NUM_LEVELS; l++) {
			int maxH = (wa::FRAME_SIZE / 2) >> l;
			float* f = bank->frame(t, l);
			for (int i = 0; i < wa::FRAME_SIZE; i++) {
				double x = 2 * PI * i / wa::FRAME_SIZE, v = 0.0;
				for (int k = 1; k <= HARMONICS && k <= maxH; k++)
					v += amps[t][k - 1] * std::sin(k * x);
				f[i] = (float) v;
				if (l == 0)
					peak = std::max(peak, std::fabs(v));
			}
		}
		for (int k = 1; k <= HARMONICS; k++)
			sumKA += k * std::fabs(amps[t][k - 1]);
		for (int l = 0; l < wa::NUM_LEVELS; l++) {
			float* f = bank->frame(t, l);
			for (int i = 0; i < wa::FRAME_SIZE; i++)
				f[i] /= (float) peak;
		}
		*slopeFactor = std::max(*slopeFactor, sumKA / peak);
	}
	return bank;
}

static std::vector<std::vector<double>> ampsFor(int n, int seed) {
	std::vector<std::vector<double>> amps(n, std::vector<double>(HARMONICS));
	for (int t = 0; t < n; t++) {
		for (int k = 0; k < HARMONICS; k++) {
			double phase = (t + 1) * (k + 1) * 0.7 + seed;
			amps[t][k] = (k == 0 ? 1.0 : 0.6 / k) * std::cos(phase);
		}
	}
	return amps;
}

struct RenderStats {
	float maxStepL = 0.f, maxStepR = 0.f, peak = 0.f;
	bool finite = true;
	long audibleSamples = 0;
};

static void testClickFree() {
	std::printf("click-free morph, 48 kHz, 4 s with sweeps, jumps, table changes, add/remove, bank swap\n");
	const float sr = 48000.f;
	double slopeA, slopeB;
	std::unique_ptr<wa::WavetableBank> bankA(makeBank(ampsFor(8, 0), &slopeA));
	std::unique_ptr<wa::WavetableBank> bankB(makeBank(ampsFor(4, 3), &slopeB));

	wa::MorphEngine engine;
	engine.setSampleRate(sr);

	wa::EngineInput in;
	in.markers[0].active = true;
	in.markers[0].pos = 0.f;
	in.markers[0].wave = 0.f;
	in.markers[1].active = true;
	in.markers[1].pos = 1.f;
	in.markers[1].wave = 1.f;
	in.markers[1].pitch = 7.f;
	in.markers[1].pan = 0.6f;
	in.markers[1].bp = 0.6f;
	in.markers[2].pos = 0.5f;
	in.markers[2].wave = 3.f / 7.f;
	in.markers[2].pitch = -5.f;
	in.markers[2].pan = -0.8f;
	in.voct = 0.f;

	const wa::WavetableBank* active = bankA.get();
	const wa::WavetableBank* pending = nullptr;
	bool swapped = false;

	// Highest frequency reached: C4 + 7 semitones.
	double fmax = 261.6256 * std::pow(2.0, 7.0 / 12.0);
	double bound = std::max(slopeA, slopeB) * 2 * PI * fmax / sr;
	// Equal-power pan can scale a channel up to sqrt(2). Small allowance for the fades.
	float limit = (float) (bound * std::sqrt(2.0) * 1.2 + 0.01);

	RenderStats st;
	float prevL = 0.f, prevR = 0.f;
	const int total = (int) (4.0f * sr);
	for (int n = 0; n < total; n++) {
		float t = n / sr;
		// Position: 3 Hz sine sweep, then a 10 Hz square wave of hard jumps (slewed by the engine).
		if (t < 2.f)
			in.position = 0.5f + 0.5f * (float) std::sin(2 * PI * 3.0 * t);
		else if (t < 2.5f)
			in.position = (std::fmod(t * 10.f, 1.f) < 0.5f) ? 0.f : 1.f;
		else
			in.position = 0.3f;

		if (n == (int) (0.40f * sr)) in.markers[0].wave = 5.f / 7.f; // jump: crossfade
		if (n == (int) (0.405f * sr)) in.markers[0].wave = 2.f / 7.f; // jump arriving mid-fade
		if (n == (int) (1.0f * sr)) in.markers[2].active = true;
		if (n == (int) (1.5f * sr)) pending = bankB.get();
		if (n == (int) (2.6f * sr)) in.markers[2].active = false;
		if (n == (int) (3.0f * sr)) in.markers[1].bp = 0.f;   // filter on -> off
		if (n == (int) (3.2f * sr)) in.markers[0].pan = -1.f; // hard pan
		// Turning WAVE: the knob value arrives in UI-frame steps (60 Hz), 0 -> 1 in 0.25 s.
		if (t >= 3.4f && t < 3.65f && n % (int) (sr / 60.f) == 0)
			in.markers[0].wave = std::min(1.f, (t - 3.4f) / 0.25f);

		// Module protocol: hold silent while a swap is pending, swap once silent.
		in.bank = active;
		in.holdSilent = pending != nullptr;
		float l, r;
		engine.process(in, &l, &r);
		if (pending && engine.isSilent()) {
			active = pending;
			pending = nullptr;
			swapped = true;
		}

		if (!std::isfinite(l) || !std::isfinite(r))
			st.finite = false;
		if (n > 0) {
			st.maxStepL = std::max(st.maxStepL, std::fabs(l - prevL));
			st.maxStepR = std::max(st.maxStepR, std::fabs(r - prevR));
		}
		st.peak = std::max(st.peak, std::max(std::fabs(l), std::fabs(r)));
		if (std::fabs(l) + std::fabs(r) > 1e-4f)
			st.audibleSamples++;
		prevL = l;
		prevR = r;
	}

	std::printf("  max step L %.4f  R %.4f  limit %.4f  peak %.3f (x5 V = %.2f V)\n",
		st.maxStepL, st.maxStepR, limit, st.peak, st.peak * 5.f);
	CHECK(st.finite, "non-finite output");
	CHECK(swapped, "bank swap never happened (engine never went silent)");
	CHECK(st.maxStepL <= limit, "click on L: step %f > %f", st.maxStepL, limit);
	CHECK(st.maxStepR <= limit, "click on R: step %f > %f", st.maxStepR, limit);
	CHECK(st.peak * 5.f < 11.7f, "output beyond Eurorack range: %f V", st.peak * 5.f);
	CHECK(st.audibleSamples > total * 0.9, "too much silence: %ld of %d samples", st.audibleSamples, total);

	// Sensitivity: an instant switch between two of these tables would exceed the limit.
	float naive = 0.f;
	for (int i = 0; i < wa::FRAME_SIZE; i++)
		naive = std::max(naive, std::fabs(bankA->frame(0, 0)[i] - bankA->frame(5, 0)[i]));
	std::printf("  an instant table switch would step up to %.4f\n", naive);
	CHECK(naive > limit, "test cannot detect clicks: naive switch %f <= limit %f", naive, limit);
}

static void testSilenceWithoutBank() {
	std::printf("no bank -> silence\n");
	wa::MorphEngine engine;
	wa::EngineInput in;
	in.markers[0].active = true;
	float l = 1.f, r = 1.f, maxAbs = 0.f;
	for (int n = 0; n < 1000; n++) {
		engine.process(in, &l, &r);
		maxAbs = std::max(maxAbs, std::max(std::fabs(l), std::fabs(r)));
	}
	CHECK(maxAbs == 0.f, "expected silence, got %f", maxAbs);
}

static void testPitchAndPan() {
	std::printf("pitch +12 st doubles frequency; pan hard left mutes R\n");
	double slope;
	std::unique_ptr<wa::WavetableBank> bank(makeBank(std::vector<std::vector<double>>(1, {1, 0, 0, 0}), &slope));
	const float sr = 48000.f;
	auto countCrossings = [&](float pitch, float pan, float* maxR) {
		wa::MorphEngine engine;
		engine.setSampleRate(sr);
		wa::EngineInput in;
		in.markers[0].active = true;
		in.markers[0].pitch = pitch;
		in.markers[0].pan = pan;
		in.bank = bank.get();
		float l, r, prev = 0.f;
		int crossings = 0;
		*maxR = 0.f;
		for (int n = 0; n < (int) sr; n++) {
			engine.process(in, &l, &r);
			if (n > (int) (0.1f * sr)) {
				if (prev < 0.f && l >= 0.f)
					crossings++;
				*maxR = std::max(*maxR, std::fabs(r));
			}
			prev = l;
		}
		return crossings;
	};
	float maxR;
	int base = countCrossings(0.f, -1.f, &maxR);
	CHECK(maxR < 1e-3f, "hard left pan leaks into R: %f", maxR);
	int up = countCrossings(12.f, -1.f, &maxR);
	std::printf("  0 st: %d cycles in 0.9 s, +12 st: %d\n", base, up);
	CHECK(std::abs(base - 235) <= 2, "C4 should give ~235 cycles in 0.9 s, got %d", base);
	CHECK(std::abs(up - 2 * base) <= 3, "+12 st should double: %d vs %d", up, base);
}

/** Magnitude of frequency f in x (Goertzel), normalized so a unit sine gives ~0.5. */
static double tone(const std::vector<float>& x, double f, double sr) {
	double w = 2 * PI * f / sr, c = 2 * std::cos(w), s1 = 0, s2 = 0;
	for (float v : x) {
		double s0 = v + c * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / x.size();
}

static void testNoPitchSlide() {
	std::printf("audio-only morph: marker A -12 st, marker B 0 st, position halfway\n");
	double slope;
	std::unique_ptr<wa::WavetableBank> bank(makeBank(std::vector<std::vector<double>>(2, {1, 0, 0, 0}), &slope));
	const float sr = 48000.f;
	const double f = 261.6256;
	wa::MorphEngine engine;
	engine.setSampleRate(sr);
	wa::EngineInput in;
	in.markers[0].active = true;
	in.markers[0].pos = 0.f;
	in.markers[0].wave = 0.f;
	in.markers[0].pitch = -12.f;
	in.markers[1].active = true;
	in.markers[1].pos = 1.f;
	in.markers[1].wave = 1.f;
	in.markers[1].pitch = 0.f;
	in.position = 0.5f;
	in.bank = bank.get();

	std::vector<float> out;
	float l, r;
	for (int n = 0; n < (int) sr; n++) {
		engine.process(in, &l, &r);
		if (n >= (int) (0.2f * sr))
			out.push_back(l);
	}
	double low = tone(out, f / 2, sr), high = tone(out, f, sr), mid = tone(out, f * 0.75, sr);
	std::printf("  level at f/2 %.3f, at f %.3f, at 0.75 f (a slide would land here) %.4f\n", low, high, mid);
	CHECK(low > 0.1 && high > 0.1, "both marker pitches must sound: %f %f", low, high);
	CHECK(std::fabs(low - high) < 0.05, "halfway should be an equal mix: %f vs %f", low, high);
	CHECK(mid < 0.01, "energy between the pitches means the pitch is sliding: %f", mid);
}

/** One sine marker at C4, centered pan (so L equals the mono signal). */
struct SineRig {
	std::unique_ptr<wa::WavetableBank> bank;
	wa::MorphEngine engine;
	wa::EngineInput in;
	const float sr = 48000.f;
	SineRig() {
		double slope;
		bank.reset(makeBank(std::vector<std::vector<double>>(1, {1, 0, 0, 0}), &slope));
		engine.setSampleRate(sr);
		in.markers[0].active = true;
		in.bank = bank.get();
	}
	/** Renders `seconds`, returns the peak of L over the last half. */
	float peakOver(float seconds, std::vector<float>* tail = nullptr) {
		int n = (int) (seconds * sr);
		float l, r, peak = 0.f;
		for (int i = 0; i < n; i++) {
			engine.process(in, &l, &r);
			if (i >= n / 2) {
				peak = std::max(peak, std::fabs(l));
				if (tail)
					tail->push_back(l);
			}
		}
		return peak;
	}
};

static void testGain() {
	std::printf("gain: scaling, smoothed jump, limiter\n");
	{
		SineRig rig;
		rig.in.markers[0].gain = 0.5f;
		float half = rig.peakOver(0.3f);
		rig.in.markers[0].gain = 1.5f;
		float more = rig.peakOver(0.3f);
		std::printf("  peak at 50%%: %.3f, at 150%%: %.3f\n", half, more);
		CHECK(std::fabs(half - 0.5f) < 0.02f, "50%% gain peak %f", half);
		CHECK(std::fabs(more - 1.5f) < 0.03f, "150%% gain peak %f (below the limit, untouched)", more);
		rig.in.markers[0].gain = 0.f;
		CHECK(rig.peakOver(0.3f) < 1e-4f, "0%% gain must be silent");
	}
	{
		// Instant 100% -> 400% jump: smoothing must keep it free of a click.
		SineRig rig;
		rig.peakOver(0.2f);
		rig.in.markers[0].gain = 4.f;
		float l, r, prev = 0.f, maxStep = 0.f;
		for (int i = 0; i < (int) (0.05f * rig.sr); i++) {
			rig.engine.process(rig.in, &l, &r);
			if (i > 0)
				maxStep = std::max(maxStep, std::fabs(l - prev));
			prev = l;
		}
		// Natural slope of a sine at 4x gain, plus the 5 ms ramp itself.
		float limit = (float) (4.0 * 2 * PI * 261.6256 / rig.sr) * 1.2f + 3.f / (0.005f * rig.sr);
		std::printf("  gain jump 100%% -> 400%%: max step %.4f, limit %.4f\n", maxStep, limit);
		CHECK(maxStep <= limit, "gain jump clicks: %f > %f", maxStep, limit);
	}
	{
		// 400% = 20 V peak before limiting. Must end up at 10 V and stay a clean sine.
		SineRig rig;
		rig.in.markers[0].gain = 4.f;
		std::vector<float> tail;
		float peak = rig.peakOver(1.f, &tail);
		// Clean = no flattened tops. RMS/peak is 1/sqrt(2) for a pure sine and higher when clipped.
		double sumSq = 0.0;
		for (float v : tail)
			sumSq += (double) v * v;
		float crest = (float) (std::sqrt(sumSq / tail.size()) / peak);
		float worst = std::fabs(crest - 0.70710678f);
		std::printf("  400%% gain: peak %.3f (x5 V = %.2f V), rms/peak %.4f (pure sine 0.7071)\n", peak, peak * 5.f, crest);
		CHECK(peak <= 2.0001f, "limiter let %f V through", peak * 5.f);
		CHECK(peak > 1.95f, "limiter pulled too far down: %f", peak);
		CHECK(worst < 0.003f, "limited tone is distorted (rms/peak %f)", crest);
	}
}

static void testResonance() {
	std::printf("resonance: max Q with fast cutoff sweeps stays stable\n");
	SineRig rig;
	double slope;
	// A bright table, so the band-pass has something to ring on.
	rig.bank.reset(makeBank(std::vector<std::vector<double>>(1, {1, 0.8, 0.6, 0.5}), &slope));
	rig.in.bank = rig.bank.get();
	rig.in.markers[0].bp = 0.5f;
	rig.in.markers[0].res = 1.f;
	rig.in.markers[0].gain = 4.f;
	float l, r, peak = 0.f;
	bool finite = true;
	for (int i = 0; i < (int) (2.f * rig.sr); i++) {
		// Cutoff swept 20 Hz..20 kHz at 30 Hz (far faster than any hand on a knob).
		rig.in.markers[0].bp = 0.5f + 0.5f * (float) std::sin(2 * PI * 30.0 * i / rig.sr);
		rig.engine.process(rig.in, &l, &r);
		if (!std::isfinite(l) || !std::isfinite(r))
			finite = false;
		peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
	}
	std::printf("  peak %.3f (x5 V = %.2f V)\n", peak, peak * 5.f);
	CHECK(finite, "non-finite output at max resonance");
	CHECK(peak <= 2.0001f, "output beyond 10 V: %f V", peak * 5.f);
}

static void testWaveSweep() {
	std::printf("WAVE knob sweeps over 64 tables, knob updates at 60 Hz\n");
	const float sr = 48000.f;
	double slope;
	std::unique_ptr<wa::WavetableBank> bank(makeBank(ampsFor(64, 7), &slope));
	float limit = (float) (slope * 2 * PI * 261.6256 / sr * 1.2 + 0.01);

	// 1 s sweep = ~1 table per knob update (glide path), 0.5 s = ~2.1 (crossfade path),
	// 0.1 s = ~10 per update (a fast flick).
	for (float sweepTime : {2.f, 1.f, 0.5f, 0.1f}) {
		wa::MorphEngine engine;
		engine.setSampleRate(sr);
		wa::EngineInput in;
		in.markers[0].active = true;
		in.bank = bank.get();
		float l, r, prev = 0.f, maxStep = 0.f;
		int frame = (int) (sr / 60.f);
		int start = (int) (0.1f * sr), total = start + (int) ((sweepTime + 0.2f) * sr);
		for (int n = 0; n < total; n++) {
			if (n >= start && (n - start) % frame == 0)
				in.markers[0].wave = std::min(1.f, (n - start) / (sweepTime * sr));
			engine.process(in, &l, &r);
			if (n > start)
				maxStep = std::max(maxStep, std::fabs(l - prev));
			prev = l;
		}
		float perUpdate = 63.f / (sweepTime * 60.f);
		std::printf("  0 -> 64 in %.1f s (%.1f tables per knob update): max step %.4f, limit %.4f\n",
			sweepTime, perUpdate, maxStep, limit);
		CHECK(maxStep <= limit, "WAVE sweep in %.1f s clicks: %f > %f", sweepTime, maxStep, limit);
	}
}

static void testWaveSmooth() {
	std::printf("SMOOTH: WAVE jump 0 -> 1 over 64 tables\n");
	const float sr = 48000.f;
	double slope;
	std::unique_ptr<wa::WavetableBank> bank(makeBank(ampsFor(64, 7), &slope));
	float limit = (float) (slope * 2 * PI * 261.6256 / sr * 1.2 + 0.01);

	for (float smooth : {wa::MIN_WAVE_SMOOTH, 0.2f, 1.f, wa::MAX_WAVE_SMOOTH}) {
		wa::MorphEngine engine;
		engine.setSampleRate(sr);
		wa::EngineInput in;
		in.markers[0].active = true;
		in.markers[0].waveSmooth = smooth;
		in.bank = bank.get();
		float l, r, prev = 0.f, maxStep = 0.f;
		for (int n = 0; n < (int) (0.1f * sr); n++)
			engine.process(in, &l, &r);
		in.markers[0].wave = 1.f;
		// After one time constant a one-pole glide covers 63% of the way; check halfway there too.
		float idxAtT = -1.f;
		int steps = (int) (smooth * sr);
		for (int n = 0; n < std::max(steps, (int) (0.05f * sr)); n++) {
			engine.process(in, &l, &r);
			if (n > 0)
				maxStep = std::max(maxStep, std::fabs(l - prev));
			prev = l;
			if (n == steps - 1)
				idxAtT = engine.getTableIndex(0);
		}
		std::printf("  SMOOTH %5.0f ms: index after %4.0f ms = %5.1f of 63, max step %.4f (limit %.4f)\n",
			smooth * 1000.f, smooth * 1000.f, idxAtT, maxStep, limit);
		CHECK(maxStep <= limit, "SMOOTH %f s clicks: %f > %f", smooth, maxStep, limit);
		if (smooth <= wa::MIN_WAVE_SMOOTH) {
			// Minimum keeps the old behavior: a big jump crossfades straight to the target.
			CHECK(idxAtT > 62.9f, "minimum SMOOTH should jump (crossfade) to 63, got %f", idxAtT);
		}
		else {
			CHECK(std::fabs(idxAtT - 63.f * 0.632f) < 1.5f, "SMOOTH %f s: expected ~39.8 after one time constant, got %f", smooth, idxAtT);
		}
	}
}

/** Largest sample difference between neighboring tables (level 0): how much the output can change
per table of WAVE movement. */
static float maxAdjacentDiff(const wa::WavetableBank& bank) {
	float m = 0.f;
	for (int t = 0; t + 1 < bank.numTables; t++) {
		const float* a = bank.frame(t, 0);
		const float* b = bank.frame(t + 1, 0);
		for (int i = 0; i < wa::FRAME_SIZE; i++)
			m = std::max(m, std::fabs(b[i] - a[i]));
	}
	return m;
}

static void testWaveLfo() {
	std::printf("WAVE LFO: full depth at 20 Hz, click-free, independent of SMOOTH\n");
	const float sr = 48000.f;
	double slope;
	std::unique_ptr<wa::WavetableBank> bank(makeBank(ampsFor(64, 7), &slope));
	// Under modulation the output legitimately changes faster: waveform slope plus
	// (tables moved per sample) x (largest neighbor difference). A click is a jump far above that.
	float adj = maxAdjacentDiff(*bank);
	auto limitFor = [&](float tablesPerSample) {
		return (float) ((slope * 2 * PI * 261.6256 / sr + tablesPerSample * adj) * 1.2 + 0.01);
	};
	// 20 Hz at 50% depth: index speed peaks at 0.5 * 2*pi*20/sr * 63 tables per sample.
	float limit = limitFor((float) (0.5 * 2 * PI * 20.0 / sr * 63.0));

	for (float smooth : {wa::MIN_WAVE_SMOOTH, wa::MAX_WAVE_SMOOTH}) {
		wa::MorphEngine engine;
		engine.setSampleRate(sr);
		wa::EngineInput in;
		in.markers[0].active = true;
		in.markers[0].wave = 0.5f;
		in.markers[0].waveSmooth = smooth;
		in.markers[0].lfoAmount = 0.5f;
		in.bank = bank.get();
		float l, r, prev = 0.f, maxStep = 0.f, lo = 1e9f, hi = -1e9f;
		for (int n = 0; n < (int) sr; n++) {
			in.markers[0].lfo = (float) std::sin(2 * PI * 20.0 * n / sr);
			engine.process(in, &l, &r);
			if (n > (int) (0.1f * sr)) {
				maxStep = std::max(maxStep, std::fabs(l - prev));
				lo = std::min(lo, engine.getPlayedIndex(0));
				hi = std::max(hi, engine.getPlayedIndex(0));
			}
			prev = l;
		}
		std::printf("  SMOOTH %4.0f ms: played table range %.1f..%.1f of 0..63, max step %.4f (limit %.4f)\n",
			smooth * 1000.f, lo, hi, maxStep, limit);
		CHECK(lo < 1.f && hi > 62.f, "LFO depth reduced: %f..%f", lo, hi);
		CHECK(maxStep <= limit, "LFO modulation clicks: %f > %f", maxStep, limit);
	}

	// AMT knob steps (UI rate) and a sudden full-depth start must not click.
	wa::MorphEngine engine;
	engine.setSampleRate(sr);
	wa::EngineInput in;
	in.markers[0].active = true;
	in.markers[0].wave = 0.5f;
	in.bank = bank.get();
	float l, r, prev = 0.f, maxStep = 0.f;
	for (int n = 0; n < (int) sr; n++) {
		in.markers[0].lfo = (float) std::sin(2 * PI * 3.0 * n / sr);
		if (n == (int) (0.3f * sr))
			in.markers[0].lfoAmount = 1.f; // 0 -> 100% in one step
		engine.process(in, &l, &r);
		if (n > (int) (0.1f * sr))
			maxStep = std::max(maxStep, std::fabs(l - prev));
		prev = l;
	}
	// 3 Hz at 100%: 2*pi*3/sr * 63, plus the 50 ms AMT ramp moving the depth by up to
	// 1/(0.05*sr) * 63 tables per sample.
	float amtLimit = limitFor((float) (2 * PI * 3.0 / sr * 63.0 + 63.0 / (0.05 * sr)));
	std::printf("  AMT jump 0 -> 100%%: max step %.4f (limit %.4f)\n", maxStep, amtLimit);
	CHECK(maxStep <= amtLimit, "AMT jump clicks: %f > %f", maxStep, amtLimit);
}

int main() {
	testClickFree();
	testSilenceWithoutBank();
	testPitchAndPan();
	testNoPitchSlide();
	testGain();
	testResonance();
	testWaveSweep();
	testWaveSmooth();
	testWaveLfo();
	if (failures) {
		std::printf("%d check(s) FAILED\n", failures);
		return 1;
	}
	std::printf("all checks passed\n");
	return 0;
}
