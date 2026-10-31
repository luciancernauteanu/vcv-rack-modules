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
- LP / HP: unity passband at any RES (resonance adds a peak at the cutoff; it never takes level away).
- BP: unity at the center at the lowest RES, rising to sqrt(Q/0.7) at full RES (keeps broadband
  loudness roughly constant as the band narrows).
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

	/** Exactly linear up to 40% of `lim`, then a tanh knee that approaches `lim`. The linear part
	matters: a state clipper that bends even slightly at low levels acts like a leak on every
	sample and dulls the whole filter. */
	static float softLimit(float x, float lim) {
		float knee = 0.4f * lim;
		float a = std::fabs(x);
		if (a <= knee)
			return x;
		float room = lim - knee;
		float y = knee + room * std::tanh((a - knee) / room);
		return (x < 0.f) ? -y : y;
	}

	/** `stateLimit` > 0 makes the integrators saturate softly above 40% of that level, like capacitors
	running into the supply rails of an analog filter: normal signals stay linear, loud or resonant
	ones compress and gain odd harmonics instead of ringing without bound. 0 = ideal (clean). */
	float process(float x, float fc, float q, float sampleRate, const float mix[NUM_MIX], float stateLimit = 0.f) {
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
		if (stateLimit > 0.f) {
			ic1 = softLimit(ic1, stateLimit);
			ic2 = softLimit(ic2, stateLimit);
		}
		float lp = v2;
		float bp = k * v1; // unity gain at the center
		float hp = x - bp - lp;
		lastBp = bp;
		float bpGain = std::sqrt(q / FILTER_Q_MIN);
		return mix[0] * x + mix[1] * lp + mix[3] * hp + mix[2] * bp * bpGain + mix[4] * (x - bp);
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
DRIVE 0..1: input gain 1..3 (0..9.5 dB), a warm saturation rather than a fuzz. Output is scaled so a full-scale input stays near full
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

	float shape(float x, float g, float bias, float norm, float blend) {
		float u0 = u1, u = g * x;
		u1 = u;
		if (blend <= 0.f)
			return x;
		float sat = (std::fabs(u - u0) > 1e-4f) ? (logCosh(u + bias) - logCosh(u0 + bias)) / (u - u0) : std::tanh(0.5f * (u0 + u) + bias);
		sat = (sat - std::tanh(bias)) * norm;
		return x + (sat - x) * blend;
	}

	float process(float x, float drive, float bias = 0.f) {
		const Halfband& hb = fir();
		drive = std::min(std::max(drive, 0.f), 1.f);
		float g = std::exp(drive * 1.098612289f); // ln(3)
		// `bias` is a DC offset in input units; scaled by the gain and capped, so it tilts the clipping
		// (even harmonics) without pushing one side flat. Normalized by the larger peak.
		bias = std::min(bias * g, 0.8f);
		float pos = std::tanh(g + bias) - std::tanh(bias);
		float neg = std::tanh(g - bias) + std::tanh(bias);
		float norm = 1.f / std::max(pos, neg);
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
			up[0] = shape(s[p], g, bias, norm, blend);
		}
		float y = 0.f;
		for (int i = 0; i < Halfband::TAPS; i++)
			y += hb.h[i] * up[i];
		return y;
	}
};


/** The drive used everywhere (each artifact, and the master): `Drive` with a small bias for even
harmonics, a DC blocker for the offset the bias creates, and make-up gain so turning DRIVE up adds
character, not level. Always in the path (constant latency). */
struct WarmDrive {
	Drive drive;
	float dcX = 0.f;
	float dcY = 0.f;

	/** Bias at full drive, in input units (scaled by the drive gain inside Drive) */
	static constexpr float BIAS = 0.2f;
	static constexpr float DC_BLOCK = 0.9995f;
	/** Level given back as DRIVE rises, so full drive is about as loud as drive 0 */
	static constexpr float MAKEUP = 0.35f;

	void reset() {
		drive.reset();
		dcX = dcY = 0.f;
	}

	float process(float x, float amount) {
		amount = std::min(std::max(amount, 0.f), 1.f);
		float y = drive.process(x, amount, BIAS * amount);
		float out = y - dcX + DC_BLOCK * dcY;
		dcX = y;
		dcY = out;
		out *= 1.f + MAKEUP * amount;
		return std::isfinite(out) ? out : 0.f;
	}
};

