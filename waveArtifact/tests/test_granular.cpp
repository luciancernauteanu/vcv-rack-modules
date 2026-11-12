// Offline tests for GrainEngine (no Rack needed). Build and run: cd tests && make run
//
// Every check prints the measured value next to its threshold. Sources: stationary noise (level and
// repeat tests), pure sines (pitch, aliasing, clicks).

#include "GrainEngine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

static std::unique_ptr<wa::WavetableBank> makeBank(const std::vector<float>& mono, unsigned channels = 1) {
	std::unique_ptr<wa::WavetableBank> bank(new wa::WavetableBank);
	wa::buildSampleBuffer(mono.data(), mono.size() / channels, channels, SRC_SR, bank->sample);
	return bank;
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

static std::vector<float> sineSource(double seconds, double hz, float amp = 0.5f) {
	std::vector<float> v((size_t) (seconds * SRC_SR));
	for (size_t i = 0; i < v.size(); i++)
		v[i] = amp * (float) std::sin(2 * PI * hz * i / SRC_SR);
	return v;
}

/** Engine + bank + clock + input, stepped one sample at a time. */
struct Rig {
	std::unique_ptr<wa::GrainEngine> engine{new wa::GrainEngine};
	std::unique_ptr<wa::WavetableBank> bank;
	wa::ClockTracker clock;
	wa::GrainInput in;
	float bpm = 120.f;
	long sample = 0;

	explicit Rig(std::unique_ptr<wa::WavetableBank> b) : bank(std::move(b)) {
		engine->setSampleRate(SR);
		in.bank = bank.get();
		in.clock = &clock;
		in.markers[0].active = true;
		in.masterMode = -1;
		in.masterDrive = 0.f;
	}

	void step(float* l, float* r) {
		clock.processInternal(bpm, SR);
		engine->process(in, l, r);
		in.reset = false;
		sample++;
	}

	/** Runs n samples; fills l / r when given. */
	void run(long n, std::vector<float>* l = nullptr, std::vector<float>* r = nullptr) {
		for (long i = 0; i < n; i++) {
			float a, b;
			step(&a, &b);
			if (l)
				l->push_back(a);
			if (r)
				r->push_back(b);
		}
	}
};

static double rms(const std::vector<float>& v, size_t from, size_t to) {
	double s = 0.0;
	for (size_t i = from; i < to; i++)
		s += (double) v[i] * v[i];
	return std::sqrt(s / std::max<size_t>(to - from, 1));
}

/** Goertzel magnitude (amplitude) at hz */
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

static void settle(Rig& rig, double seconds = 0.5) {
	rig.run((long) (seconds * SR));
}

// ---------------------------------------------------------------------------------------------

static void testSampleBuffer() {
	std::printf("sample buffer\n");
	std::vector<float> src = sineSource(2.0, 1000.0, 0.25f);
	auto bank = makeBank(src);
	const wa::SampleBuffer& s = bank->sample;
	CHECK(s.valid() && s.channels == 1, "mono sample kept");
	CHECK(s.levelFrames(0) == src.size(), "level 0 has all frames");
	for (int k = 1; k < wa::SAMPLE_LEVELS; k++)
		CHECK(std::abs((long) s.levelFrames(k) - (long) ((s.levelFrames(k - 1) + 1) / 2)) <= 1, "level %d is half the previous", k);
	double r = 0.0;
	for (float v : s.levels[0])
		r += (double) v * v;
	r = std::sqrt(r / s.levels[0].size()) * s.normGain;
	std::printf("  normalized RMS %.3f (target 0.4), normGain %.2f\n", r, s.normGain);
	CHECK(std::fabs(r - 0.4) < 0.02, "RMS %.3f", r);
	// 1 kHz is far below every level's cutoff: the level-2 copy must keep it at full amplitude
	// (level 2 runs at 11025 Hz, 1 kHz is well inside).
	double peak = 0.0;
	for (size_t i = s.levelFrames(2) / 4; i < s.levelFrames(2) * 3 / 4; i++)
		peak = std::max(peak, (double) std::fabs(s.levels[2][i]));
	std::printf("  level 2 peak %.3f (source 0.25)\n", peak);
	CHECK(std::fabs(peak - 0.25) < 0.01, "level 2 keeps in-band content: %.3f", peak);
	// A tone at 15 kHz is above level 1's cutoff (5.5 kHz): it must be gone there.
	std::vector<float> hi = sineSource(1.0, 15000.0, 0.25f);
	auto hb = makeBank(hi);
	double peak1 = 0.0;
	for (size_t i = hb->sample.levelFrames(1) / 4; i < hb->sample.levelFrames(1) * 3 / 4; i++)
		peak1 = std::max(peak1, (double) std::fabs(hb->sample.levels[1][i]));
	std::printf("  15 kHz tone in level 1: %.5f (source 0.25) = %.1f dB\n", peak1, 20 * std::log10(peak1 / 0.25 + 1e-12));
	CHECK(peak1 < 0.25 * 0.01, "level 1 removes 15 kHz: %.5f", peak1);
	// Stereo kept as stereo
	std::vector<float> st(8192 * 2, 0.1f);
	auto sb = makeBank(st, 2);
	CHECK(sb->sample.channels == 2 && sb->sample.frames == 8192, "stereo kept");
}

static void testLevelFlat() {
	std::printf("level across SIZE x DENSITY (free and synced)\n");
	auto src = noiseSource(8.0);
	double lo = 1e9, hi = 0.0, allLo = 1e9, allHi = 0.0;
	for (int sync = 0; sync < 2; sync++) {
		for (float density : {0.35f, 0.55f, 0.8f}) {
			for (float size : {0.2f, 0.45f, 0.7f}) {
				Rig rig(makeBank(src));
				wa::GrainMarkerInput& m = rig.in.markers[0];
				m.density = density;
				m.size = size;
				m.sync = sync;
				m.start = 0.1f;
				m.scan = 0.4f;
				m.spray = 0.2f;
				settle(rig, 1.5);
				std::vector<float> l;
				rig.run((long) (3 * SR), &l);
				double r = rms(l, 0, l.size());
				if (std::getenv("GRAIN_VERBOSE")) {
					double hz = sync ? 1.0 / (wa::SYNC_DIVISIONS[wa::lfoDivision(density, false, false)].beats * 0.5) : wa::grainFreeHz(density);
					double sec = sync ? wa::grainSizeSyncRatio(size) / hz : wa::grainSizeFreeSeconds(size);
					std::printf("    sync %d: %.1f grains/s x %.1f ms (overlap %.2f): RMS %.4f\n", sync, hz, sec * 1000, hz * sec, r);
				}
				double hz = sync ? 1.0 / (wa::SYNC_DIVISIONS[wa::lfoDivision(density, false, false)].beats * 0.5) : wa::grainFreeHz(density);
				double sec = sync ? wa::grainSizeSyncRatio(size) / hz : wa::grainSizeFreeSeconds(size);
				double overlap = hz * sec;
				allLo = std::min(allLo, r);
				allHi = std::max(allHi, r);
				// (fewer than 5 grains per second: a 3 s measurement holds only a few grains)
				if (overlap >= 0.45 && hz >= 5.0) {
					lo = std::min(lo, r);
					hi = std::max(hi, r);
				}
			}
		}
	}
	double spread = 20 * std::log10(hi / lo);
	double allSpread = 20 * std::log10(allHi / allLo);
	std::printf("  overlap >= 0.45 and >= 5 grains/s: RMS %.4f .. %.4f = %.2f dB spread (limit 3)\n", lo, hi, spread);
	std::printf("  all configurations incl. sparse pulses: %.2f dB spread (limit 13)\n", allSpread);
	CHECK(spread < 3.0, "level spread %.2f dB", spread);
	CHECK(allSpread < 13.0, "level spread incl. sparse %.2f dB", allSpread);
}

static void testSyncGrid() {
	std::printf("synced grains land on the clock grid\n");
	auto src = noiseSource(6.0);
	for (float bpm : {120.f, 117.3f, 203.9f}) {
		Rig rig(makeBank(src));
		rig.bpm = bpm;
		wa::GrainMarkerInput& m = rig.in.markers[0];
		m.sync = true;
		// 1/8 note: find the knob value that selects it
		float knob = 0.f;
		for (float k = 0.f; k <= 1.f; k += 0.001f) {
			if (wa::SYNC_DIVISIONS[wa::lfoDivision(k, false, false)].beats == 0.5f) {
				knob = k;
				break;
			}
		}
		m.density = knob;
		m.size = 0.2f;
		double beatSamples = 60.0 * SR / bpm;
		double interval = 0.5 * beatSamples;
		uint32_t seen = 0;
		std::vector<long> onsets;
		long total = (long) (20 * SR);
		for (long i = 0; i < total; i++) {
			float a, b;
			rig.step(&a, &b);
			if (rig.engine->eventCount() != seen) {
				seen = rig.engine->eventCount();
				onsets.push_back(i);
			}
		}
		// The first grid point (beat 0) may not fire (entering sync waits for the next one): use all later.
		double worst = 0.0;
		long first = onsets.empty() ? 0 : onsets[0];
		double firstExpected = std::round(first / interval) * interval;
		for (size_t k = 0; k < onsets.size(); k++) {
			double expected = firstExpected + (double) k * interval;
			worst = std::max(worst, std::fabs(onsets[k] - expected));
		}
		// Beat edges of the clock fall on whole samples while the beat length is not whole, so the grid
		// itself moves by up to a sample; the grains' sub-sample delay removes that from the sound.
		std::printf("  %.1f BPM: %zu grains in 20 s (expect %.0f), worst onset error %.2f samples (limit 1.05)\n",
			bpm, onsets.size(), 20.0 * SR / interval, worst);
		CHECK(std::fabs((double) onsets.size() - 20.0 * SR / interval) <= 2, "grain count");
		CHECK(worst <= 1.05, "onset error %.2f samples", worst);
	}
	// RESET: the grid restarts, a grain on the very next sample
	Rig rig(makeBank(src));
	wa::GrainMarkerInput& m = rig.in.markers[0];
	m.sync = true;
	m.density = 1.f; // fastest division
	m.size = 0.1f;
	settle(rig, 1.234);
	uint32_t before = rig.engine->eventCount();
	rig.clock.reset();
	rig.in.reset = true;
	long start = rig.sample;
	long firstAfter = -1;
	for (int i = 0; i < 4000 && firstAfter < 0; i++) {
		float a, b;
		rig.step(&a, &b);
		if (rig.engine->eventCount() != before)
			firstAfter = rig.sample - start;
	}
	std::printf("  RESET: first grain %ld samples after the reset (limit 2)\n", firstAfter);
	CHECK(firstAfter >= 0 && firstAfter <= 2, "grain on beat 0 after RESET");
}

static void testScanMapping() {
	std::printf("scan\n");
	auto src = noiseSource(10.0);
	// Synced: slice = START .. end of sample, 4 beats long. Grain positions follow the beat.
	{
		Rig rig(makeBank(src));
		wa::GrainMarkerInput& m = rig.in.markers[0];
		m.sync = true;
		m.start = 0.2f;
		m.density = 1.f;
		m.size = 0.1f;
		m.scan = 0.03f + 0.97f * (2.f / 7.f); // index 2 = 1 beat? find the knob that gives 4 beats below
		for (float k = 0.04f; k <= 1.f; k += 0.001f) {
			if (wa::grainLoopBeats(k) == 4.f) {
				m.scan = k;
				break;
			}
		}
		rig.clock.reset();
		rig.in.reset = true;
		uint32_t seen = 0;
		double worst = 0.0;
		long total = (long) (8 * SR); // 4 beats at 120 BPM = 2 s: four loops
		for (long i = 0; i < total; i++) {
			float a, b;
			rig.step(&a, &b);
			if (rig.engine->eventCount() != seen) {
				seen = rig.engine->eventCount();
				const wa::GrainEvent& ev = rig.engine->events()[(seen - 1) % wa::GrainEngine::NUM_EVENTS];
				double t = i / SR;
				double phase = std::fmod(t / 2.0, 1.0); // 4 beats at 120 BPM
				double expected = 0.2 + 0.8 * phase;
				double err = std::fabs(ev.pos - expected);
				err = std::min(err, 0.8 - err); // a grain on the loop point may be on either side of it
				worst = std::max(worst, err);
			}
		}
		std::printf("  synced slice read position vs beat: worst error %.4f of the sample (limit 0.01)\n", worst);
		CHECK(worst < 0.01, "synced scan error %.4f", worst);
	}
	// Free: speed knob 0.5 = 1x real time: 1 s moves 1/10 of a 10 s sample
	{
		Rig rig(makeBank(src));
		wa::GrainMarkerInput& m = rig.in.markers[0];
		m.start = 0.f;
		m.density = 0.7f;
		m.size = 0.3f;
		m.scan = 0.5f;
		settle(rig, 0.2);
		float p0 = rig.engine->getReadPosition(0);
		settle(rig, 1.0);
		float p1 = rig.engine->getReadPosition(0);
		std::printf("  free scan, 1 s at 1x: moved %.4f of the sample (expect 0.1000)\n", p1 - p0);
		CHECK(std::fabs((p1 - p0) - 0.1f) < 0.005f, "free scan speed");
		m.scan = 0.f;
		settle(rig, 0.3);
		float p2 = rig.engine->getReadPosition(0);
		settle(rig, 0.5);
		CHECK(rig.engine->getReadPosition(0) == p2, "scan 0 stays put");
		m.scan = -0.5f;
		settle(rig, 0.5);
		float p3 = rig.engine->getReadPosition(0);
		std::printf("  reverse scan: %.4f -> %.4f\n", p2, p3);
		CHECK(p3 < p2 || p3 > 0.5f, "negative scan goes backwards (or wrapped)");
	}
	// Slice end: with a second marker the first one's slice stops at its START
	{
		Rig rig(makeBank(src));
		rig.in.markers[1].active = true;
		rig.in.markers[1].pos = 1.f;
		rig.in.markers[0].start = 0.1f;
		rig.in.markers[1].start = 0.3f;
		rig.in.markers[0].scan = 0.75f; // 1.5x
		rig.in.markers[0].density = 0.8f;
		rig.in.markers[0].size = 0.3f;
		float lo = 1.f, hi = 0.f;
		for (int i = 0; i < (int) (3 * SR); i++) {
			float a, b;
			rig.step(&a, &b);
			float p = rig.engine->getReadPosition(0);
			if (i > 1000) {
				lo = std::min(lo, p);
				hi = std::max(hi, p);
			}
		}
		std::printf("  slice 0.1 .. 0.3: read position stayed in %.3f .. %.3f\n", lo, hi);
		CHECK(lo >= 0.1f - 1e-4f && hi <= 0.3f + 1e-4f, "scan stays inside the slice");
	}
}

static void testSeededRepeat() {
	std::printf("seeded randomness repeats every loop cycle\n");
	auto src = noiseSource(6.0);
	for (int seeded = 1; seeded >= 0; seeded--) {
		Rig rig(makeBank(src));
		rig.bpm = 120.f;
		rig.in.seeded = seeded != 0;
		wa::GrainMarkerInput& m = rig.in.markers[0];
		m.sync = true;
		m.density = 1.f;
		m.size = 0.3f;
		m.spray = 0.5f;
		m.pitchScatter = 0.3f;
		m.spread = 0.8f;
		m.start = 0.1f;
		for (float k = 0.04f; k <= 1.f; k += 0.001f) {
			if (wa::grainLoopBeats(k) == 2.f) {
				m.scan = k;
				break;
			}
		}
		rig.clock.reset();
		rig.in.reset = true;
		std::vector<float> l;
		rig.run((long) (4 * SR), &l); // 2 beats = 1 s at 120 BPM: four cycles
		size_t cyc = (size_t) SR;
		double num = 0, d1 = 0, d2 = 0;
		for (size_t i = 0; i < cyc; i++) {
			double a = l[2 * cyc + i], b = l[3 * cyc + i];
			num += a * b;
			d1 += a * a;
			d2 += b * b;
		}
		double corr = num / std::sqrt(d1 * d2 + 1e-30);
		if (std::getenv("GRAIN_VERBOSE")) {
			long first = -1, count = 0;
			double maxd = 0;
			for (size_t i = 0; i < cyc; i++) {
				double d = std::fabs(l[2 * cyc + i] - l[3 * cyc + i]);
				maxd = std::max(maxd, d);
				if (d > 1e-3) {
					count++;
					if (first < 0)
						first = (long) i;
				}
			}
			std::printf("    differing samples (>1e-3): %ld of %zu, first at %ld, max diff %.4f\n", count, cyc, first, maxd);
		}
		std::printf("  seeded=%d: correlation of cycle 3 vs cycle 4 = %.4f\n", seeded, corr);
		if (seeded)
			CHECK(corr > 0.999, "seeded cycles repeat (%.4f)", corr);
		else
			CHECK(corr < 0.5, "unseeded cycles differ (%.4f)", corr);
	}
}

static void testPitchAndAlias() {
	std::printf("pitch accuracy and aliasing\n");
	// 1 kHz source, 1 V up: the cloud plays 2 kHz. Measured on sparse, separate grains: in a dense
	// cloud whose grains all start at the same source position the loop is one grain interval long, so
	// the pitch is pulled toward a multiple of the grain rate (synchronous granular synthesis; the
	// PITCH grain-trigger mode, planned for later, is the answer to that).
	for (float volts : {-1.f, 0.f, 1.f}) {
		Rig rig(makeBank(sineSource(6.0, 1000.0)));
		rig.in.voct = volts;
		wa::GrainMarkerInput& m = rig.in.markers[0];
		m.density = 0.3f;
		m.size = 0.4f;
		m.window = wa::GRAIN_HANN;
		settle(rig, 0.5);
		std::vector<float> l;
		rig.run((long) (6 * SR), &l);
		double want = 1000.0 * std::exp2(volts) * SRC_SR / SR; // the source is 44.1k, the engine 48k
		// The source's pitch in engine terms: a 44.1k sample at original speed plays 1000 Hz (rate-corrected)
		want = 1000.0 * std::exp2(volts);
		// Frequency from upward zero crossings (the cloud's grain-rate sidebands blur a spectral peak,
		// but every grain is the same sine, so the crossings are the pitch).
		std::vector<double> xs;
		for (size_t i = 1; i < l.size(); i++) {
			if (l[i - 1] < 0.f && l[i] >= 0.f)
				xs.push_back((double) (i - 1) + (-l[i - 1]) / (double) (l[i] - l[i - 1]));
		}
		// One frequency per grain (crossings less than 10 ms apart), then the mean over the grains
		double sumHz = 0;
		int groups = 0;
		for (size_t a = 0; a < xs.size();) {
			size_t b = a;
			while (b + 1 < xs.size() && xs[b + 1] - xs[b] < 0.01 * SR)
				b++;
			// Skip the few crossings at each end, where the window is nearly closed and the DC blocker shows
			if (b - a >= 8) {
				sumHz += (double) (b - a - 4) * SR / (xs[b - 2] - xs[a + 2]);
				groups++;
			}
			a = b + 1;
		}
		double bestHz = groups ? sumHz / groups : 0.0;
		CHECK(groups >= 8, "grains measured: %d", groups);
		double cents = 1200 * std::log2(bestHz / want);
		std::printf("  %+.0f V: %d grains, measured %.2f Hz, expected %.2f Hz: %+.2f cents (limit 2)\n", volts, groups, bestHz, want, cents);
		CHECK(std::fabs(cents) < 2.0, "pitch error %.2f cents", cents);
	}
	// 15 kHz source, +1 octave: would alias to 14.1 kHz without the decimated copies
	{
		Rig rig(makeBank(sineSource(4.0, 15000.0)));
		rig.in.voct = 1.f;
		wa::GrainMarkerInput& m = rig.in.markers[0];
		m.density = 0.9f;
		m.size = 0.55f;
		settle(rig, 0.5);
		std::vector<float> l;
		rig.run((long) (1 * SR), &l);
		double alias = tone(l, 0, l.size(), 48000.0 - 30000.0);
		Rig ref(makeBank(sineSource(4.0, 3000.0)));
		ref.in.markers[0].density = 0.9f;
		ref.in.markers[0].size = 0.55f;
		settle(ref, 0.5);
		std::vector<float> rl;
		ref.run((long) (1 * SR), &rl);
		double reference = tone(rl, 0, rl.size(), 3000.0);
		double db = 20 * std::log10((alias + 1e-12) / reference);
		std::printf("  15 kHz source up an octave: alias at 18 kHz = %.1f dB re a 3 kHz tone (limit -50)\n", db);
		CHECK(db < -50.0, "aliasing %.1f dB", db);
	}
}

static void testClicks() {
	std::printf("no clicks\n");
	// Sparse grains of a sine: the steepest legitimate step is the sine's slope plus the window's.
	Rig rig(makeBank(sineSource(6.0, 300.0)));
	wa::GrainMarkerInput& m = rig.in.markers[0];
	m.density = 0.25f; // ~2.7 grains/s
	m.size = 0.4f;     // ~40 ms
	m.window = wa::GRAIN_HANN;
	settle(rig, 0.3);
	std::vector<float> l;
	rig.run((long) (6 * SR), &l);
	double maxStep = 0, peak = 0;
	for (size_t i = 1; i < l.size(); i++) {
		maxStep = std::max(maxStep, (double) std::fabs(l[i] - l[i - 1]));
		peak = std::max(peak, (double) std::fabs(l[i]));
	}
	double amp = peak;
	double bound = amp * (2 * PI * 300.0 * SRC_SR / SR / SR + 1.2 * PI / (0.02 * SR)); // sine slope + window slope
	std::printf("  sparse grains: max step %.5f, bound %.5f, peak %.3f\n", maxStep, bound, peak);
	CHECK(maxStep <= bound * 1.6, "click: step %.5f above %.5f", maxStep, bound);
	// Every window shape: the grain's first and last samples are silent and smooth
	for (int w = 0; w < wa::NUM_GRAIN_WINDOWS; w++)
		CHECK(wa::grainWindowValue(w, 0.f) < 0.02f && wa::grainWindowValue(w, 1.f) < 0.02f, "window %d ends at zero", w);
}

/** Sharp decaying hits over a quiet tone: what a percussive sample looks like */
static std::vector<float> hitsSource(double seconds) {
	std::vector<float> v((size_t) (seconds * SRC_SR));
	size_t period = (size_t) (0.11 * SRC_SR);
	uint32_t s = 777u;
	for (size_t i = 0; i < v.size(); i++) {
		s = s * 1664525u + 1013904223u;
		double noise = (s >> 8) * (1.0 / 8388608.0) - 1.0;
		v[i] = (float) (0.8 * noise * std::exp(-(double) (i % period) / (0.004 * SRC_SR)) + 0.05 * std::sin(2 * PI * 200.0 * i / SRC_SR));
	}
	return v;
}

static float densityKnob(float hz) {
	return std::log(hz / 0.5f) / 6.907755279f;
}
static float sizeKnob(float seconds) {
	return std::log(seconds / 0.002f) / 6.214608098f;
}

struct Render {
	double holes = 0.0, wobble = 0.0, rmsVolts = 0.0, peak = 0.0, maxStep = 0.0;
};

/** 378 ms grains, 2 grains/s, +-6 ms spray unless changed */
template <typename Setup>
static Render renderCloud(const std::vector<float>& src, int window, Setup setup, unsigned channels = 1) {
	Rig rig(makeBank(src, channels));
	wa::GrainMarkerInput& m = rig.in.markers[0];
	m.density = densityKnob(2.f);
	m.size = sizeKnob(0.378f);
	m.spray = std::sqrt(0.006f / 2.f);
	m.start = 0.4f;
	m.window = window;
	m.sync = false;
	setup(m);
	settle(rig, 2.5);
	std::vector<float> l;
	rig.run((long) (6 * SR), &l);
	Render r;
	double all = 0.0;
	for (size_t i = 1; i < l.size(); i++) {
		r.peak = std::max(r.peak, (double) std::fabs(l[i]));
		r.maxStep = std::max(r.maxStep, (double) std::fabs(l[i] - l[i - 1]));
		all += (double) l[i] * l[i];
	}
	r.rmsVolts = 5.0 * std::sqrt(all / l.size());
	const size_t frame = (size_t) (0.05 * SR);
	std::vector<double> db;
	for (size_t i = 0; i + frame <= l.size(); i += frame)
		db.push_back(20.0 * std::log10(rms(l, i, i + frame) + 1e-7));
	double lo = 1e9, hi = -1e9, mean = 0.0;
	for (double d : db) {
		lo = std::min(lo, d);
		hi = std::max(hi, d);
		mean += d;
	}
	mean /= db.size();
	for (double d : db)
		r.wobble += (d - mean) * (d - mean);
	r.wobble = std::sqrt(r.wobble / db.size());
	r.holes = lo - hi;
	return r;
}

static void testSampleEnds() {
	std::printf("grains stay inside the sample\n");
	// A pure low sine that ends on a peak (a whole number of cycles would end at a zero crossing, where a
	// cut makes no step): any cut shows as a step far above its slope.
	std::vector<float> src = sineSource(4.0, 300.0625);
	auto nop = [](wa::GrainMarkerInput&) {};
	Render ref = renderCloud(src, wa::GRAIN_HANN, nop);
	struct Case {
		const char* name;
		float start;
		int direction;
	};
	const Case cases[] = {
		{"forward, START 0.95", 0.95f, wa::GRAIN_FORWARD},
		{"forward, START 1.00", 1.00f, wa::GRAIN_FORWARD},
		{"reverse, START 0.95", 0.95f, wa::GRAIN_REVERSE},
		{"reverse, START 0.00", 0.00f, wa::GRAIN_REVERSE},
	};
	for (const Case& c : cases) {
		Render r = renderCloud(src, wa::GRAIN_HANN, [&](wa::GrainMarkerInput& m) {
			m.start = c.start;
			m.direction = c.direction;
		});
		std::printf("  %s: max step %.5f (mid-sample %.5f), level %.2f V (mid-sample %.2f)\n", c.name, r.maxStep, ref.maxStep, r.rmsVolts, ref.rmsVolts);
		CHECK(r.maxStep <= ref.maxStep * 1.3, "%s: step %.5f against %.5f", c.name, r.maxStep, ref.maxStep);
		CHECK(std::fabs(20.0 * std::log10(std::max(r.rmsVolts, 1e-6) / ref.rmsVolts)) < 1.0, "%s: level %.2f against %.2f", c.name, r.rmsVolts, ref.rmsVolts);
	}
	// A pitch rise after the grain started can still reach the end: it must fade, not cut.
	Rig rig(makeBank(sineSource(1.0, 300.25)));
	wa::GrainMarkerInput& m = rig.in.markers[0];
	m.density = densityKnob(4.f);
	m.size = sizeKnob(0.25f);
	m.start = 0.7f;
	m.window = wa::GRAIN_TUKEY;
	settle(rig, 0.1);
	m.pitch = 24.f;
	std::vector<float> l;
	rig.run((long) (1.5 * SR), &l);
	double maxStep = 0.0, peak = 0.0;
	for (size_t i = 1; i < l.size(); i++) {
		maxStep = std::max(maxStep, (double) std::fabs(l[i] - l[i - 1]));
		peak = std::max(peak, (double) std::fabs(l[i]));
	}
	double bound = peak * 2.0 * PI * (4.0 * 300.0) * SRC_SR / SR / SR + peak * 1.5 / 32.0;
	std::printf("  pitched up into the end: max step %.4f (limit %.4f)\n", maxStep, bound);
	CHECK(maxStep <= bound, "pitch rise cuts at the end: %.4f above %.4f", maxStep, bound);
}

/** The map draws and grabs bullets at this position, so it must follow START for every cloud, heard or not. */
static void testIdleCloudPosition() {
	std::printf("clouds not being heard still report START\n");
	Rig rig(makeBank(noiseSource(4.0)));
	rig.in.position = 0.f;
	rig.in.markers[0].start = 0.1f;
	rig.in.markers[1].active = true;
	rig.in.markers[1].pos = 1.f;
	rig.in.markers[1].start = 0.6f;
	settle(rig, 0.2);
	double first = rig.engine->getReadPosition(1);
	rig.in.markers[1].start = 0.3f;
	rig.run(64);
	double moved = rig.engine->getReadPosition(1);
	rig.in.holdSilent = true;
	settle(rig, 0.1);
	rig.in.markers[1].start = 0.8f;
	rig.run(64);
	double silent = rig.engine->getReadPosition(1);
	std::printf("  playhead on another marker: reads %.3f (START 0.6), then %.3f (START 0.3); silenced %.3f (START 0.8)\n", first, moved, silent);
	CHECK(std::fabs(first - 0.6) < 1e-3, "reads %.3f", first);
	CHECK(std::fabs(moved - 0.3) < 1e-3, "reads %.3f", moved);
	CHECK(std::fabs(silent - 0.8) < 1e-3, "silenced: reads %.3f", silent);
}

static void testSmear() {
	std::printf("smear window\n");
	auto nop = [](wa::GrainMarkerInput&) {};
	std::vector<float> sine = sineSource(6.0, 300.0), noise = noiseSource(6.0), hits = hitsSource(6.0);
	std::vector<float> chord((size_t) (6.0 * SRC_SR));
	for (size_t i = 0; i < chord.size(); i++) {
		double s = 0.0;
		for (double hz : {110.0, 164.81, 220.0})
			for (int h = 1; h <= 20; h++)
				s += std::sin(2 * PI * hz * h * i / SRC_SR) / h;
		chord[i] = (float) (0.12 * s);
	}
	// Continuous: grains at the settings that leave gaps with any other window. (SIZE sets a Smear grain: 378 ms here,
	// at 8 grains/s. The hits, a 110 ms pulse, keep some of their own rhythm in a grain that short: -4.7 dB, where the
	// 1.5 s grains Smear made at 2 grains/s before SIZE set the length gave -2.2.)
	struct Source {
		const char* name;
		const std::vector<float>* data;
		double holesLimit;
	};
	const Source sources[] = {{"sine", &sine, 1.0}, {"chord", &chord, 2.0}, {"noise", &noise, 3.0}, {"hits", &hits, 6.0}};
	double lo = 1e9, hi = 0.0;
	for (const Source& s : sources) {
		Render h = renderCloud(*s.data, wa::GRAIN_HANN, nop), r = renderCloud(*s.data, wa::GRAIN_SMEAR, nop);
		std::printf("  %-6s Hann holes %7.1f dB, Smear holes %5.1f dB (limit %.1f), wobble %.2f dB, level %.2f V\n", s.name, h.holes, r.holes, s.holesLimit, r.wobble, r.rmsVolts);
		CHECK(r.holes > -s.holesLimit, "%s: holes %.1f dB", s.name, r.holes);
		if (s.data != &hits) {
			lo = std::min(lo, r.rmsVolts);
			hi = std::max(hi, r.rmsVolts);
		}
		CHECK(r.peak <= 2.0, "%s: peak %.2f", s.name, r.peak);
	}
	// Matched grains add up as amplitude: the compensation keeps tonal and noisy sources together.
	double spreadDb = 20.0 * std::log10(hi / lo);
	std::printf("  level across sine, chord, noise: %.2f .. %.2f V = %.1f dB (limit 2)\n", lo, hi, spreadDb);
	CHECK(spreadDb < 2.0, "level differs %.1f dB by source", spreadDb);
	// Same loudness as a dense cloud of the same source.
	Render dense = renderCloud(noise, wa::GRAIN_HANN, [](wa::GrainMarkerInput& m) {
		m.density = densityKnob(10.f);
	});
	Render smearNoise = renderCloud(noise, wa::GRAIN_SMEAR, nop);
	double vsDense = 20.0 * std::log10(smearNoise.rmsVolts / dense.rmsVolts);
	std::printf("  noise against a dense Hann cloud: %+.2f dB (limit +-1.5)\n", vsDense);
	CHECK(std::fabs(vsDense) < 1.5, "%+.2f dB against a dense cloud", vsDense);
	// Attacks: the ticks repeated by every grain are spread out.
	Render hitsHann = renderCloud(hits, wa::GRAIN_HANN, nop), hitsSmear = renderCloud(hits, wa::GRAIN_SMEAR, nop);
	double crestHann = 20.0 * std::log10(hitsHann.peak / (hitsHann.rmsVolts / 5.0));
	double crestSmear = 20.0 * std::log10(hitsSmear.peak / (hitsSmear.rmsVolts / 5.0));
	std::printf("  hits: crest %.1f dB -> %.1f dB (at least 6 lower), max step %.3f -> %.3f\n", crestHann, crestSmear, hitsHann.maxStep, hitsSmear.maxStep);
	CHECK(crestSmear < crestHann - 6.0, "crest %.1f -> %.1f dB", crestHann, crestSmear);
	CHECK(hitsSmear.maxStep < hitsHann.maxStep * 0.6, "max step %.3f -> %.3f", hitsHann.maxStep, hitsSmear.maxStep);
	// The cases where a grain cannot be matched must not break: big spray, pitch scatter, reverse, stereo.
	struct Var {
		const char* name;
		float spray, scatter;
		int direction;
		double holesLimit;
	};
	const Var vars[] = {
		{"spray +-200 ms", 0.316f, 0.f, wa::GRAIN_FORWARD, 3.0},
		{"pitch scatter +-1 st", 0.055f, 0.083f, wa::GRAIN_FORWARD, 12.0},
		{"reverse", 0.055f, 0.f, wa::GRAIN_REVERSE, 3.0},
		{"random direction", 0.055f, 0.f, wa::GRAIN_RANDOM_DIR, 12.0},
	};
	for (const Var& v : vars) {
		Render r = renderCloud(chord, wa::GRAIN_SMEAR, [&](wa::GrainMarkerInput& m) {
			m.spray = v.spray;
			m.pitchScatter = v.scatter;
			m.direction = v.direction;
		});
		std::printf("  chord, %-20s holes %5.1f dB (limit %.1f), level %.2f V, peak %.2f\n", v.name, r.holes, v.holesLimit, r.rmsVolts, r.peak);
		CHECK(r.holes > -v.holesLimit, "%s: holes %.1f dB", v.name, r.holes);
		CHECK(r.peak <= 2.0, "%s: peak %.2f", v.name, r.peak);
	}
	std::vector<float> st(sine.size() * 2);
	for (size_t i = 0; i < sine.size(); i++) {
		st[2 * i] = sine[i];
		st[2 * i + 1] = 0.5f * sine[i];
	}
	Render stereo = renderCloud(st, wa::GRAIN_SMEAR, nop, 2);
	std::printf("  stereo source: holes %.1f dB (limit 1.0)\n", stereo.holes);
	CHECK(stereo.holes > -1.0, "stereo: holes %.1f dB", stereo.holes);
	// Leaving Smear must not click or leave the diffuser running.
	{
		Rig rig(makeBank(sine));
		wa::GrainMarkerInput& m = rig.in.markers[0];
		m.density = densityKnob(4.f);
		m.size = sizeKnob(0.5f);
		m.start = 0.4f;
		m.window = wa::GRAIN_SMEAR;
		settle(rig, 1.5);
		std::vector<float> l;
		rig.run((long) (0.5 * SR), &l);
		m.window = wa::GRAIN_HANN;
		rig.run((long) (0.5 * SR), &l);
		double maxStep = 0.0, peak = 0.0;
		for (size_t i = 1; i < l.size(); i++) {
			maxStep = std::max(maxStep, (double) std::fabs(l[i] - l[i - 1]));
			peak = std::max(peak, (double) std::fabs(l[i]));
		}
		double bound = peak * 2.0 * PI * 300.0 * SRC_SR / SR / SR * 2.5;
		std::printf("  Smear -> Hann while playing: max step %.4f (limit %.4f)\n", maxStep, bound);
		CHECK(maxStep <= bound, "switching out of Smear: %.4f above %.4f", maxStep, bound);
	}
}

/** Noise bursts every 400 ms (20 ms long) over a quiet tone: where the sample is in time is easy to see. */
static std::vector<float> burstSource(double seconds) {
	std::vector<float> v((size_t) (seconds * SRC_SR));
	uint32_t s = 4242u;
	size_t period = (size_t) (0.4 * SRC_SR), burst = (size_t) (0.02 * SRC_SR);
	for (size_t i = 0; i < v.size(); i++) {
		s = s * 1664525u + 1013904223u;
		double noise = (s >> 8) * (1.0 / 8388608.0) - 1.0;
		v[i] = (float) ((i % period) < burst ? 0.8 * noise : 0.02 * std::sin(2 * PI * 300.0 * i / SRC_SR));
	}
	return v;
}

struct Timing {
	double lagMs = 0.0, match = 0.0, rmsVolts = 0.0;
	uint32_t grains = 0;
};

/** Plays a cloud scanning through the bursts at 1x, and finds how late the output's bursts are against the place
the cloud is reading (its read position, recorded every sample): 0 = the sample keeps its timing. */
template <typename Setup>
static Timing timingOf(Setup setup) {
	std::vector<float> src = burstSource(8.0);
	Rig rig(makeBank(src));
	wa::GrainMarkerInput& m = rig.in.markers[0];
	m.sync = false;
	m.start = 0.f;
	m.scan = 0.5f; // 1x real time
	m.window = wa::GRAIN_HANN;
	setup(m);
	settle(rig, 0.6);
	const int msN = 4000;
	std::vector<double> outEnv(msN, 0.0), srcEnv(msN, 0.0);
	const long perMs = (long) (SR / 1000);
	uint32_t g0 = rig.engine->eventCount();
	double all = 0.0;
	long n = 0;
	for (int ms = 0; ms < msN; ms++) {
		double e = 0.0;
		double read = rig.engine->getReadPosition(0);
		for (long i = 0; i < perMs; i++) {
			float l, r;
			rig.step(&l, &r);
			e += (double) l * l;
			all += (double) l * l;
			n++;
		}
		outEnv[ms] = std::sqrt(e / perMs);
		// Source envelope at the read position (1 ms around it)
		long at = (long) (read * src.size());
		double se = 0.0;
		long w = (long) (SRC_SR / 1000);
		for (long k = 0; k < w; k++) {
			long idx = std::min(std::max(at + k, 0L), (long) src.size() - 1);
			se += (double) src[idx] * src[idx];
		}
		srcEnv[ms] = std::sqrt(se / w);
	}
	Timing t;
	t.grains = rig.engine->eventCount() - g0;
	t.rmsVolts = 5.0 * std::sqrt(all / n);
	double best = -1e9;
	for (int lag = -150; lag <= 300; lag++) {
		double xy = 0.0, xx = 0.0, yy = 0.0;
		for (int i = 300; i < msN - 300; i++) {
			double a = outEnv[i + lag], b = srcEnv[i];
			xy += a * b;
			xx += a * a;
			yy += b * b;
		}
		double c = xy / std::sqrt(xx * yy + 1e-30);
		if (c > best) {
			best = c;
			t.lagMs = lag;
		}
	}
	t.match = best;
	return t;
}

static void testKeepSpeed() {
	std::printf("keep speed\n");
	CHECK(std::fabs(wa::GrainEngine::keepLengthSecondsForTest(0.4, 1.0) - 0.4) < 1e-9, "no change at the original pitch");
	std::printf("  grain length under KEEP: an octave up %.0f ms, an octave down %.0f ms, 3 octaves up %.0f ms (SIZE 400 ms)\n",
		wa::GrainEngine::keepLengthSecondsForTest(0.4, 2.0) * 1000.0, wa::GrainEngine::keepLengthSecondsForTest(0.4, 0.5) * 1000.0,
		wa::GrainEngine::keepLengthSecondsForTest(0.4, 8.0) * 1000.0);
	auto settings = [](float pitch, bool keep) {
		return [=](wa::GrainMarkerInput& m) {
			m.density = std::log(5.7f / 0.5f) / 6.907755279f; // 5.7 grains/s
			m.size = std::log(0.432f / 0.002f) / 6.214608098f; // 432 ms, as in the user's patch
			m.pitch = pitch;
			m.keepSpeed = keep;
		};
	};
	Timing base = timingOf(settings(0.f, false));
	Timing baseKeep = timingOf(settings(0.f, true));
	std::printf("  original pitch: bursts %+.0f ms late (match %.2f); with KEEP %+.0f ms, %u grains either way\n", base.lagMs, base.match, baseKeep.lagMs, baseKeep.grains);
	CHECK(std::fabs(base.lagMs) <= 3.0, "unpitched should be on time: %+.0f ms", base.lagMs);
	CHECK(baseKeep.lagMs == base.lagMs && baseKeep.grains == base.grains && baseKeep.rmsVolts == base.rmsVolts, "KEEP must change nothing at the original pitch");
	for (float pitch : {-12.f, -7.f, 12.f}) {
		Timing off = timingOf(settings(pitch, false));
		Timing on = timingOf(settings(pitch, true));
		double db = 20.0 * std::log10(on.rmsVolts / off.rmsVolts);
		std::printf("  %+3.0f st: tape %+4.0f ms late (match %.2f), KEEP %+4.0f ms (match %.2f); grains %u -> %u; level %+.1f dB\n", pitch, off.lagMs, off.match, on.lagMs, on.match, off.grains, on.grains, db);
		CHECK(std::fabs(on.lagMs) <= 12.0, "%+.0f st with KEEP: %+.0f ms late", pitch, on.lagMs);
		// On time is not enough: shortening keeps the bursts sharp (about 0.5 without it, 0.86 with).
		CHECK(on.match > 0.8, "%+.0f st with KEEP: bursts smeared (match %.2f)", pitch, on.match);
		CHECK(on.match > off.match, "%+.0f st: KEEP should follow the sample more closely (%.2f against %.2f)", pitch, on.match, off.match);
		CHECK(on.grains > off.grains * 2, "%+.0f st: more, shorter grains: %u against %u", pitch, on.grains, off.grains);
		CHECK(std::fabs(db) < 2.5, "%+.0f st: level %+.1f dB", pitch, db);
	}
	Timing down = timingOf(settings(-12.f, false));
	CHECK(down.lagMs > 50.0, "the test must see tape-style slowing: %+.0f ms", down.lagMs);

	// The pitch itself still changes.
	{
		Rig rig(makeBank(sineSource(6.0, 1000.0)));
		wa::GrainMarkerInput& m = rig.in.markers[0];
		m.density = 0.6f;
		m.size = 0.6f;
		m.pitch = -12.f;
		m.keepSpeed = true;
		settle(rig, 0.5);
		std::vector<float> l;
		rig.run((long) (1.0 * SR), &l);
		double half = tone(l, 0, l.size(), 500.0), orig = tone(l, 0, l.size(), 1000.0);
		std::printf("  1 kHz sample, -12 st with KEEP: 500 Hz %.3f, 1 kHz %.4f\n", half, orig);
		CHECK(half > 0.01 && orig * 20.0 < half, "pitch must still drop an octave");
	}
	// Synced: the extra grains are whole subdivisions of the grid.
	{
		Rig rig(makeBank(noiseSource(4.0)));
		wa::GrainMarkerInput& m = rig.in.markers[0];
		m.sync = true;
		m.density = 0.65f; // 1/8 notes: 8 grains in 2 s at 120 BPM
		m.size = 0.5f;
		m.pitch = -12.f;
		m.keepSpeed = false;
		settle(rig, 0.5);
		uint32_t a = rig.engine->eventCount();
		rig.run((long) (2.0 * SR));
		uint32_t tape = rig.engine->eventCount() - a;
		m.keepSpeed = true;
		settle(rig, 0.5);
		a = rig.engine->eventCount();
		rig.run((long) (2.0 * SR));
		uint32_t kept = rig.engine->eventCount() - a;
		std::printf("  synced, -12 st: %u grains in 2 s, with KEEP %u (a whole multiple: %s)\n", tape, kept, (tape > 0 && kept % tape <= 1) ? "yes" : "no");
		CHECK(tape > 0 && kept > tape && (kept % tape <= 1 || tape - kept % tape <= 1), "subdivision: %u, %u", tape, kept);
	}
}

static void testScanLoop() {
	std::printf("scan loop and fade\n");
	// Free scan at 2x through a 1 s slice (a second marker at 0.5 of a 2 s sample).
	auto path = [](bool ping, float fade, std::vector<float>* level) {
		Rig rig(makeBank(noiseSource(2.0)));
		rig.in.markers[0].start = 0.f;
		rig.in.markers[0].scan = 1.f;
		rig.in.markers[0].sync = false;
		rig.in.markers[0].density = 0.8f;
		rig.in.markers[0].size = 0.3f;
		rig.in.markers[0].scanPing = ping;
		rig.in.markers[0].scanFade = fade;
		rig.in.markers[1].active = true;
		rig.in.markers[1].pos = 1.f;
		rig.in.markers[1].start = 0.5f;
		rig.in.position = 0.f;
		std::vector<double> reads;
		long ms = (long) (SR / 1000);
		for (int i = 0; i < 3000; i++) {
			reads.push_back(rig.engine->getReadPosition(0));
			std::vector<float> l;
			rig.run(ms, &l);
			if (level)
				level->push_back((float) rms(l, 0, l.size()));
		}
		return reads;
	};
	auto biggestJump = [](const std::vector<double>& r) {
		double j = 0.0;
		for (size_t i = 1; i < r.size(); i++)
			j = std::max(j, std::fabs(r[i] - r[i - 1]));
		return j;
	};
	auto turns = [](const std::vector<double>& r) {
		int t = 0;
		for (size_t i = 2; i < r.size(); i++)
			if ((r[i] - r[i - 1]) * (r[i - 1] - r[i - 2]) < 0.0)
				t++;
		return t;
	};
	std::vector<double> loop = path(false, 0.f, nullptr), ping = path(true, 0.f, nullptr);
	double lo = 1.0, hi = 0.0;
	for (double v : ping) {
		lo = std::min(lo, v);
		hi = std::max(hi, v);
	}
	std::printf("  LOOP: biggest step of the read position per ms %.3f (a jump back), PING %.4f, turning %d times in 3 s, range %.3f..%.3f\n", biggestJump(loop), biggestJump(ping), turns(ping), lo, hi);
	CHECK(biggestJump(loop) > 0.2, "LOOP should jump back: %.3f", biggestJump(loop));
	CHECK(biggestJump(ping) < 0.002, "PING must not jump: %.4f", biggestJump(ping));
	CHECK(turns(ping) >= 4 && turns(ping) <= 8 && lo < 0.01 && hi > 0.49 && hi < 0.51, "PING goes there and back within the slice");

	// FADE: the level dips where the slice ends (LOOP jumps back there), not in the middle.
	std::vector<float> flat, faded;
	path(false, 0.f, &flat);
	std::vector<double> r = path(false, 0.15f, &faded);
	double ends = 0.0, mid = 0.0, endsFlat = 0.0, midFlat = 0.0;
	int nEnds = 0, nMid = 0;
	for (size_t i = 200; i < r.size(); i++) {
		double q = r[i] / 0.5;
		if (q < 0.03 || q > 0.97) {
			ends += faded[i];
			endsFlat += flat[i];
			nEnds++;
		}
		else if (q > 0.4 && q < 0.6) {
			mid += faded[i];
			midFlat += flat[i];
			nMid++;
		}
	}
	double dip = 20.0 * std::log10((ends / nEnds) / (mid / nMid)), dipFlat = 20.0 * std::log10((endsFlat / nEnds) / (midFlat / nMid));
	std::printf("  LOOP, at the slice ends against its middle: FADE 0 %+.1f dB, FADE 15%% %+.1f dB\n", dipFlat, dip);
	CHECK(std::fabs(dipFlat) < 2.0, "no fade: %+.1f dB", dipFlat);
	CHECK(dip < -8.0, "fade should dip the ends: %+.1f dB", dip);
	CHECK(std::fabs(20.0 * std::log10((mid / nMid) / (midFlat / nMid))) < 1.0, "the middle keeps its level");

	// Synced grains that all land on the loop's start (a loop no longer than the grain grid) must still sound.
	{
		auto levelWith = [](float fade) {
			Rig rig(makeBank(noiseSource(4.0)));
			wa::GrainMarkerInput& m = rig.in.markers[0];
			m.sync = true;
			m.density = 0.41f;   // as in the user's patch
			m.size = 0.33f;
			m.scan = 0.031f;     // just past the dead zone: the shortest synced loop, 1/16
			m.scanFade = fade;
			m.start = 0.45f;
			settle(rig, 1.0);
			std::vector<float> l;
			rig.run((long) (2.0 * SR), &l);
			return rms(l, 0, l.size());
		};
		double plain = levelWith(0.f), faded = levelWith(0.07f);
		std::printf("  synced, every grain on the loop's start: FADE 0 %.3f, FADE 7%% %.3f (%.1f dB)\n", plain, faded, 20.0 * std::log10(faded / plain));
		CHECK(faded > plain * 0.3, "FADE must not silence grains that start on the loop's start: %.4f against %.4f", faded, plain);
	}

	// Synced PING: one way per loop length, locked to the beat.
	CHECK(std::fabs(wa::GrainEngine::syncedScanPlaceForTest(0.0, 2.0, 0.5f, true)) < 1e-9, "starts at START");
	CHECK(std::fabs(wa::GrainEngine::syncedScanPlaceForTest(2.0, 2.0, 0.5f, true) - 1.0) < 1e-9, "at the end after one loop");
	CHECK(std::fabs(wa::GrainEngine::syncedScanPlaceForTest(3.0, 2.0, 0.5f, true) - 0.5) < 1e-9, "half way back");
	CHECK(std::fabs(wa::GrainEngine::syncedScanPlaceForTest(4.0, 2.0, 0.5f, true)) < 1e-9, "back at START after two");
	CHECK(std::fabs(wa::GrainEngine::syncedScanPlaceForTest(3.0, 2.0, 0.5f, false) - 0.5) < 1e-9, "LOOP as before");
	std::printf("  synced PING over 2 beats: at beat 0, 1, 2, 3, 4: %.2f %.2f %.2f %.2f %.2f\n", wa::GrainEngine::syncedScanPlaceForTest(0.0, 2.0, 0.5f, true),
		wa::GrainEngine::syncedScanPlaceForTest(1.0, 2.0, 0.5f, true), wa::GrainEngine::syncedScanPlaceForTest(2.0, 2.0, 0.5f, true),
		wa::GrainEngine::syncedScanPlaceForTest(3.0, 2.0, 0.5f, true), wa::GrainEngine::syncedScanPlaceForTest(4.0, 2.0, 0.5f, true));
}

/** The display's grain flash shows how much of the sample a grain reads, not how long it plays. */
static void testGrainEventSpan() {
	std::printf("grain flash length\n");
	for (float pitch : {0.f, -24.f, 12.f}) {
		Rig rig(makeBank(noiseSource(4.0)));
		wa::GrainMarkerInput& m = rig.in.markers[0];
		m.start = 0.2f;
		m.sync = false;
		m.density = 0.4f;
		m.size = std::log(0.3f / 0.002f) / 6.214608098f; // 300 ms
		m.pitch = pitch;
		settle(rig, 0.5);
		uint32_t n = rig.engine->eventCount();
		const wa::GrainEvent& ev = rig.engine->events()[(n - 1) % wa::GrainEngine::NUM_EVENTS];
		double want = 0.3 * std::exp2(pitch / 12.0);
		std::printf("  300 ms grains at %+3.0f st: flash %.3f s of the sample (it reads %.3f s)\n", pitch, ev.seconds, want);
		CHECK(n > 0 && std::fabs(ev.seconds - want) < 0.01 * want + 0.001, "%+.0f st: %.3f against %.3f", pitch, ev.seconds, want);
	}
}

static void testScanLength() {
	std::printf("scan loop length\n");
	CHECK(wa::grainScanLengthFraction(1.f, 2.0) < 0.f, "the top means to the next marker");
	CHECK(std::fabs(wa::grainScanLengthFraction(0.f, 2.0) - 0.005f) < 1e-6f, "the shortest is 10 ms");
	CHECK(std::fabs(wa::grainScanLengthFraction(0.994999f, 2.0) - 1.f) < 1e-3f, "just under the top: the whole sample");
	float back = wa::grainScanLengthFraction(wa::grainScanLengthKnob(0.3, 2.0), 2.0);
	std::printf("  knob ends: %.0f ms .. %.2f s; 0.3 of the sample through the knob and back: %.4f\n", wa::grainScanLengthFraction(0.f, 2.0) * 2000.0,
		wa::grainScanLengthFraction(0.994f, 2.0) * 2.0, back);
	CHECK(std::fabs(back - 0.3f) < 1e-4f, "round trip %.4f", back);

	// Free scan at 2x, START 0, a second marker at 0.5 of a 2 s sample.
	auto range = [](float start, float length, float* shownStart, float* shownEnd) {
		Rig rig(makeBank(noiseSource(2.0)));
		rig.in.markers[0].start = start;
		rig.in.markers[0].scan = 1.f;
		rig.in.markers[0].sync = false;
		rig.in.markers[0].density = 0.8f;
		rig.in.markers[0].size = 0.3f;
		rig.in.markers[0].scanLength = length;
		rig.in.markers[1].active = true;
		rig.in.markers[1].pos = 1.f;
		rig.in.markers[1].start = 0.5f;
		rig.in.position = 0.f;
		double lo = 1.0, hi = 0.0;
		for (int i = 0; i < 3000; i++) {
			double r = rig.engine->getReadPosition(0);
			if (i > 100) // after the engine has started
				lo = std::min(lo, r);
			hi = std::max(hi, r);
			rig.run((long) (SR / 1000));
		}
		rig.engine->getScanSlice(0, shownStart, shownEnd);
		return std::make_pair(lo, hi);
	};
	struct Case {
		const char* name;
		float start, length, lo, hi;
	};
	const Case cases[] = {
		{"default (to the next marker at 0.50)", 0.f, -1.f, 0.f, 0.5f},
		{"LENGTH 0.10 of the sample", 0.f, 0.1f, 0.f, 0.1f},
		{"LENGTH 0.80: past the next marker", 0.f, 0.8f, 0.f, 0.8f},
		{"START 0.70, LENGTH 0.50: cut at the end", 0.7f, 0.5f, 0.7f, 1.f},
	};
	for (const Case& c : cases) {
		float s, e;
		std::pair<double, double> r = range(c.start, c.length, &s, &e);
		std::printf("  %-42s reads %.3f .. %.3f; map shows %.3f .. %.3f\n", c.name, r.first, r.second, s, e);
		CHECK(std::fabs(r.first - c.lo) < 0.01 && std::fabs(r.second - c.hi) < 0.01, "%s: %.3f .. %.3f", c.name, r.first, r.second);
		CHECK(std::fabs(s - c.start) < 1e-4f && std::fabs(e - c.hi) < 1e-3f, "%s: shown %.3f .. %.3f", c.name, s, e);
	}
	// Not scanning: nothing to show.
	{
		Rig rig(makeBank(noiseSource(2.0)));
		rig.in.markers[0].scan = 0.f;
		rig.in.markers[0].scanLength = 0.2f;
		rig.run((long) (0.1 * SR));
		float s, e;
		rig.engine->getScanSlice(0, &s, &e);
		CHECK(e < 0.f, "no loop to show while SCAN is off: %.3f", e);
	}
}

static void testSafety() {
	std::printf("everything maxed stays within 10 V\n");
	Rig rig(makeBank(noiseSource(4.0)));
	for (int i = 0; i < wa::NUM_MARKERS; i++) {
		wa::GrainMarkerInput& m = rig.in.markers[i];
		m.active = true;
		m.pos = i / 4.f;
		m.start = i / 5.f;
		m.density = 1.f;
		m.size = 1.f;
		m.gain = 4.f;
		m.res = 1.f;
		m.spread = 1.f;
		m.pitchScatter = 1.f;
		m.spray = 1.f;
		m.scan = 1.f;
	}
	rig.in.position = 0.5f;
	rig.in.masterGain = 2.f;
	rig.in.masterMode = 0;
	rig.in.masterRes = 1.f;
	rig.in.masterFdbk = 1.f;
	rig.in.masterDrive = 1.f;
	std::vector<float> l;
	rig.run((long) (3 * SR), &l);
	double peak = 0;
	bool finite = true;
	for (float v : l) {
		peak = std::max(peak, (double) std::fabs(v));
		finite = finite && std::isfinite(v);
	}
	std::printf("  peak %.3f engine units (limit 2.0 = 10 V)\n", peak);
	CHECK(finite, "finite");
	CHECK(peak <= 2.0001, "peak %.3f", peak);
}

static void testCrossfadeAndSwap() {
	std::printf("position crossfade and bank swap\n");
	auto src = noiseSource(8.0);
	auto levelAt = [&](float position) {
		Rig rig(makeBank(src));
		for (int i = 0; i < 2; i++) {
			wa::GrainMarkerInput& m = rig.in.markers[i];
			m.active = true;
			m.pos = i;
			m.start = i ? 0.6f : 0.1f;
			m.density = 0.7f;
			m.size = 0.45f;
		}
		rig.in.position = position;
		settle(rig, 1.0);
		std::vector<float> l;
		rig.run((long) (4 * SR), &l);
		return rms(l, 0, l.size());
	};
	double a = levelAt(0.f), mid = levelAt(0.5f), b = levelAt(1.f);
	std::printf("  RMS at A %.4f, midway %.4f, at B %.4f: midway is %+.2f dB re the mean of the ends (limit +-2)\n",
		a, mid, b, 20 * std::log10(mid / (0.5 * (a + b))));
	CHECK(std::fabs(20 * std::log10(mid / (0.5 * (a + b)))) < 2.0, "equal-power crossfade keeps the level");

	// Bank swap: fades to silence and back, no NaN, no step above a generous bound
	Rig rig(makeBank(sineSource(4.0, 200.0)));
	rig.in.markers[0].density = 0.7f;
	rig.in.markers[0].size = 0.4f;
	settle(rig, 0.5);
	auto second = makeBank(sineSource(4.0, 400.0));
	rig.in.bank = second.get();
	std::vector<float> l;
	rig.run((long) (1 * SR), &l);
	double maxStep = 0;
	for (size_t i = 1; i < l.size(); i++)
		maxStep = std::max(maxStep, (double) std::fabs(l[i] - l[i - 1]));
	std::printf("  bank swap: max step %.4f (limit 0.12)\n", maxStep);
	CHECK(maxStep < 0.12, "swap step %.4f", maxStep);
	CHECK(rms(l, l.size() - 2000, l.size()) > 0.01, "sound returns after the swap");
	rig.in.bank = nullptr;
	std::vector<float> s;
	rig.run((long) (0.5 * SR), &s);
	CHECK(rms(s, s.size() - 1000, s.size()) < 1e-4, "silent without a bank");
}

static void testDirectOutput() {
	std::printf("cloud direct output\n");
	Rig rig(makeBank(noiseSource(4.0)));
	rig.in.markers[0].density = 0.7f;
	rig.in.markers[0].size = 0.45f;
	rig.in.wantDirect[0] = true;
	settle(rig, 0.5);
	double mainRms = 0, directRms = 0;
	long n = (long) SR;
	for (long i = 0; i < n; i++) {
		float l, r, dl, dr;
		rig.step(&l, &r);
		rig.engine->getDirect(0, &dl, &dr);
		mainRms += l * l;
		directRms += dl * dl;
	}
	mainRms = std::sqrt(mainRms / n);
	directRms = std::sqrt(directRms / n);
	std::printf("  patched: main %.5f, direct %.4f\n", mainRms, directRms);
	CHECK(mainRms < 0.01 * directRms && directRms > 0.01, "a patched cloud leaves the main mix");
}

static void testCpu() {
	std::printf("CPU\n");
	Rig rig(makeBank(noiseSource(8.0)));
	for (int i = 0; i < 2; i++) {
		wa::GrainMarkerInput& m = rig.in.markers[i];
		m.active = true;
		m.pos = i;
		m.start = i * 0.5f;
		m.density = 1.f;  // 500 grains / s
		m.size = 0.5f;    // long: the per-cloud cap is reached
		m.spread = 0.5f;
		m.pitchScatter = 0.3f;
	}
	rig.in.position = 0.5f;
	settle(rig, 2.0);
	long n = (long) (4 * SR);
	auto t0 = std::chrono::steady_clock::now();
	std::vector<float> l;
	for (long i = 0; i < n; i++) {
		float a, b;
		rig.step(&a, &b);
		l.push_back(a);
	}
	double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
	std::printf("  two clouds at the grain cap (%d grains): %.2f us per sample (budget at 48 kHz: 20.8 us; limit 4)\n",
		rig.engine->getActiveGrains(), us);
	CHECK(us < 4.0, "%.2f us per sample", us);
}

/** POLY note `seconds` long from this sample on (env 1, then 0). `direct`: record the cloud's own output (before the
master section, whose DRIVE path and DC blocker carry the last note's tail for a while) instead of the main one. */
static void playNote(Rig& rig, double seconds, std::vector<float>* out, bool direct = false) {
	rig.in.voiceEnv[0] = 1.f;
	long n = (long) (seconds * SR);
	for (long i = 0; i < n; i++) {
		rig.in.voiceStart[0] = i == 0;
		float a, b;
		rig.step(&a, &b);
		if (direct)
			rig.engine->getDirect(0, &a, &b);
		if (out)
			out->push_back(a);
	}
	rig.in.voiceStart[0] = false;
	rig.in.voiceEnv[0] = 0.f;
}

static double peakOf(const std::vector<float>& v) {
	double p = 0.0;
	for (float x : v)
		p = std::max(p, (double) std::fabs(x));
	return p;
}

/** Largest difference between two renders, relative to their peak, over [from, to) */
static double relDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to) {
	double d = 0.0;
	to = std::min(to, std::min(a.size(), b.size()));
	for (size_t i = from; i < to; i++)
		d = std::max(d, (double) std::fabs(a[i] - b[i]));
	return d / std::max(peakOf(a), 1e-9);
}

