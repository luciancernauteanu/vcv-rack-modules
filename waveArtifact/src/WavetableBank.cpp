#include "WavetableBank.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define DR_WAV_IMPLEMENTATION
#include "dep/dr_wav.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// The analysis below is my own DSP design; the Rack manual's DSP page does not cover it.
// Per table:
// 1. Detect the fundamental with a normalized square difference function (McLeod pitch method).
// 2. Pitched: resample one period to FRAME_SIZE and average up to 4 consecutive periods.
//    Unpitched: take FRAME_SIZE raw samples and crossfade the wrap so the loop is continuous.
// 3. FFT, drop DC, drop harmonics the source could not contain, rotate so the fundamental has
//    zero phase (adjacent tables then blend without cancelling), and build band-limited mip levels.

namespace wa {

namespace {

typedef std::complex<double> cplx;

const double PI = 3.14159265358979323846;
const int WRAP_FADE = 256;
const int PITCH_WINDOW = 4096;
const int MAX_PERIODS = 4;
const double MIN_F0 = 40.0;
const double MAX_F0 = 2000.0;
const double CLARITY_THRESHOLD = 0.6;
const double PEAK_RATIO = 0.9;

void fft(std::vector<cplx>& a, bool inverse) {
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
		double ang = 2.0 * PI / len * (inverse ? 1.0 : -1.0);
		cplx wlen(std::cos(ang), std::sin(ang));
		for (size_t i = 0; i < n; i += len) {
			cplx w(1.0);
			for (size_t j = 0; j < len / 2; j++) {
				cplx u = a[i + j];
				cplx v = a[i + j + len / 2] * w;
				a[i + j] = u + v;
				a[i + j + len / 2] = u - v;
				w *= wlen;
			}
		}
	}
	if (inverse) {
		for (size_t i = 0; i < n; i++)
			a[i] /= (double) n;
	}
}

/** Catmull-Rom interpolation, clamped at the buffer edges. */
float sampleAt(const std::vector<float>& x, double pos) {
	long n = (long) x.size();
	long i = (long) std::floor(pos);
	double t = pos - i;
	auto at = [&](long k) {
		return x[std::min(std::max(k, 0L), n - 1)];
	};
	double y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
	double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
	double b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
	double c = -0.5 * y0 + 0.5 * y2;
	return (float) (((a * t + b) * t + c) * t + y1);
}

/** Returns the period in samples (fractional), or 0 if no clear fundamental. */
double detectPeriod(const float* x, int n, double sampleRate) {
	int minLag = std::max(2, (int) (sampleRate / MAX_F0));
	int maxLag = std::min(n / 2, (int) (sampleRate / MIN_F0));
	if (maxLag <= minLag + 2)
		return 0.0;

	std::vector<double> nsdf(maxLag + 2, 0.0);
	for (int tau = minLag - 1; tau <= maxLag + 1; tau++) {
		double acf = 0.0, m = 0.0;
		for (int i = 0; i + tau < n; i++) {
			acf += (double) x[i] * x[i + tau];
			m += (double) x[i] * x[i] + (double) x[i + tau] * x[i + tau];
		}
		nsdf[tau] = (m > 1e-12) ? 2.0 * acf / m : 0.0;
	}

	// Skip the lobe around lag 0, then take the maximum of each positive lobe.
	std::vector<int> peaks;
	int tau = minLag;
	while (tau < maxLag && nsdf[tau] > 0.0)
		tau++;
	while (tau < maxLag) {
		while (tau < maxLag && nsdf[tau] <= 0.0)
			tau++;
		if (tau >= maxLag)
			break;
		int best = tau;
		while (tau < maxLag && nsdf[tau] > 0.0) {
			if (nsdf[tau] > nsdf[best])
				best = tau;
			tau++;
		}
		peaks.push_back(best);
	}
	if (peaks.empty())
		return 0.0;

	double globalMax = 0.0;
	for (int p : peaks)
		globalMax = std::max(globalMax, nsdf[p]);
	if (globalMax < CLARITY_THRESHOLD)
		return 0.0;

	// The first lobe close to the global maximum avoids picking octave-down multiples.
	int chosen = peaks[0];
	for (int p : peaks) {
		if (nsdf[p] >= PEAK_RATIO * globalMax) {
			chosen = p;
			break;
		}
	}
	double a = nsdf[chosen - 1], b = nsdf[chosen], c = nsdf[chosen + 1];
	double denom = a - 2.0 * b + c;
	double shift = (denom != 0.0) ? 0.5 * (a - c) / denom : 0.0;
	return chosen + std::min(std::max(shift, -0.5), 0.5);
}

/** Fills `out` (FRAME_SIZE) with one cycle. Returns the highest harmonic the source can contain. */
int extractFrame(const std::vector<float>& src, double center, double sampleRate, std::vector<float>& out, bool* pitched) {
	long total = (long) src.size();

	int window = (int) std::min<long>(PITCH_WINDOW, total);
	long ws = std::min(std::max((long) center - window / 2, 0L), total - window);
	double period = detectPeriod(&src[ws], window, sampleRate);

	if (period > 0.0) {
		int periods = std::min(MAX_PERIODS, (int) ((total - 4) / period));
		if (periods >= 1) {
			double span = periods * period;
			double start = std::min(std::max(center - span / 2.0, 1.0), total - span - 3.0);
			for (int i = 0; i < FRAME_SIZE; i++) {
				double sum = 0.0;
				for (int k = 0; k < periods; k++)
					sum += sampleAt(src, start + k * period + i * period / FRAME_SIZE);
				out[i] = (float) (sum / periods);
			}
			*pitched = true;
			return std::max(1, std::min(FRAME_SIZE / 2 - 1, (int) (period / 2.0)));
		}
	}

	// Unpitched: raw window, tail crossfaded with the samples just before the start so the
	// last sample flows into the first.
	*pitched = false;
	long start = std::min(std::max((long) center - FRAME_SIZE / 2, (long) WRAP_FADE), total - FRAME_SIZE);
	for (int i = 0; i < FRAME_SIZE - WRAP_FADE; i++)
		out[i] = src[start + i];
	for (int j = 0; j < WRAP_FADE; j++) {
		float w = (float) (j + 1) / WRAP_FADE;
		float tail = src[start + FRAME_SIZE - WRAP_FADE + j];
		float pre = src[start - WRAP_FADE + j];
		out[FRAME_SIZE - WRAP_FADE + j] = tail * (1.f - w) + pre * w;
	}
	return FRAME_SIZE / 2 - 1;
}

/** Writes NUM_LEVELS band-limited frames for one table. */
void buildLevels(const std::vector<float>& raw, int maxHarmonic, WavetableBank& bank, int table, TableTone tone) {
	const int n = FRAME_SIZE;
	std::vector<cplx> spec(n);
	for (int i = 0; i < n; i++)
		spec[i] = cplx(raw[i], 0.0);
	fft(spec, false);

	spec[0] = 0.0;
	for (int k = maxHarmonic + 1; k <= n / 2; k++) {
		spec[k] = 0.0;
		spec[(n - k) % n] = 0.0;
	}

	// Tone: a 2nd-order low-pass shape on the harmonic number (-3 dB at `corner`, -12 dB per octave
	// above). Sampled instruments carry a lot of energy in the top harmonics that, played at low
	// pitches, reads as a harsh, buzzy edge; this takes the edge off without dulling the body.
	double corner = (tone == TONE_SMOOTH) ? 48.0 : (tone == TONE_SOFT) ? 20.0 : 0.0;
	if (corner > 0.0) {
		for (int k = 1; k <= maxHarmonic; k++) {
			double r = k / corner;
			double g = 1.0 / std::sqrt(1.0 + r * r * r * r);
			spec[k] *= g;
			spec[n - k] *= g;
		}
	}

	// Rotate in time so harmonic 1 has zero phase.
	if (std::abs(spec[1]) > 1e-9) {
		double phi = std::arg(spec[1]);
		for (int k = 1; k < n / 2; k++) {
			spec[k] *= std::polar(1.0, -k * phi);
			spec[n - k] = std::conj(spec[k]);
		}
	}

	double num = 0.0, den = 0.0;
	for (int k = 1; k <= maxHarmonic; k++) {
		double mag = std::abs(spec[k]);
		num += k * mag;
		den += mag;
	}
	float centroid = (den > 0.0) ? (float) (num / den) : 1.f;
	bank.brightness[table] = std::min(1.f, std::max(0.f, std::log2(std::max(centroid, 1.f)) / 9.f));

	std::vector<cplx> level(n);
	for (int l = 0; l < NUM_LEVELS; l++) {
		int harmonics = std::min(maxHarmonic, levelHarmonics(l));
		std::fill(level.begin(), level.end(), cplx(0.0));
		// A plain cut, no taper: the wobble at a band-limited edge is just the highest harmonic kept,
		// not an artifact you hear, and a taper only dulled the top end (measured in test_sound).
		// Softening is the job of the tone setting above, which is the same at every pitch.
		for (int k = 1; k <= harmonics && k < n / 2; k++) {
			level[k] = spec[k];
			level[n - k] = spec[n - k];
		}
		fft(level, true);
		float* out = bank.frame(table, l);
		for (int i = 0; i < n; i++)
			out[i] = (float) level[i].real();
	}

	// Loudness-normalize: scale level 0 to ARTIFACT_RMS unless its peak would pass ARTIFACT_PEAK_CAP
	// (spiky shapes stop at the cap). Peak normalization made a narrow pulse ~12 dB quieter than a
	// square, so moving between them swelled and dipped. Every level gets the same gain, so the
	// levels stay consistent with each other.
	float* base = bank.frame(table, 0);
	float peak = 0.f;
	double sumSq = 0.0;
	for (int i = 0; i < n; i++) {
		peak = std::max(peak, std::fabs(base[i]));
		sumSq += (double) base[i] * base[i];
	}
	float rms = (float) std::sqrt(sumSq / n);
	float gain = (peak > 1e-6f && rms > 1e-9f) ? std::min(ARTIFACT_RMS / rms, ARTIFACT_PEAK_CAP / peak) : 0.f;
	for (int l = 0; l < NUM_LEVELS; l++) {
		float* out = bank.frame(table, l);
		for (int i = 0; i < n; i++)
			out[i] *= gain;
	}
}

} // namespace

