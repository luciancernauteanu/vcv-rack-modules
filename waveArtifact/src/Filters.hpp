#pragma once
#include <algorithm>
#include <cmath>

// Multimode filter building blocks. No Rack dependencies (tested offline in tests/).
// Own implementation; the control set (simultaneous LP/BP/HP/notch from one state-variable core,
// stereo SPREAD, band-pass FDBK) is inspired by Venom's Multimode Filter.

namespace wa {

enum FilterMode {
	MODE_LP,
	MODE_BP,
	MODE_HP,
	MODE_NOTCH,
	NUM_FILTER_MODES
};

/** Mode mix weights: dry, LP, BP, HP, notch. Crossfading these weights morphs between modes. */
static const int NUM_MIX = 5;

static const float FILTER_Q_MIN = 0.7f;
static const float FILTER_LN_Q_RANGE = 3.352407217f; // ln(20 / 0.7)
static const float FILTER_LN_1000 = 6.907755279f;

/** RES 0..1 -> Q 0.7 (wide) .. 20 (narrow, ringing). */
inline float filterQ(float res) {
	return FILTER_Q_MIN * std::exp(std::min(std::max(res, 0.f), 1.f) * FILTER_LN_Q_RANGE);
}

/** CUTOFF 0..1 -> 20 Hz .. 20 kHz, log. */
inline float filterHz(float cutoff) {
	return 20.f * std::exp(cutoff * FILTER_LN_1000);
}

/** One-hot mix weights for a mode; `on` false = dry. */
inline void modeWeights(bool on, int mode, float out[NUM_MIX]) {
	for (int k = 0; k < NUM_MIX; k++)
		out[k] = 0.f;
	out[on ? 1 + std::min(std::max(mode, 0), (int) NUM_FILTER_MODES - 1) : 0] = 1.f;
}

/** Trapezoidal state-variable filter (Cytomic/Simper form), stable under fast cutoff and Q
modulation. Gives all modes at once; `mix` blends them:
- LP / HP: resonant peak; the level is tilted down by (Q/0.7)^-0.3 so high RES rings without
  jumping 26 dB (the passband drops a little as RES rises, as in many analog designs).
- BP: unity at the center at the lowest RES, rising to sqrt(Q/0.7) at full RES.
- Notch: unity passband, a dip at the cutoff that narrows as RES rises.
*/
struct Svf {
	float ic1 = 0.f;
	float ic2 = 0.f;
	/** Last normalized band-pass output (unity at center), for feedback. */
	float lastBp = 0.f;

	void reset() {
		ic1 = ic2 = lastBp = 0.f;
	}

	float process(float x, float fc, float q, float sampleRate, const float mix[NUM_MIX]) {
		fc = std::min(std::max(fc, 5.f), 0.45f * sampleRate);
		float k = 1.f / q;
		float g = std::tan(3.141592654f * fc / sampleRate);
		float a1 = 1.f / (1.f + g * (g + k));
		float a2 = g * a1;
		float a3 = g * a2;
		float v3 = x - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		float lp = v2;
		float bp = k * v1; // unity gain at the center
		float hp = x - bp - lp;
		lastBp = bp;
		float tilt = std::pow(q / FILTER_Q_MIN, -0.3f);
		float bpGain = std::sqrt(q / FILTER_Q_MIN);
		return mix[0] * x + (mix[1] * lp + mix[3] * hp) * tilt + mix[2] * bp * bpGain + mix[4] * (x - bp);
	}
};

/** 63-tap halfband low-pass (Blackman-windowed sinc) at the 2x rate, for 2x oversampling. */
struct Halfband {
	static const int TAPS = 63;
	float h[TAPS];
	Halfband() {
		const int c = TAPS / 2;
		float sum = 0.f;
		for (int i = 0; i < TAPS; i++) {
			int t = i - c;
			double x = 0.5 * t;
			double sinc = (t == 0) ? 1.0 : std::sin(3.14159265358979 * x) / (3.14159265358979 * x);
			double n = (double) i / (TAPS - 1);
			double win = 0.42 - 0.5 * std::cos(2 * 3.14159265358979 * n) + 0.08 * std::cos(4 * 3.14159265358979 * n);
			h[i] = (float) (sinc * win);
			sum += h[i];
		}
		for (int i = 0; i < TAPS; i++)
			h[i] /= sum;
	}
};

/** Soft-clip drive: tanh with first-order antiderivative anti-aliasing (ADAA), run at 2x
oversampling. Heavy drive adds harmonics without fold-back hash.
DRIVE 0..1: input gain 1..25 (0..28 dB). Output is scaled so a full-scale input stays near full
scale. The first quarter of the knob fades the drive in, so 0 is clean. The path is always
oversampled (constant 31-sample latency), so turning DRIVE never jumps in time. */
struct Drive {
	static const int HIST = Halfband::TAPS / 2 + 1; // base-rate history for the polyphase upsampler
	const Halfband& fir() {
		static const Halfband hb;
		return hb;
	}
	float in[HIST] = {};
	float up[Halfband::TAPS] = {};
	float u1 = 0.f;