/** Master filter settings. Knob values are smoothed by the filter; the *Mod values (CV) are added
after the smoothing, so they can run at audio rate. */
struct MasterParams {
	/** -1 = off, else FilterMode */
	int mode = -1;
	float cutoff = 0.7f;
	float res = 0.f;
	/** -1..1 = -2..+2 octaves between left and right */
	float spread = 0.f;
	float fdbk = 0.f;
	float drive = 0.f;
	float cutoffMod = 0.f;
	float resMod = 0.f;
	float spreadMod = 0.f;
	float fdbkMod = 0.f;
	float driveMod = 0.f;
};

/** Stereo master section: drive -> multimode filter.
Warmth comes from three analog habits, all gentle at low settings:
- DRIVE is an asymmetric soft clip (a small bias before the tanh), which adds even harmonics (the
  "tube" 2nd) next to the odd ones, then a DC blocker removes the offset the bias creates. It runs
  oversampled with ADAA, so it stays smooth up top.
- The filter's integrators saturate softly (STATE_LIMIT), so resonance and loud signals compress
  and thicken instead of ringing like a perfect digital filter.
- FDBK feeds the band-pass back through its own soft clip (zero delay). Loop gain at 100% is just
  above 1, so most of the knob is ringing resonance and only the top ~8% self-oscillates, at a
  moderate level (~2 V, was 5.7 V). Feedback eases off at high cutoffs, so it cannot whistle above
  ~4 kHz. DRIVE also drives this feedback clip (harder clip, a little more loop gain): more drive =
  grittier, more compressed feedback, not louder. The oversampled drive itself stays outside the
  loop: its ~0.7 ms delay inside a feedback loop would detune and smear the resonance.
With DRIVE at 0 and the filter off, only the drive's oversampling path remains (flat to ~0.2 dB at
15 kHz, 31 samples of delay). */
struct MasterFilter {
	Svf svf[2];
	WarmDrive drive[2];
	// Starts as the panel default: low-pass, wide open (20 kHz), so there is no sweep at start-up.
	float mixS[NUM_MIX] = {0.f, 1.f, 0.f, 0.f, 0.f};
	float cutoffS = 1.f;
	float resS = 0.f;
	float spreadS = 0.f;
	float fdbkS = 0.f;
	float driveS = 0.f;

	/** Loop gain at FDBK 100% (self-oscillation starts at 1/FDBK_GAIN of the knob's effect) */
	static constexpr float FDBK_GAIN = 1.12f;
	/** Feedback clip: tanh(k * bp) / k. Larger k = clips earlier = quieter, grittier oscillation. */
	static constexpr float FDBK_CLIP = 1.3f;
	/** DRIVE pushes the feedback clip harder (k x (1 + this)) and raises loop gain a little. */
	static constexpr float FDBK_CLIP_DRIVE = 1.5f;
	static constexpr float FDBK_GAIN_DRIVE = 0.15f;
	/** Above this cutoff the feedback eases off (1 / (1 + (fc / corner)^2)) */
	static constexpr float FDBK_HF_CORNER = 12000.f;
	/** Integrator ceiling in engine units (1 = a full-scale input) */
	static constexpr float STATE_LIMIT = 2.5f;

	/** Clears all filter and drive memory (not the smoothed settings). */
	void reset() {
		svf[0].reset();
		svf[1].reset();
		drive[0].reset();
		drive[1].reset();
	}

