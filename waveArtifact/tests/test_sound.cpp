// Sound-quality measurements of the whole oscillator (analysis + engine), with an FFT.
// Build and run: cd tests && make run
//
// A bright 110 Hz saw sample is analyzed into artifacts, then the engine plays one artifact at
// every semitone of an octave. Each pitch is chosen so its harmonics fall exactly on FFT bins, so
// harmonic energy and everything else (aliasing, interpolation hash) can be separated cleanly.

#include "MorphEngine.hpp"
#include "WavetableBank.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
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

static const double PI = 3.14159265358979323846;
static const float SR = 48000.f;
static const int N = 65536;

static void fft(std::vector<std::complex<double>>& a) {
	size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; i++) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j)
			std::swap(a[i], a[j]);
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		double ang = -2.0 * PI / len;
		std::complex<double> wl(std::cos(ang), std::sin(ang));
		for (size_t i = 0; i < n; i += len) {
			std::complex<double> w(1.0);
			for (size_t j = 0; j < len / 2; j++) {
				std::complex<double> u = a[i + j], v = a[i + j + len / 2] * w;
				a[i + j] = u + v;
				a[i + j + len / 2] = u - v;
				w *= wl;
			}
		}
	}
}

static void put16(std::vector<uint8_t>& b, uint16_t v) {
	b.push_back(v & 0xff);
	b.push_back(v >> 8);
}
static void put32(std::vector<uint8_t>& b, uint32_t v) {
	for (int i = 0; i < 4; i++)
		b.push_back((v >> (8 * i)) & 0xff);
}
static std::vector<uint8_t> makeWav(const std::vector<float>& x, unsigned sr) {
	std::vector<uint8_t> b;
	b.insert(b.end(), {'R', 'I', 'F', 'F'});
	put32(b, 36 + (uint32_t) x.size() * 2);
	b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
	put32(b, 16);
	put16(b, 1);
	put16(b, 1);
	put32(b, sr);
	put32(b, sr * 2);
	put16(b, 2);
	put16(b, 16);
	b.insert(b.end(), {'d', 'a', 't', 'a'});
	put32(b, (uint32_t) x.size() * 2);
	for (float v : x)
		put16(b, (uint16_t) (int16_t) std::lround(std::max(-1.f, std::min(1.f, v)) * 32767.f));
	return b;
}

struct Measure {
	/** Highest frequency where the played saw is still within 6 dB of an ideal saw */
	double topHz;
	/** Energy outside the harmonics, relative to the harmonics */
	double aliasDb;
};

/** Plays artifact `wave` at `bin` cycles per N samples and measures the spectrum. */
/** `panelDefaults`: artifact filter and master filter on as on a fresh module (low-pass, 20 kHz). */
static Measure measure(const wa::WavetableBank* bank, float wave, int bin, bool panelDefaults = false) {
	wa::MorphEngine engine;
	engine.setSampleRate(SR);
	wa::EngineInput in;
	in.bank = bank;
	in.markers[0].active = true;
	in.markers[0].wave = wave;
	if (panelDefaults) {
		in.markers[0].cutoff = 1.f;
		in.markers[0].filterMode = wa::MODE_LP;
		in.masterMode = wa::MODE_LP;
		in.masterCutoff = 1.f;
	}
	double f = (double) bin * SR / N;
	in.voct = (float) std::log2(f / 261.6255653);
	float l, r;
	for (int i = 0; i < (int) (0.2f * SR); i++)
		engine.process(in, &l, &r);
	std::vector<std::complex<double>> x(N);
	for (int i = 0; i < N; i++) {
		engine.process(in, &l, &r);
		x[i] = l;
	}
	fft(x);
	double h1 = std::abs(x[bin]);
	double harm = 0, other = 0;
	double topHz = 0;
	for (int k = 1; k < N / 2; k++) {
		double p = std::norm(x[k]);
		if (k % bin == 0) {
			harm += p;
			int h = k / bin;
			// An ideal saw has harmonic h at 1/h of the fundamental.
			if (std::abs(x[k]) * h >= 0.5 * h1 && topHz < (double) k * SR / N)
				topHz = (double) k * SR / N;
		}
		else {
			other += p;
		}
	}
	Measure m;
	m.topHz = topHz;
	m.aliasDb = 10 * std::log10(other / harm + 1e-30);
	return m;
}