	static float logCosh(float u) {
		float a = std::fabs(u);
		return a + std::log1p(std::exp(-2.f * a)) - 0.693147181f;
	}

	void reset() {
		std::fill(in, in + HIST, 0.f);
		std::fill(up, up + Halfband::TAPS, 0.f);
		u1 = 0.f;
	}

	float shape(float x, float g, float norm, float blend) {
		float u0 = u1, u = g * x;
		u1 = u;
		if (blend <= 0.f)
			return x;
		float sat = (std::fabs(u - u0) > 1e-4f) ? (logCosh(u) - logCosh(u0)) / (u - u0) : std::tanh(0.5f * (u0 + u));
		sat *= norm;
		return x + (sat - x) * blend;
	}

	float process(float x, float drive) {
		const Halfband& hb = fir();
		drive = std::min(std::max(drive, 0.f), 1.f);
		float g = std::exp(drive * 3.218875825f); // ln(25)
		float norm = 1.f / std::tanh(g);
		float blend = std::min(1.f, 4.f * drive);

		// Upsample: zero-stuffing + halfband, as two polyphase branches (gain 2 restores the level).
		for (int i = HIST - 1; i > 0; i--)
			in[i] = in[i - 1];
		in[0] = x;
		float s[2];
		for (int p = 0; p < 2; p++) {
			float acc = 0.f;
			for (int k = 0; 2 * k + p < Halfband::TAPS; k++)
				acc += hb.h[2 * k + p] * in[k];
			s[p] = 2.f * acc;
		}
		// Shape at 2x, then decimate through the same halfband.
		for (int p = 0; p < 2; p++) {
			for (int i = Halfband::TAPS - 1; i > 0; i--)
				up[i] = up[i - 1];
			up[0] = shape(s[p], g, norm, blend);
		}
		float y = 0.f;
		for (int i = 0; i < Halfband::TAPS; i++)
			y += hb.h[i] * up[i];
		return y;
	}
};

/** Stereo master filter: mode (off/LP/BP/HP/notch, crossfaded), cutoff, resonance, SPREAD
(left/right cutoff apart, +-2 octaves: half down on the left, half up on the right) and FDBK
(band-pass fed back into the input through a soft clip; above ~75% it self-oscillates, and the
clip keeps the oscillation bounded). */
struct MasterFilter {
	Svf svf[2];
	float mixS[NUM_MIX] = {1.f, 0.f, 0.f, 0.f, 0.f};
	float cutoffS = 0.7f;
	float resS = 0.f;
	float spreadS = 0.f;
	float fdbkS = 0.f;

	static constexpr float FDBK_GAIN = 1.3f;

	void reset() {
		svf[0].reset();
		svf[1].reset();
	}

	/** mode: -1 = off, else FilterMode. cutoff 0..1, res 0..1, spread -1..1 (x2 octaves), fdbk 0..1.
	`coef` is the parameter smoothing coefficient per sample. */
	void process(float* l, float* r, int mode, float cutoff, float res, float spread, float fdbk, float coef, float sampleRate) {
		float target[NUM_MIX];
		modeWeights(mode >= 0, mode, target);
		for (int k = 0; k < NUM_MIX; k++)
			mixS[k] += (target[k] - mixS[k]) * coef;
		cutoffS += (std::min(std::max(cutoff, 0.f), 1.f) - cutoffS) * coef;
		resS += (std::min(std::max(res, 0.f), 1.f) - resS) * coef;
		spreadS += (std::min(std::max(spread, -1.f), 1.f) - spreadS) * coef;
		fdbkS += (std::min(std::max(fdbk, 0.f), 1.f) - fdbkS) * coef;

		if (mixS[0] > 0.99999f) {
			// Fully off: pass through and let the filter rest.
			reset();
			return;
		}
		float fc = filterHz(cutoffS);
		float q = filterQ(resS);
		float oct = spreadS * 2.f; // +-2 octaves between the channels
		float fcs[2] = {fc * std::exp2(-0.5f * oct), fc * std::exp2(0.5f * oct)};
		float* io[2] = {l, r};
		float onAmount = 1.f - mixS[0];
		for (int c = 0; c < 2; c++) {
			float x = *io[c];
			float fb = onAmount * fdbkS * FDBK_GAIN * std::tanh(svf[c].lastBp);
			// The dry share must stay the clean input, so the feedback only enters the filter core.
			float mixNoDry[NUM_MIX] = {0.f, mixS[1], mixS[2], mixS[3], mixS[4]};
			float y = svf[c].process(x + fb, fcs[c], q, sampleRate, mixNoDry);
			// The notch's own dry part (x - bp) carries the feedback too; take it back out.
			y -= mixS[4] * fb;
			y += mixS[0] * x;
			if (!std::isfinite(y)) {
				svf[c].reset();
				y = x;
			}
			*io[c] = y;
		}
	}
};

} // namespace wa
