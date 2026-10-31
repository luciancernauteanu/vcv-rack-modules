// Offline click/NaN tests for MorphEngine (no Rack needed).
// Build and run: cd tests && make run
//
// Method: build banks from a few low harmonics, so the steepest legitimate sample-to-sample step
// has a known analytic bound (sum of k*|a_k| * 2*pi*f/sr). Then run the engine through fast
// position sweeps, position jumps, table changes (including one mid-fade), marker add/remove and a
// bank swap. Any step above the bound is a click.

#include "ExpanderMessages.hpp"
#include "MorphEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
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
			int maxH = wa::levelHarmonics(l);
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
	in.markers[1].cutoff = 0.6f;
	in.markers[1].filterMode = wa::MODE_BP;
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
		if (n == (int) (3.0f * sr)) in.markers[1].cutoff = 0.f;   // filter on -> off
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
	rig.in.markers[0].cutoff = 0.5f;
	rig.in.markers[0].filterMode = wa::MODE_BP;
	rig.in.markers[0].res = 1.f;
	rig.in.markers[0].gain = 4.f;
	float l, r, peak = 0.f;
	bool finite = true;
	for (int i = 0; i < (int) (2.f * rig.sr); i++) {
		// Cutoff swept 20 Hz..20 kHz at 30 Hz (far faster than any hand on a knob).
		rig.in.markers[0].cutoff = 0.5f + 0.5f * (float) std::sin(2 * PI * 30.0 * i / rig.sr);
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
	std::printf("WAVE LFO: follows SMOOTH, click-free\n");
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

	// The LFO moves the glide target, so SMOOTH filters it like any WAVE move: a one-pole
	// glide with time constant SMOOTH has gain 1/sqrt(1 + (2*pi*f*SMOOTH)^2) at LFO rate f.
	// Measured as the peak-to-peak of the played table index, in steady state.
	const float amt = 0.2f;
	const float depthTables = amt * 63.f; // LFO swing at the input, before SMOOTH
	std::printf("  LFO 0.2 (wave 0.5) through SMOOTH: played swing, measured vs expected (tables)\n");
	for (float smooth : {wa::MIN_WAVE_SMOOTH, 0.2f, 2.f}) {
		for (float freq : {2.f, 20.f}) {
			wa::MorphEngine engine;
			engine.setSampleRate(sr);
			wa::EngineInput in;
			in.markers[0].active = true;
			in.markers[0].wave = 0.5f;
			in.markers[0].waveSmooth = smooth;
			in.markers[0].lfoAmount = amt;
			in.bank = bank.get();
			float l, r, prev = 0.f, maxStep = 0.f, lo = 1e9f, hi = -1e9f;
			int total = (int) (12.f * sr); // long enough for SMOOTH = 2 s to reach steady state
			for (int n = 0; n < total; n++) {
				in.markers[0].lfo = (float) std::sin(2 * PI * freq * n / sr);
				engine.process(in, &l, &r);
				if (n > total - (int) (2.f * sr)) {
					lo = std::min(lo, engine.getPlayedIndex(0));
					hi = std::max(hi, engine.getPlayedIndex(0));
					maxStep = std::max(maxStep, std::fabs(l - prev));
				}
				prev = l;
			}
			double w = 2 * PI * freq * smooth;
			float expected = depthTables / (float) std::sqrt(1.0 + w * w);
			float measured = (hi - lo) / 2.f;
			std::printf("  SMOOTH %5.0f ms, LFO %4.0f Hz: expected %6.2f, measured %6.2f\n",
				smooth * 1000.f, freq, expected, measured);
			CHECK(std::fabs(measured - expected) <= 0.1f * expected + 0.15f,
				"SMOOTH %f, %f Hz: swing %f, expected %f", smooth, freq, measured, expected);
			CHECK(maxStep <= limit, "LFO modulation clicks at SMOOTH %f, %f Hz: %f > %f", smooth, freq, maxStep, limit);
		}
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

static void testPreview() {
	std::printf("preview: start, switch tables, stop, during a marker change; no clicks\n");
	const float sr = 48000.f;
	double slope;
	std::unique_ptr<wa::WavetableBank> bank(makeBank(ampsFor(8, 1), &slope));
	wa::MorphEngine engine;
	engine.setSampleRate(sr);
	wa::EngineInput in;
	in.markers[0].active = true;
	in.markers[0].pos = 0.f;
	in.markers[0].wave = 0.f;
	in.markers[0].pitch = 12.f;
	in.markers[0].pan = -1.f;
	in.markers[1].active = true;
	in.markers[1].pos = 1.f;
	in.markers[1].wave = 1.f;
	in.markers[2].pos = 0.5f;
	in.position = 0.3f;
	in.bank = bank.get();

	// Highest pitch is marker 0 at +12 st; hard pan can scale a channel by sqrt(2).
	float limit = (float) (slope * 2 * PI * 261.6256 * 2.0 / sr * std::sqrt(2.0) * 1.2 + 0.01);
	float prevL = 0.f, prevR = 0.f, maxStep = 0.f, levelAtFull = 0.f, maxLR = 0.f;
	for (int n = 0; n < (int) (2.f * sr); n++) {
		float t = n / sr;
		in.previewTable = (t >= 0.3f && t < 1.6f) ? (t < 0.8f ? 2 : 6) : -1;
		in.markers[2].active = t >= 1.0f; // structural change while previewing
		float l, r;
		engine.process(in, &l, &r);
		if (n > 0)
			maxStep = std::max(maxStep, std::max(std::fabs(l - prevL), std::fabs(r - prevR)));
		prevL = l;
		prevR = r;
		if (t > 0.5f && t < 0.75f) {
			levelAtFull = std::max(levelAtFull, engine.getPreviewLevel());
			// Fully previewed: centered (no pan), so L == R.
			maxLR = std::max(maxLR, std::fabs(l - r));
		}
	}
	std::printf("  max step %.4f (limit %.4f), preview level %.2f, |L-R| while previewing %.5f\n", maxStep, limit, levelAtFull, maxLR);
	CHECK(maxStep <= limit, "preview clicks: %f > %f", maxStep, limit);
	CHECK(levelAtFull >= 0.999f, "preview never fully replaced the morph");
	CHECK(maxLR < 1e-5f, "preview output is not the centered table: |L-R| %f", maxLR);
	CHECK(engine.getPreviewLevel() == 0.f, "preview did not stop");
}

static float cutoffFor(float hz) {
	return std::log(hz / 20.f) / std::log(1000.f);
}

static void testFilterModes() {
	std::printf("marker filter modes on a C4 sine (261.6 Hz)\n");
	struct Case {
		const char* name;
		int mode;
		float hz;
		float lo, hi;
	} cases[] = {
		{"LP @ 2 kHz (passes)", wa::MODE_LP, 2000.f, 0.9f, 1.1f},
		{"LP @ 40 Hz (blocks)", wa::MODE_LP, 40.f, 0.f, 0.05f},
		{"HP @ 2 kHz (blocks)", wa::MODE_HP, 2000.f, 0.f, 0.05f},
		{"HP @ 40 Hz (passes)", wa::MODE_HP, 40.f, 0.9f, 1.1f},
		{"BP @ 261 Hz (center)", wa::MODE_BP, 261.6f, 0.9f, 1.1f},
		{"BP @ 4 kHz (off center)", wa::MODE_BP, 4000.f, 0.f, 0.12f},
		{"NOTCH @ 261 Hz (dip)", wa::MODE_NOTCH, 261.6f, 0.f, 0.05f},
		{"NOTCH @ 4 kHz (passes)", wa::MODE_NOTCH, 4000.f, 0.9f, 1.1f},
	};
	for (const Case& c : cases) {
		SineRig rig;
		rig.in.markers[0].cutoff = cutoffFor(c.hz);
		rig.in.markers[0].filterMode = c.mode;
		float peak = rig.peakOver(1.f);
		std::printf("  %-24s level %.3f (expected %.2f..%.2f)\n", c.name, peak, c.lo, c.hi);
		CHECK(peak >= c.lo && peak <= c.hi, "%s: level %f", c.name, peak);
	}

	std::printf("mode morph LP marker -> HP marker, and TYPE switched while playing: no clicks\n");
	SineRig rig;
	rig.in.markers[0].cutoff = cutoffFor(800.f);
	rig.in.markers[0].filterMode = wa::MODE_LP;
	rig.in.markers[1].active = true;
	rig.in.markers[1].pos = 1.f;
	rig.in.markers[1].cutoff = cutoffFor(200.f);
	rig.in.markers[1].filterMode = wa::MODE_HP;
	float prev = 0.f, maxStep = 0.f;
	for (int n = 0; n < (int) (2.f * rig.sr); n++) {
		float t = n / rig.sr;
		rig.in.position = 0.5f + 0.5f * (float) std::sin(2 * PI * 2.0 * t);
		if (n == (int) (0.7f * rig.sr)) rig.in.markers[0].filterMode = wa::MODE_NOTCH;
		if (n == (int) (1.3f * rig.sr)) rig.in.markers[1].filterMode = wa::MODE_BP;
		float l, r;
		rig.engine.process(rig.in, &l, &r);
		if (n > 0)
			maxStep = std::max(maxStep, std::fabs(l - prev));
		prev = l;
	}
	// A unit sine at 261.6 Hz steps at most 0.034 per sample; allow for the resonant tilt and fades.
	std::printf("  max step %.4f (limit 0.08)\n", maxStep);
	CHECK(maxStep <= 0.08f, "filter mode change clicks: %f", maxStep);
}

static void testDrive() {
	std::printf("drive: 0 is clean, more adds odd harmonics, level stays near full scale\n");
	for (float d : {0.f, 0.3f, 1.f}) {
		SineRig rig;
		rig.in.markers[0].drive = d;
		std::vector<float> tail;
		float peak = rig.peakOver(1.f, &tail);
		double f = 261.6256;
		double h1 = tone(tail, f, rig.sr), h3 = tone(tail, 3 * f, rig.sr);
		std::printf("  drive %.1f: peak %.3f, 3rd harmonic %.1f%% of fundamental\n", d, peak, 100.0 * h3 / h1);
		if (d == 0.f)
			CHECK(h3 / h1 < 0.005, "drive 0 is not clean: %f", h3 / h1);
		else
			CHECK(h3 / h1 > 0.05, "drive %.1f adds too little: %f", d, h3 / h1);
		CHECK(peak <= 1.1f, "drive raises the peak: %f", peak);
	}
	// Aliasing: a high note driven hard. Harmonics of 3520 Hz land on multiples of 3520; with
	// 48 kHz, the 7th harmonic (24640) folds to 23360 and the 9th (31680) to 16320 Hz.
	SineRig rig;
	rig.in.voct = std::log2(3520.0 / 261.6256);
	rig.in.markers[0].drive = 1.f;
	std::vector<float> tail;
	rig.peakOver(1.f, &tail);
	double fund = tone(tail, 3520.0, rig.sr);
	double alias = tone(tail, 16320.0, rig.sr);
	std::printf("  3520 Hz at full drive: 9th-harmonic alias at 16320 Hz is %.1f dB below the fundamental\n",
		20 * std::log10(alias / fund + 1e-12));
	CHECK(alias / fund < 0.03, "too much aliasing: %f", alias / fund);

	// The oversampled path is always in; it must stay flat across the audio band.
	for (double hz : {5000.0, 10000.0, 15000.0}) {
		SineRig flat;
		flat.in.voct = (float) std::log2(hz / 261.6256);
		float peak = flat.peakOver(0.5f);
		std::printf("  drive 0, %5.0f Hz sine: level %.3f\n", hz, peak);
		CHECK(peak > 0.95f && peak < 1.05f, "drive path not flat at %f Hz: %f", hz, peak);
	}
}

static void testMasterDrive() {
	std::printf("master drive: 0 is clean, warm (even + odd harmonics), bounded, no DC\n");
	double f = 261.6256;
	for (float d : {0.f, 0.4f, 1.f}) {
		SineRig rig;
		rig.in.masterDrive = d;
		std::vector<float> tail;
		float peak = rig.peakOver(1.f, &tail);
		double h1 = tone(tail, f, rig.sr), h2 = tone(tail, 2 * f, rig.sr), h3 = tone(tail, 3 * f, rig.sr);
		double dc = 0;
		for (float v : tail) dc += v;
		dc /= tail.size();
		std::printf("  drive %.1f: peak %.3f, 2nd %.1f%%, 3rd %.1f%%, DC %.4f\n", d, peak, 100 * h2 / h1, 100 * h3 / h1, dc);
		if (d == 0.f) {
			CHECK(h2 / h1 < 0.003 && h3 / h1 < 0.003, "master drive 0 is not clean");
		}
		else {
			CHECK(h2 / h1 > 0.01, "drive %.1f adds no even harmonic: %f", d, h2 / h1);
			CHECK(h3 / h1 > 0.02, "drive %.1f adds no odd harmonic: %f", d, h3 / h1);
		}
		CHECK(peak < 1.4f, "master drive %.1f is too loud: %f", d, peak);
		CHECK(std::fabs(dc) < 0.01, "master drive leaves DC: %f", dc);
	}
	SineRig a;
	a.in.voct = (float) std::log2(3520.0 / 261.6256);
	a.in.masterDrive = 1.f;
	std::vector<float> tail;
	a.peakOver(1.f, &tail);
	double alias = tone(tail, 16320.0, a.sr) / tone(tail, 3520.0, a.sr);
	std::printf("  3520 Hz at full drive: alias at 16320 Hz %.1f dB below\n", 20 * std::log10(alias + 1e-12));
	CHECK(alias < 0.03, "master drive aliases: %f", alias);
}

static void testMasterCv() {
	std::printf("master CV is added after smoothing (audio rate), and changes the sound\n");
	// LP at 200 Hz blocks a C4 sine's level; +1 (10 oct) of cutoff CV opens it, instantly.
	{
		SineRig rig;
		rig.in.masterMode = wa::MODE_LP;
		rig.in.masterCutoff = cutoffFor(80.f);
		float closed = rig.peakOver(0.5f);
		rig.in.masterCutoffMod = 0.6f; // 80 Hz * 2^6 = 5 kHz
		float open = rig.peakOver(0.5f);
		std::printf("  cutoff CV: closed %.3f, open %.3f\n", closed, open);
		CHECK(closed < 0.2f && open > 0.9f, "cutoff CV does nothing: %f -> %f", closed, open);
	}
	// Audio-rate: a 2 kHz sine on the cutoff modulates the output at that rate (sidebands).
	{
		SineRig rig;
		rig.in.masterMode = wa::MODE_LP;
		rig.in.masterCutoff = cutoffFor(300.f);
		rig.in.masterRes = 0.6f;
		std::vector<float> out;
		float l, r;
		for (int n = 0; n < (int) (0.6f * rig.sr); n++) {
			rig.in.masterCutoffMod = 0.25f * (float) std::sin(2 * PI * 2000.0 * n / rig.sr);
			rig.engine.process(rig.in, &l, &r);
			if (n > (int) (0.1f * rig.sr)) out.push_back(l);
		}
		double side = tone(out, 2000.0 + 261.6256, rig.sr) + tone(out, 2000.0 - 261.6256, rig.sr);
		std::printf("  audio-rate cutoff FM: sideband level %.4f\n", side);
		CHECK(side > 0.005, "cutoff CV is smoothed away at audio rate: %f", side);
	}
	// Drive CV, feedback CV, resonance CV all act.
	{
		SineRig a, b;
		b.in.masterDriveMod = 1.f;
		std::vector<float> ta, tb;
		a.peakOver(0.5f, &ta);
		b.peakOver(0.5f, &tb);
		double h3a = tone(ta, 3 * 261.6256, a.sr), h3b = tone(tb, 3 * 261.6256, b.sr);
		CHECK(h3b > h3a * 10 + 0.01, "drive CV does nothing");
	}
}

static void testMasterWarmth() {
	std::printf("master warmth: resonance compresses at high level instead of ringing linearly\n");
	// BP at the signal frequency with high RES: the gain at level 0.05 vs 1.0. A linear filter gives
	// the same gain; the saturating integrators give less at the loud level.
	double gains[2];
	int k = 0;
	for (float level : {0.05f, 1.f}) {
		SineRig rig;
		rig.in.masterMode = wa::MODE_BP;
		rig.in.masterCutoff = cutoffFor(261.6f);
		rig.in.masterRes = 1.f;
		rig.in.markers[0].gain = level;
		gains[k++] = rig.peakOver(1.f) / level;
	}
	std::printf("  BP, RES 100%%: gain %.2f at level 0.05, %.2f at level 1.0\n", gains[0], gains[1]);
	CHECK(gains[1] < gains[0] * 0.9, "no compression at high level: %f vs %f", gains[1], gains[0]);
}

static void testMasterFilter() {
	std::printf("master filter: off is transparent, spread splits L/R, feedback self-oscillates but stays bounded\n");
	{
		SineRig a, b;
		b.in.masterMode = -1;
		b.in.masterFdbk = 1.f; // ignored while off
		float maxDiff = 0.f;
		for (int n = 0; n < 20000; n++) {
			float l1, r1, l2, r2;
			a.engine.process(a.in, &l1, &r1);
			b.engine.process(b.in, &l2, &r2);
			// Skip the first 50 ms: the master starts as low-pass and settles to off over a few ms.
			if (n > 2400)
				maxDiff = std::max(maxDiff, std::fabs(l1 - l2) + std::fabs(r1 - r2));
		}
		std::printf("  off vs no filter: max difference %.6f\n", maxDiff);
		CHECK(maxDiff < 1e-4f, "master off is not transparent: %f", maxDiff);
	}
	{
		// LP at C4 with SPREAD +100% (+-1 octave): left cutoff 131 Hz, right 523 Hz.
		SineRig rig;
		rig.in.masterMode = wa::MODE_LP;
		rig.in.masterCutoff = cutoffFor(261.6f);
		rig.in.masterSpread = 0.5f;
		int n = (int) rig.sr;
		float l, r, pl = 0.f, pr = 0.f;
		for (int i = 0; i < n; i++) {
			rig.engine.process(rig.in, &l, &r);
			if (i > n / 2) {
				pl = std::max(pl, std::fabs(l));
				pr = std::max(pr, std::fabs(r));
			}
		}
		std::printf("  spread: left %.3f, right %.3f\n", pl, pr);
		CHECK(pr > pl * 1.8f, "spread does not open the right channel: L %f R %f", pl, pr);
	}
	{
		// Feedback: play 0.2 s, then silence (no bank -> engine input 0). Above the threshold the
		// filter keeps ringing; below it, it dies away.
		for (float fb : {0.4f, 1.f}) {
			SineRig rig;
			rig.in.masterMode = wa::MODE_BP;
			rig.in.masterCutoff = cutoffFor(500.f);
			rig.in.masterFdbk = fb;
			float l, r, late = 0.f, peak = 0.f;
			bool finite = true;
			for (int i = 0; i < (int) (2.f * rig.sr); i++) {
				if (i == (int) (0.2f * rig.sr))
					rig.in.markers[0].gain = 0.f;
				rig.engine.process(rig.in, &l, &r);
				finite = finite && std::isfinite(l) && std::isfinite(r);
				peak = std::max(peak, std::fabs(l));
				if (i > (int) (1.5f * rig.sr))
					late = std::max(late, std::fabs(l));
			}
			std::printf("  FDBK %3.0f%%: level 1.3 s after the input stopped %.4f, peak %.3f\n", fb * 100, late, peak);
			CHECK(finite, "feedback produced NaN");
			CHECK(peak <= 2.0001f, "feedback beyond the limiter: %f", peak);
			if (fb < 0.5f)
				CHECK(late < 0.001f, "low feedback should decay: %f", late);
			else
				CHECK(late > 0.1f, "full feedback should self-oscillate: %f", late);
		}
	}
}

static void testVcaLevel() {
	std::printf("expander GAIN as a VCA\n");
	CHECK(wa::vcaLevel(0.f, 0.f) == 1.f, "amount 0 must leave the level alone at 0 V");
	CHECK(wa::vcaLevel(0.f, 10.f) == 1.f, "amount 0 must leave the level alone at 10 V");
	CHECK(wa::vcaLevel(1.f, 0.f) == 0.f, "full amount, 0 V must be silent");
	CHECK(std::fabs(wa::vcaLevel(1.f, 5.f) - 0.5f) < 1e-6f, "full amount, 5 V = half");
	CHECK(wa::vcaLevel(1.f, 10.f) == 1.f, "full amount, 10 V = unity");
	CHECK(wa::vcaLevel(1.f, 15.f) == 1.f, "CV above 10 V must not exceed the knob");
	CHECK(wa::vcaLevel(1.f, -5.f) == 0.f, "negative CV must not invert");
	CHECK(std::fabs(wa::vcaLevel(0.5f, 0.f) - 0.5f) < 1e-6f, "half amount, 0 V = half");
	// In the engine: a silenced marker is silent, a half VCA is half level.
	for (float mul : {0.f, 0.5f, 1.f}) {
		SineRig rig;
		rig.in.markers[0].gain = 1.f * mul;
		float peak = rig.peakOver(0.5f);
		CHECK(std::fabs(peak - mul) < 0.01f, "VCA level %f gave peak %f", mul, peak);
	}
}

static void testMarkerCrossing() {
	std::printf("a marker dragged across another (order changes while playing): finite, no clicks\n");
	const float sr = 48000.f;
	double slope;
	std::unique_ptr<wa::WavetableBank> bank(makeBank(ampsFor(8, 2), &slope));
	wa::MorphEngine engine;
	engine.setSampleRate(sr);
	wa::EngineInput in;
	in.bank = bank.get();
	for (int i = 0; i < 3; i++) {
		in.markers[i].active = true;
		in.markers[i].pos = i * 0.5f;
		in.markers[i].wave = i / 2.f;
	}
	in.position = 0.5f;
	float prev = 0.f, maxStep = 0.f;
	bool finite = true;
	long audible = 0;
	int total = (int) (3.f * sr);
	for (int n = 0; n < total; n++) {
		float t = n / sr;
		// Marker 1 (the middle one) is dragged right across marker 3, then back across marker 1.
		in.markers[1].pos = 0.5f + 0.5f * std::sin((float) (2 * PI * 0.5 * t)) * 0.9f + (t > 2.f ? 0.f : 0.f);
		in.position = 0.75f;
		float l, r;
		engine.process(in, &l, &r);
		finite = finite && std::isfinite(l) && std::isfinite(r);
		if (n > 0)
			maxStep = std::max(maxStep, std::fabs(l - prev));
		prev = l;
		if (std::fabs(l) > 1e-3f)
			audible++;
	}
	// 261 Hz, 4 harmonics: same bound as the other click tests.
	float limit = (float) (slope * 2 * PI * 261.6256 / sr * std::sqrt(2.0) * 1.2 + 0.01);
	std::printf("  max step %.4f (limit %.4f), audible %.0f%% of the time\n", maxStep, limit, 100.0 * audible / total);
	CHECK(finite, "non-finite output while crossing markers");
	CHECK(maxStep <= limit, "marker crossing clicks: %f > %f", maxStep, limit);
	CHECK(audible > total * 0.9, "crossing causes too much silence: %ld of %d", audible, total);
}

/** RMS of 1 s of white-ish noise through an Svf mode at the given cutoff and RES. */
static double noiseRms(int mode, float cutoffHz, float res, bool* finite = nullptr) {
	const float sr = 48000.f;
	wa::Svf svf;
	float mix[wa::NUM_MIX];
	wa::modeWeights(true, mode, mix);
	uint32_t s = 12345;
	double sum = 0;
	long n = 0;
	for (int i = 0; i < (int) (2 * sr); i++) {
		s = s * 1664525u + 1013904223u;
		float x = ((s >> 8) * (1.f / 8388608.f) - 1.f) * 0.5f;
		float y = svf.process(x, cutoffHz, wa::filterQ(res), sr, mix);
		if (finite && !std::isfinite(y))
			*finite = false;
		if (i > (int) sr) {
			sum += (double) y * y;
			n++;
		}
	}
	return std::sqrt(sum / n);
}

static double sineGain(int mode, float cutoffHz, float sineHz, float res) {
	const float sr = 48000.f;
	wa::Svf svf;
	float mix[wa::NUM_MIX];
	wa::modeWeights(true, mode, mix);
	float peak = 0.f;
	for (int i = 0; i < (int) (1.5 * sr); i++) {
		float x = (float) std::sin(2 * PI * sineHz * i / sr);
		float y = svf.process(x, cutoffHz, wa::filterQ(res), sr, mix);
		if (i > (int) sr)
			peak = std::max(peak, std::fabs(y));
	}
	return peak;
}

static void testResonanceLevel() {
	std::printf("resonance never takes level away (passband stays put, broadband stays loud)\n");
	// A tone well inside the passband keeps its level at any RES.
	struct P { const char* name; int mode; float cutoff, tone; } passes[] = {
		{"LP, 150 Hz tone, cutoff 4 kHz", wa::MODE_LP, 4000.f, 150.f},
		{"HP, 6 kHz tone, cutoff 300 Hz", wa::MODE_HP, 300.f, 6000.f},
		{"Notch, 150 Hz tone, cutoff 4 kHz", wa::MODE_NOTCH, 4000.f, 150.f},
	};
	for (const P& p : passes) {
		double g0 = sineGain(p.mode, p.cutoff, p.tone, 0.f), g1 = sineGain(p.mode, p.cutoff, p.tone, 1.f);
		std::printf("  %-34s res 0%%: %.3f  res 100%%: %.3f\n", p.name, g0, g1);
		CHECK(g1 > g0 * 0.97 && g1 < g0 * 1.05, "%s: passband level moved with RES: %f -> %f", p.name, g0, g1);
	}
	// Broadband (noise) must not get quieter as RES rises. BP is allowed a little: the band narrows.
	struct B { const char* name; int mode; float cutoff; double minRatio; } broad[] = {
		{"LP @ 1 kHz noise", wa::MODE_LP, 1000.f, 1.0},
		{"HP @ 1 kHz noise", wa::MODE_HP, 1000.f, 1.0},
		{"Notch @ 1 kHz noise", wa::MODE_NOTCH, 1000.f, 0.97},
		{"BP @ 1 kHz noise", wa::MODE_BP, 1000.f, 0.75},
	};
	for (const B& b : broad) {
		bool finite = true;
		double r0 = noiseRms(b.mode, b.cutoff, 0.f), r1 = noiseRms(b.mode, b.cutoff, 1.f, &finite);
		std::printf("  %-22s rms res 0%%: %.3f  res 100%%: %.3f (x%.2f)\n", b.name, r0, r1, r1 / r0);
		CHECK(finite, "%s: not finite", b.name);
		CHECK(r1 >= r0 * b.minRatio, "%s gets quieter with RES: %f -> %f", b.name, r0, r1);
	}
}

static void testLimiter() {
	std::printf("output protection: look-ahead limiter, DC block, NaN guard\n");
	const float sr = 48000.f;
	const float ceil = 2.f;
	{
		// Below the ceiling: unchanged (only delayed by the look-ahead).
		wa::SafetyLimiter lim;
		lim.setSampleRate(sr);
		int lat = lim.getLatency();
		std::vector<float> in, out;
		for (int i = 0; i < (int) sr; i++) {
			float x = 1.5f * (float) std::sin(2 * PI * 261.6256 * i / sr);
			float l, r;
			lim.process(x, -x, ceil, &l, &r);
			in.push_back(x);
			out.push_back(l);
		}
		float worst = 0.f;
		for (size_t i = 24000; i < in.size(); i++)
			worst = std::max(worst, std::fabs(out[i] - in[i - lat]));
		std::printf("  below the ceiling: max difference from the delayed input %.4f (latency %d samples = %.2f ms)\n", worst, lat, 1000.f * lat / sr);
		// The DC blocker (about 4 Hz) leads the phase by under 1 degree at 261 Hz: a few percent of the sample
		// difference, not a change of level.
		CHECK(worst < 0.03f, "limiter alters a signal below the ceiling: %f", worst);
	}
	{
		// A sudden jump to 4x the ceiling: never above the ceiling, and no click from the gain change.
		wa::SafetyLimiter lim;
		lim.setSampleRate(sr);
		float peak = 0.f, maxStep = 0.f, prev = 0.f, minGain = 1.f;
		for (int i = 0; i < (int) (1.5 * sr); i++) {
			float amp = (i < (int) (0.3 * sr)) ? 0.5f : 8.f;
			float x = amp * (float) std::sin(2 * PI * 110.0 * i / sr);
			float l, r;
			lim.process(x, x, ceil, &l, &r);
			peak = std::max(peak, std::fabs(l));
			if (i > 0)
				maxStep = std::max(maxStep, std::fabs(l - prev));
			prev = l;
			minGain = std::min(minGain, lim.getGain());
		}
		// A 110 Hz sine at the ceiling steps at most 2*pi*110/48000*2 = 0.029 per sample.
		std::printf("  sudden 4x overload: peak %.4f (ceiling %.1f), max step %.4f, gain down to %.2f\n", peak, ceil, maxStep, minGain);
		CHECK(peak <= ceil + 1e-4f, "limiter let %f through (ceiling %f)", peak, ceil);
		CHECK(maxStep < 0.1f, "limiter clicks on a sudden overload: step %f", maxStep);
		CHECK(minGain < 0.3f && minGain > 0.2f, "limiter gain not as expected: %f", minGain);
	}
	{
		// Extreme and invalid input.
		wa::SafetyLimiter lim;
		lim.setSampleRate(sr);
		float l = 0, r = 0, peak = 0;
		bool finite = true;
		for (int i = 0; i < (int) sr; i++) {
			float x = (i == 1000) ? std::numeric_limits<float>::quiet_NaN() : (i == 2000) ? std::numeric_limits<float>::infinity() : (i % 7 == 0 ? 1e6f : -1e6f);
			lim.process(x, -x, ceil, &l, &r);
			finite = finite && std::isfinite(l) && std::isfinite(r);
			peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
		}
		std::printf("  NaN, infinity and 1e6 input: finite %d, peak %.3f\n", (int) finite, peak);
		CHECK(finite, "limiter output not finite");
		CHECK(peak <= ceil + 1e-4f, "limiter exceeded the ceiling on extreme input: %f", peak);
	}
	{
		// DC offset is removed; gain recovers after a burst.
		wa::SafetyLimiter lim;
		lim.setSampleRate(sr);
		float l = 0, r = 0;
		for (int i = 0; i < (int) (2 * sr); i++)
			lim.process(1.5f, 1.5f, ceil, &l, &r);
		std::printf("  1.5 (7.5 V) of constant DC for 2 s: output %.4f\n", l);
		CHECK(std::fabs(l) < 0.01f, "DC passes the limiter: %f", l);
		for (int i = 0; i < (int) (0.1 * sr); i++) {
			float x = 6.f * (float) std::sin(2 * PI * 200.0 * i / sr);
			lim.process(x, x, ceil, &l, &r);
		}
		float g1 = lim.getGain();
		for (int i = 0; i < (int) (2 * sr); i++)
			lim.process(0.1f * (float) std::sin(2 * PI * 200.0 * i / sr), 0.f, ceil, &l, &r);
		std::printf("  after a burst: gain %.2f, 2 s later %.3f\n", g1, lim.getGain());
		CHECK(g1 < 0.5f && lim.getGain() > 0.99f, "limiter does not recover: %f -> %f", g1, lim.getGain());
	}
	{
		// Steady loud low note: the gain stays steady inside each cycle (no waveform distortion).
		wa::SafetyLimiter lim;
		lim.setSampleRate(sr);
		float gmin = 1.f, gmax = 0.f, l, r;
		for (int i = 0; i < (int) (2 * sr); i++) {
			lim.process(5.f * (float) std::sin(2 * PI * 40.0 * i / sr), 0.f, ceil, &l, &r);
			if (i > (int) sr) {
				gmin = std::min(gmin, lim.getGain());
				gmax = std::max(gmax, lim.getGain());
			}
		}
		std::printf("  40 Hz at 2.5x the ceiling: gain stays between %.4f and %.4f\n", gmin, gmax);
		CHECK(gmax - gmin < 0.01f, "limiter modulates a steady note: %f..%f", gmin, gmax);
	}
}

static void testMasterGainAndProtection() {
	std::printf("master gain, and the output never exceeds 10 V whatever is turned up\n");
	for (float g : {0.f, 0.5f, 1.f, 2.f}) {
		SineRig rig;
		rig.in.masterGain = g;
		float peak = rig.peakOver(1.f);
		std::printf("  master gain %3.0f%%: peak %.3f\n", g * 100, peak);
		CHECK(std::fabs(peak - std::min(g, 2.f)) < 0.02f, "master gain %f gave peak %f", g, peak);
	}
	// Everything at once: 400% artifact gain, 200% master, full drive, resonant filter with feedback.
	SineRig rig;
	for (int m = 0; m < 3; m++) {
		rig.in.markers[m].active = true;
		rig.in.markers[m].pos = m * 0.5f;
		rig.in.markers[m].gain = 4.f;
		rig.in.markers[m].drive = 1.f;
		rig.in.markers[m].cutoff = 0.5f;
		rig.in.markers[m].res = 1.f;
	}
	rig.in.masterGain = 2.f;
	rig.in.masterDrive = 1.f;
	rig.in.masterMode = wa::MODE_BP;
	rig.in.masterCutoff = 0.5f;
	rig.in.masterRes = 1.f;
	rig.in.masterFdbk = 1.f;
	float l, r, peak = 0.f;
	bool finite = true;
	for (int i = 0; i < (int) (4.f * rig.sr); i++) {
		rig.in.position = 0.5f + 0.5f * (float) std::sin(2 * PI * 3.0 * i / rig.sr);
		rig.engine.process(rig.in, &l, &r);
		finite = finite && std::isfinite(l) && std::isfinite(r);
		peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
	}
	std::printf("  everything maxed: peak %.4f (x5 V = %.2f V)\n", peak, peak * 5.f);
	CHECK(finite, "non-finite output with everything maxed");
	CHECK(peak <= 2.0001f, "protection failed: %f V", peak * 5.f);
}

int main() {
	testResonanceLevel();
	testLimiter();
	testMasterGainAndProtection();
	testMarkerCrossing();
	testVcaLevel();
	testMasterDrive();
	testMasterCv();
	testFilterModes();
	testDrive();
	testMasterWarmth();
	testMasterFilter();
	testPreview();
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