	void process(float* l, float* r, const MasterParams& p, float coef, float sampleRate) {
		float target[NUM_MIX];
		modeWeights(p.mode >= 0, p.mode, target);
		for (int k = 0; k < NUM_MIX; k++)
			mixS[k] += (target[k] - mixS[k]) * coef;
		cutoffS += (std::min(std::max(p.cutoff, 0.f), 1.f) - cutoffS) * coef;
		resS += (std::min(std::max(p.res, 0.f), 1.f) - resS) * coef;
		spreadS += (std::min(std::max(p.spread, -1.f), 1.f) - spreadS) * coef;
		fdbkS += (std::min(std::max(p.fdbk, 0.f), 1.f) - fdbkS) * coef;
		driveS += (std::min(std::max(p.drive, 0.f), 1.f) - driveS) * coef;

		auto clamp01 = [](float v) {
			return std::min(std::max(v, 0.f), 1.f);
		};
		float* io[2] = {l, r};

		// Drive (always in the path, so turning it never shifts timing).
		float dr = clamp01(driveS + p.driveMod);
		for (int c = 0; c < 2; c++)
			*io[c] = drive[c].process(*io[c], dr);

		if (mixS[0] > 0.99999f) {
			// Filter fully off: let it rest.
			svf[0].reset();
			svf[1].reset();
			return;
		}
		float fc = filterHz(clamp01(cutoffS + p.cutoffMod));
		float q = filterQ(clamp01(resS + p.resMod));
		float fdbk = clamp01(fdbkS + p.fdbkMod);
		float spread = std::min(std::max(spreadS + p.spreadMod, -1.f), 1.f);
		float oct = spread * 2.f; // +-2 octaves between the channels
		float fcs[2] = {fc * std::exp2(-0.5f * oct), fc * std::exp2(0.5f * oct)};
		float onAmount = 1.f - mixS[0];
		// Feedback clip and loop gain, shaped by DRIVE (see the struct notes).
		float clipK = FDBK_CLIP * (1.f + FDBK_CLIP_DRIVE * dr);
		float loopGain = onAmount * fdbk * FDBK_GAIN * (1.f + FDBK_GAIN_DRIVE * dr);
		for (int c = 0; c < 2; c++) {
			float x = *io[c];
			float hf = fcs[c] / FDBK_HF_CORNER;
			float fb = loopGain / (1.f + hf * hf) * std::tanh(clipK * svf[c].lastBp) / clipK;
			// The dry share must stay the clean input, so the feedback only enters the filter core.
			float mixNoDry[NUM_MIX] = {0.f, mixS[1], mixS[2], mixS[3], mixS[4]};
			float y = svf[c].process(x + fb, fcs[c], q, sampleRate, mixNoDry, STATE_LIMIT);
			// The notch's own dry part (x - bp) carries the feedback too; take it back out.
			y -= mixS[4] * fb;
			y += mixS[0] * x;
			if (!std::isfinite(y)) {
				svf[c].reset();
				y = x;
			}
			ys[c] = y;
			// Energy in and out of each channel's filter (slow), for the spread balance.
			energyIn[c] += (x * x - energyIn[c]) * balanceCoef;
			energyOut[c] += (y * y - energyOut[c]) * balanceCoef;
		}

		// Balanced spread: SPREAD gives each channel a different cutoff, so with a low cutoff the
		// darker side loses most of the sound and the image leans to the brighter side. Even out
		// the filters' own losses (output/input energy per channel, not the input's balance, so
		// panning is kept): spread then changes tone between the sides, not their level. Fades in
		// over the first 10% of SPREAD (so 0 is untouched), then corrects fully; limited to +-6 dB.
		float amount = std::min(1.f, 10.f * std::fabs(spread));
		// Only when both channels carry signal (a hard-panned input has nothing to balance against).
		const float quiet = 1e-6f;
		if (amount > 1e-3f && energyIn[0] > quiet && energyIn[1] > quiet) {
			const float eps = 1e-9f;
			float loss[2] = {(energyOut[0] + eps) / (energyIn[0] + eps), (energyOut[1] + eps) / (energyIn[1] + eps)};
			float mean = std::sqrt(loss[0] * loss[1]);
			for (int c = 0; c < 2; c++) {
				float g = std::sqrt(mean / loss[c]);
				g = std::min(std::max(g, 0.5f), 2.f);
				ys[c] *= std::pow(g, amount);
			}
		}
		*l = ys[0];
		*r = ys[1];
	}

	void setSampleRate(float sr) {
		balanceCoef = 1.f - std::exp(-1.f / (BALANCE_TIME * sr));
	}

private:
	/** Time constant of the spread balance's level tracking */
	static constexpr float BALANCE_TIME = 0.15f;
	float balanceCoef = 1.f - std::exp(-1.f / (0.15f * 44100.f));
	float energyIn[2] = {};
	float energyOut[2] = {};
	float ys[2] = {};
};

} // namespace wa