static std::unique_ptr<wa::WavetableBank> sawBank(wa::TableTone tone) {
	// A 110 Hz saw with every harmonic up to 20 kHz, 2 s at 44.1 kHz.
	std::vector<float> src;
	for (int i = 0; i < 88200; i++) {
		double t = i / 44100.0, s = 0;
		for (int k = 1; k * 110 < 20000; k++)
			s += std::sin(2 * PI * k * 110.0 * t) / k;
		src.push_back((float) (0.3 * s));
	}
	std::vector<uint8_t> wav = makeWav(src, 44100);
	std::string err;
	return std::unique_ptr<wa::WavetableBank>(wa::analyzeWav(wav.data(), wav.size(), wa::TABLES_50, &err, tone));
}

static void testBandLimit() {
	std::printf("band-limiting across an octave (A4..A5, 48 kHz, tone Natural): top end and aliasing\n");
	std::unique_ptr<wa::WavetableBank> bank = sawBank(wa::TONE_NATURAL);
	CHECK(bank != nullptr, "analysis failed");
	if (!bank)
		return;
	double minTop = 1e9, maxTop = 0, worstAlias = -1e9;
	for (int s = 0; s <= 12; s++) {
		double f = 440.0 * std::pow(2.0, s / 12.0);
		int bin = (int) std::lround(f * N / SR);
		Measure m = measure(bank.get(), 0.5f, bin);
		std::printf("  %7.1f Hz: within 6 dB up to %5.1f kHz, non-harmonic %6.1f dB\n", (double) bin * SR / N, m.topHz / 1000, m.aliasDb);
		minTop = std::min(minTop, m.topHz);
		maxTop = std::max(maxTop, m.topHz);
		worstAlias = std::max(worstAlias, m.aliasDb);
	}
	std::printf("  top end ranges %.1f..%.1f kHz, worst non-harmonic %.1f dB\n", minTop / 1000, maxTop / 1000, worstAlias);
	// Before two levels per octave (and with the old taper) this was 8.3..12.3 kHz.
	CHECK(minTop >= 14000.0, "dull: top end only %.1f kHz at some pitch", minTop / 1000);
	CHECK(maxTop - minTop <= 6000.0, "brightness jumps between semitones: %.1f..%.1f kHz", minTop / 1000, maxTop / 1000);
	CHECK(worstAlias <= -50.0, "aliasing: %.1f dB", worstAlias);
}

/** dB change at 5, 10 and 15 kHz (harmonics of the played note) between the bare oscillator and the
panel's default chain. */
static void chainLoss(const wa::WavetableBank* bank, int bin, double out[3]) {
	std::vector<std::complex<double>> spec[2];
	for (int d = 0; d < 2; d++) {
		wa::MorphEngine engine;
		engine.setSampleRate(SR);
		wa::EngineInput in;
		in.bank = bank;
		in.markers[0].active = true;
		in.markers[0].wave = 0.5f;
		if (d == 1) {
			in.markers[0].cutoff = 1.f;
			in.markers[0].filterMode = wa::MODE_LP;
			in.masterMode = wa::MODE_LP;
			in.masterCutoff = 1.f;
		}
		in.voct = (float) std::log2((double) bin * SR / N / 261.6255653);
		float l, r;
		for (int i = 0; i < (int) (0.2f * SR); i++)
			engine.process(in, &l, &r);
		spec[d].resize(N);
		for (int i = 0; i < N; i++) {
			engine.process(in, &l, &r);
			spec[d][i] = l;
		}
		fft(spec[d]);
	}
	const double freqs[3] = {5000.0, 10000.0, 15000.0};
	for (int j = 0; j < 3; j++) {
		int h = (int) std::lround(freqs[j] / ((double) bin * SR / N));
		int k = h * bin;
		out[j] = 20 * std::log10(std::abs(spec[1][k]) / std::abs(spec[0][k]));
	}
}

static void testDefaultChain() {
	std::printf("panel defaults (artifact LP 20 kHz + master LP 20 kHz) must not dull the oscillator\n");
	std::unique_ptr<wa::WavetableBank> bank = sawBank(wa::TONE_NATURAL);
	if (!bank)
		return;
	double worst = 0;
	for (double f : {110.0, 440.0}) {
		int bin = (int) std::lround(f * N / SR);
		double loss[3];
		chainLoss(bank.get(), bin, loss);
		std::printf("  %5.0f Hz note: change at 5 kHz %+.2f dB, 10 kHz %+.2f dB, 15 kHz %+.2f dB\n", f, loss[0], loss[1], loss[2]);
		for (double v : loss)
			worst = std::min(worst, v);
	}
	CHECK(worst > -0.5, "default filters dull the sound: %.2f dB", worst);
}

int main() {
	testBandLimit();
	testDefaultChain();
	if (failures) {
		std::printf("%d check(s) FAILED\n", failures);
		return 1;
	}
	std::printf("all checks passed\n");
	return 0;
}
