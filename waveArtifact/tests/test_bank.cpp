// Offline tests for WavetableBank (no Rack needed).
// Build and run: cd tests && make run

#include "WavetableBank.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

static void put16(std::vector<uint8_t>& b, uint16_t v) {
	b.push_back(v & 0xff);
	b.push_back(v >> 8);
}
static void put32(std::vector<uint8_t>& b, uint32_t v) {
	for (int i = 0; i < 4; i++)
		b.push_back((v >> (8 * i)) & 0xff);
}

/** 16-bit PCM WAV from interleaved samples in [-1, 1]. */
static std::vector<uint8_t> makeWav(const std::vector<float>& samples, unsigned channels, unsigned sampleRate) {
	std::vector<uint8_t> b;
	uint32_t dataBytes = (uint32_t) samples.size() * 2;
	b.insert(b.end(), {'R', 'I', 'F', 'F'});
	put32(b, 36 + dataBytes);
	b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
	put32(b, 16);
	put16(b, 1);
	put16(b, channels);
	put32(b, sampleRate);
	put32(b, sampleRate * channels * 2);
	put16(b, channels * 2);
	put16(b, 16);
	b.insert(b.end(), {'d', 'a', 't', 'a'});
	put32(b, dataBytes);
	for (float s : samples) {
		int v = (int) std::lround(std::max(-1.f, std::min(1.f, s)) * 32767.f);
		put16(b, (uint16_t) (int16_t) v);
	}
	return b;
}

/** Sine at 220 Hz morphing into a band-limited saw over `seconds`. */
static std::vector<float> sineToSaw(double seconds, unsigned sr) {
	size_t n = (size_t) (seconds * sr);
	std::vector<float> x(n);
	double f = 220.0;
	for (size_t i = 0; i < n; i++) {
		double t = (double) i / sr;
		double mix = (double) i / n;
		double saw = 0.0;
		for (int k = 1; k * f < sr / 2.0 && k <= 40; k++)
			saw += std::sin(2 * PI * k * f * t) / k;
		saw *= 2.0 / PI;
		x[i] = (float) (0.5 * ((1.0 - mix) * std::sin(2 * PI * f * t) + mix * saw));
	}
	return x;
}

static bool allFinite(const wa::WavetableBank& b) {
	for (float v : b.data)
		if (!std::isfinite(v))
			return false;
	return true;
}

static float maxAbsDiff(const float* a, const float* b, int n) {
	float m = 0.f;
	for (int i = 0; i < n; i++)
		m = std::max(m, std::fabs(a[i] - b[i]));
	return m;
}

static void testSineToSaw() {
	std::printf("sine -> saw, 3 s, 44.1 kHz, Auto\n");
	std::vector<uint8_t> wav = makeWav(sineToSaw(3.0, 44100), 1, 44100);
	std::string err;
	auto t0 = std::chrono::steady_clock::now();
	std::unique_ptr<wa::WavetableBank> bank(wa::analyzeWav(wav.data(), wav.size(), wa::TABLES_AUTO, &err));
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	CHECK(bank, "analysis failed: %s", err.c_str());
	if (!bank)
		return;
	std::printf("  %d tables in %lld ms\n", bank->numTables, (long long) ms);

	CHECK(bank->numTables == 60, "expected 60 tables (3 s * 20), got %d", bank->numTables);
	CHECK(allFinite(*bank), "non-finite sample");

	int pitched = 0;
	for (int t = 0; t < bank->numTables; t++) {
		pitched += bank->pitched[t];
		const float* f = bank->frame(t, 0);
		float peak = 0.f;
		for (int i = 0; i < wa::FRAME_SIZE; i++)
			peak = std::max(peak, std::fabs(f[i]));
		CHECK(std::fabs(peak - 1.f) < 1e-3f, "table %d peak %f, expected 1", t, peak);
		// Periodic: the wrap step must be about as small as an in-frame step.
		float wrapStep = std::fabs(f[0] - f[wa::FRAME_SIZE - 1]);
		CHECK(wrapStep < 0.05f, "table %d wrap discontinuity %f", t, wrapStep);
	}
	CHECK(pitched == bank->numTables, "only %d/%d tables pitched", pitched, bank->numTables);
	CHECK(bank->brightness.back() > bank->brightness.front() + 0.1f,
		"saw end should be brighter: first %f last %f", bank->brightness.front(), bank->brightness.back());

	// Phase alignment: neighbors in the near-sine region must be almost identical, so a morph
	// between them does not cancel.
	float d = maxAbsDiff(bank->frame(0, 0), bank->frame(1, 0), wa::FRAME_SIZE);
	CHECK(d < 0.1f, "adjacent tables differ by %f, phase alignment failed", d);
	// Midpoint of a 50/50 blend between first and last table must not collapse.
	const float* a = bank->frame(0, 0);
	const float* b = bank->frame(bank->numTables - 1, 0);
	float midPeak = 0.f;
	for (int i = 0; i < wa::FRAME_SIZE; i++)
		midPeak = std::max(midPeak, std::fabs(0.5f * (a[i] + b[i])));
	CHECK(midPeak > 0.6f, "50/50 morph peak %f, tables cancel", midPeak);

	// Top level must be (close to) a pure sine.
	const float* top = bank->frame(bank->numTables - 1, wa::NUM_LEVELS - 1);
	float topPeak = 0.f;
	for (int i = 0; i < wa::FRAME_SIZE; i++)
		topPeak = std::max(topPeak, std::fabs(top[i]));
	float sineErr = 0.f;
	for (int i = 0; i < wa::FRAME_SIZE; i++) {
		float s = topPeak * (float) std::cos(2 * PI * i / wa::FRAME_SIZE);
		sineErr = std::max(sineErr, std::fabs(top[i] - s));
	}
	CHECK(sineErr < 1e-3f, "top level is not a single cosine, err %f", sineErr);
}