int tableCountFor(TableCountMode mode, double seconds) {
	switch (mode) {
		case TABLES_50: return 50;
		case TABLES_64: return 64;
		case TABLES_100: return 100;
		default: {
			int n = (int) std::lround(seconds * 20.0);
			return std::min(MAX_TABLES, std::max(MIN_TABLES, n));
		}
	}
}

WavetableBank* analyzeWav(const uint8_t* bytes, size_t size, TableCountMode mode, std::string* error, TableTone tone) {
	auto fail = [&](const char* msg) -> WavetableBank* {
		if (error)
			*error = msg;
		return nullptr;
	};
	if (!bytes || size == 0)
		return fail("Empty file");
	if (size > MAX_FILE_BYTES)
		return fail("File larger than 2 MB");

	unsigned channels = 0, sampleRate = 0;
	drwav_uint64 frames = 0;
	float* pcm = drwav_open_memory_and_read_pcm_frames_f32(bytes, size, &channels, &sampleRate, &frames, NULL);
	if (!pcm)
		return fail("Not a readable WAV file");
	if (channels == 0 || sampleRate == 0 || frames < (drwav_uint64) MIN_SOURCE_FRAMES) {
		drwav_free(pcm, NULL);
		return fail("Sample too short");
	}

	std::vector<float> mono((size_t) frames);
	for (size_t i = 0; i < mono.size(); i++) {
		float sum = 0.f;
		for (unsigned c = 0; c < channels; c++)
			sum += pcm[i * channels + c];
		mono[i] = sum / channels;
	}
	drwav_free(pcm, NULL);

	std::unique_ptr<WavetableBank> bank(new WavetableBank);
	bank->sourceSampleRate = sampleRate;
	bank->sourceSeconds = (double) frames / sampleRate;
	bank->numTables = tableCountFor(mode, bank->sourceSeconds);
	bank->data.assign((size_t) bank->numTables * NUM_LEVELS * FRAME_SIZE, 0.f);
	bank->brightness.assign(bank->numTables, 0.f);
	bank->pitched.assign(bank->numTables, 0);

	std::vector<float> raw(FRAME_SIZE);
	for (int t = 0; t < bank->numTables; t++) {
		double center = (t + 0.5) * frames / bank->numTables;
		bool pitched = false;
		int maxHarmonic = extractFrame(mono, center, sampleRate, raw, &pitched);
		bank->pitched[t] = pitched ? 1 : 0;
		buildLevels(raw, maxHarmonic, *bank, t, tone);
	}
	return bank.release();
}

} // namespace wa