static void restartSetup(Rig& rig, bool sync, bool restart) {
	wa::GrainMarkerInput& M = rig.in.markers[0];
	M.sync = sync;
	M.density = sync ? 0.5f : densityKnob(20.f);
	M.size = sync ? 0.5f : sizeKnob(0.15f);
	M.spray = 0.3f;
	M.spread = 0.6f;
	M.pitchScatter = 0.2f;
	M.scan = 0.5f;
	M.start = 0.2f;
	rig.in.seeded = true;
	rig.in.poly = true;
	rig.in.noteRestart = restart;
}

static void testNoteRestart() {
	std::printf("note restart (same start every trig)\n");
	// Two equal notes landing at different places on the beat grid (beat 0.26 and 4.74), 1.6 s of silence between.
	for (int sync = 0; sync < 2; sync++) {
		double diff[2];
		for (int restart = 0; restart < 2; restart++) {
			Rig rig(makeBank(noiseSource(4.0)));
			restartSetup(rig, sync, restart);
			std::vector<float> a, b;
			rig.run((long) (0.13 * SR));
			playNote(rig, 0.6, &a);
			rig.run((long) (1.64 * SR));
			playNote(rig, 0.6, &b);
			diff[restart] = relDiff(a, b, 0, a.size());
		}
		std::printf("  %s: largest difference between the two notes %.2e with RESTART (limit 1e-4), %.2f without (above 0.1)\n",
			sync ? "synced, scan + scatter" : "free, scan + scatter ", diff[1], diff[0]);
		CHECK(diff[1] < 1e-4, "restart: notes differ by %.2e", diff[1]);
		CHECK(diff[0] > 0.1, "the check cannot fail: without restart the notes differ only by %.2e", diff[0]);
	}
	// A note over the last one's release: once the old grains have faded (5 ms), it is the same as a note from silence.
	for (int sync = 0; sync < 2; sync++) {
		std::vector<float> fresh, again;
		{
			Rig rig(makeBank(noiseSource(4.0)));
			restartSetup(rig, sync, true);
			rig.in.wantDirect[0] = true;
			rig.in.markers[0].gain = 0.2f; // under the safety limiters: they would react to the overlap
			rig.run((long) (1.13 * SR));
			playNote(rig, 0.6, &fresh, true);
		}
		int left = 0;
		{
			Rig rig(makeBank(noiseSource(4.0)));
			restartSetup(rig, sync, true);
			rig.in.wantDirect[0] = true;
			rig.in.markers[0].gain = 0.2f;
			rig.run((long) (0.4 * SR));
			playNote(rig, 0.35, nullptr);
			rig.in.voiceEnv[0] = 1.f; // still sounding
			left = rig.engine->getActiveGrains(0);
			playNote(rig, 0.6, &again, true);
		}
		// After the fade and the limiter's look-ahead (1.5 ms). The output's 4 Hz DC blocker still carries a slow,
		// inaudible tail of the last note (about 40 ms), so the notes are compared sample to sample (first
		// difference), which that tail does not reach.
		size_t from = (size_t) (0.007 * SR);
		double d = 0.0, ref = 0.0;
		for (size_t i = from; i < fresh.size(); i++) {
			double df = fresh[i] - fresh[i - 1], da = again[i] - again[i - 1];
			d = std::max(d, std::fabs(da - df));
			ref = std::max(ref, std::fabs(df));
		}
		d /= std::max(ref, 1e-9);
		double plain = relDiff(fresh, again, from, fresh.size());
		std::printf("  %s retrigger over a sounding note (%d grains): after 7 ms, sample-to-sample difference from a fresh note %.1e (limit 1e-3; level %.1e with the DC tail)\n",
			sync ? "synced" : "free  ", left, d, plain);
		CHECK(d < 1e-3, "retrigger differs from a fresh note by %.2e", d);
		// The old grains fade, not cut: no step bigger than the source allows in the first 6 ms.
		double jump = 0.0;
		for (size_t i = 1; i < from; i++)
			jump = std::max(jump, (double) std::fabs(again[i] - again[i - 1]));
		double typical = 0.0;
		for (size_t i = from + 1; i < again.size(); i++)
			typical = std::max(typical, (double) std::fabs(again[i] - again[i - 1]));
		std::printf("    largest sample step in the 5 ms fade %.3f, in the note after it %.3f (limit x1.5)\n", jump, typical);
		CHECK(jump < 1.5 * typical, "fade step %.3f against %.3f", jump, typical);
	}
}
/** Smear at the lowest DENSITY used to stretch each grain to 3 grain intervals (4 s, capped): a frozen cloud played
the sample back. SIZE now sets the grain, and the rate rises to keep 3 overlapping. */
static void testSmearSizeSetsGrain() {
	std::printf("Smear: SIZE sets the grain, not DENSITY\n");
	Rig rig(makeBank(noiseSource(15.0)));
	wa::GrainMarkerInput& m = rig.in.markers[0];
	m.window = wa::GRAIN_SMEAR;
	m.sync = false;
	m.start = 0.6f;
	m.density = 0.f; // 0.5 grains/s
	const float sizes[] = {0.f, sizeKnob(0.1f), sizeKnob(0.5f)};
	const double expect[] = {0.02, 0.1, 0.5}; // seconds (SIZE 0 is 2 ms: Smear's shortest grain is 20 ms)
	for (int i = 0; i < 3; i++) {
		m.size = sizes[i];
		settle(rig, 0.5);
		uint32_t before = rig.engine->eventCount();
		double longest = 0.0;
		long n = (long) SR;
		for (long k = 0; k < n; k++) {
			rig.run(1);
			uint32_t now = rig.engine->eventCount();
			for (uint32_t e = before; e < now; e++)
				longest = std::max(longest, (double) rig.engine->events()[e % wa::GrainEngine::NUM_EVENTS].seconds);
			before = now;
		}
		uint32_t perSecond = 0;
		{
			uint32_t a = rig.engine->eventCount();
			rig.run(n);
			perSecond = rig.engine->eventCount() - a;
		}
		double overlap = perSecond * expect[i];
		std::printf("  SIZE %.0f ms at DENSITY 0: grains of %.0f ms, %u a second (%.1f overlapping; limits +-10 %%, 2.5 .. 4)\n",
			expect[i] * 1000.0, longest * 1000.0, perSecond, overlap);
		CHECK(std::fabs(longest / expect[i] - 1.0) < 0.1, "SIZE %.0f ms: grains of %.0f ms", expect[i] * 1000.0, longest * 1000.0);
		CHECK(overlap > 2.5 && overlap < 4.0, "SIZE %.0f ms: %.1f grains overlap", expect[i] * 1000.0, overlap);
	}
}
int main() {
	testSmearSizeSetsGrain();
	testNoteRestart();
	testSampleBuffer();
	testLevelFlat();
	testSyncGrid();
	testScanMapping();
	testSeededRepeat();
	testPitchAndAlias();
	testClicks();
	testSampleEnds();
	testIdleCloudPosition();
	testSmear();
	testKeepSpeed();
	testScanLoop();
	testScanLength();
	testGrainEventSpan();
	testSafety();
	testCrossfadeAndSwap();
	testDirectOutput();
	testCpu();
	if (failures)
		std::printf("%d FAILED\n", failures);
	else
		std::printf("all checks passed\n");
	return failures ? 1 : 0;
}