static void testModes() {
	std::printf("table count modes\n");
	std::vector<uint8_t> wav = makeWav(sineToSaw(1.0, 48000), 1, 48000);
	const wa::TableCountMode modes[] = {wa::TABLES_AUTO, wa::TABLES_50, wa::TABLES_64, wa::TABLES_100};
	const int expected[] = {50, 50, 64, 100};
	for (int i = 0; i < 4; i++) {
		std::string err;
		std::unique_ptr<wa::WavetableBank> bank(wa::analyzeWav(wav.data(), wav.size(), modes[i], &err));
		CHECK(bank && bank->numTables == expected[i], "mode %d: expected %d tables", i, expected[i]);
	}
	CHECK(wa::tableCountFor(wa::TABLES_AUTO, 10.0) == 100, "Auto must cap at 100");
}

static void testStereoNoise() {
	std::printf("stereo noise, 1 s\n");
	std::vector<float> x(44100 * 2);
	unsigned seed = 1;
	for (float& v : x) {
		seed = seed * 1664525u + 1013904223u;
		v = ((seed >> 8) / 16777216.f) * 2.f - 1.f;
	}
	std::vector<uint8_t> wav = makeWav(x, 2, 44100);
	std::string err;
	std::unique_ptr<wa::WavetableBank> bank(wa::analyzeWav(wav.data(), wav.size(), wa::TABLES_AUTO, &err));
	CHECK(bank, "analysis failed: %s", err.c_str());
	if (!bank)
		return;
	CHECK(allFinite(*bank), "non-finite sample");
	int pitched = 0;
	for (int t = 0; t < bank->numTables; t++) {
		pitched += bank->pitched[t];
		// White noise has large steps everywhere; the wrap must not be worse than the frame itself.
		const float* f = bank->frame(t, 0);
		float maxStep = 0.f;
		for (int i = 1; i < wa::FRAME_SIZE; i++)
			maxStep = std::max(maxStep, std::fabs(f[i] - f[i - 1]));
		float wrapStep = std::fabs(f[0] - f[wa::FRAME_SIZE - 1]);
		CHECK(wrapStep <= maxStep, "noise table %d wrap step %f > max in-frame step %f", t, wrapStep, maxStep);
	}
	CHECK(pitched < bank->numTables / 4, "noise detected as pitched in %d tables", pitched);
}

static void testRejects() {
	std::printf("rejects\n");
	std::string err;
	std::vector<uint8_t> big(wa::MAX_FILE_BYTES + 1, 0);
	CHECK(!wa::analyzeWav(big.data(), big.size(), wa::TABLES_AUTO, &err), "accepted > 2 MB");
	CHECK(err == "File larger than 2 MB", "wrong error: %s", err.c_str());

	std::vector<uint8_t> junk(1000, 0x5a);
	CHECK(!wa::analyzeWav(junk.data(), junk.size(), wa::TABLES_AUTO, &err), "accepted junk");

	std::vector<uint8_t> tiny = makeWav(std::vector<float>(1000, 0.f), 1, 44100);
	CHECK(!wa::analyzeWav(tiny.data(), tiny.size(), wa::TABLES_AUTO, &err), "accepted too-short sample");
	CHECK(err == "Sample too short", "wrong error: %s", err.c_str());

	std::vector<uint8_t> silent = makeWav(std::vector<float>(44100, 0.f), 1, 44100);
	std::unique_ptr<wa::WavetableBank> bank(wa::analyzeWav(silent.data(), silent.size(), wa::TABLES_AUTO, &err));
	CHECK(bank && allFinite(*bank), "silent file must give a finite (silent) bank");
}

int main() {
	testSineToSaw();
	testModes();
	testStereoNoise();
	testRejects();
	if (failures) {
		std::printf("%d check(s) FAILED\n", failures);
		return 1;
	}
	std::printf("all checks passed\n");
	return 0;
}
