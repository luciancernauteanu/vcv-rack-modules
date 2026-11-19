#pragma once
#include <algorithm>
#include <cmath>

// One filter core of Nur Prisma, and the maps from the knobs to its settings. No Rack dependencies (tested in tests/).
// Own implementation: a trapezoidal (Cytomic / Simper form) state-variable filter, as in waveArtifact's Svf.

#if defined(__GNUC__)
#define NP_NOINLINE __attribute__((noinline))
#else
#define NP_NOINLINE
#endif

namespace np {

static const float Q_MIN = 0.7f;
static const float Q_MAX = 300.f;
static const float LN_Q_RANGE = 6.059123f; // ln(300 / 0.7)
/** RES above this adds feedback: rings sustain, the very top self-oscillates */
static const float RES_FB_START = 0.92f;
/** Loop gain at RES 100% (self-oscillation starts at 1/1.12 of the feedback range) */
static const float FB_GAIN = 1.12f;
/** Feedback clip tanh(c v1) / c on the raw resonator state v1: the self-oscillation settles near 0.65 / c (~3 V) */
static const float FB_CLIP = 1.f;
static const float FB_HF_CORNER = 12000.f;
/** Level-dependent resonance (ANALOG): damping factor 1 + RES_SAT * ANALOG * v1^2, v1^2 capped (keeps it bounded) */
static const float RES_SAT = 0.6f;
static const float RES_SAT_CAP = 16.f;
static const float LOG2_20 = 4.321928f;
static const float LOG2_1000 = 9.965784f;

inline float clamp01(float x) {
	return std::min(std::max(x, 0.f), 1.f);
}

/** RES 0..1 -> Q 0.7 .. 300 (reached at RES_FB_START), squared so the middle of the knob stays musical:
50% ~ 4.2, 70% ~ 23, 85% ~ 120, 92% 300. Ring time (T60) ~ 2.2 Q / f seconds: low notes ring longer. */
inline float resQ(float res) {
	float t = std::min(clamp01(res) / RES_FB_START, 1.f);
	return Q_MIN * std::exp(LN_Q_RANGE * t * t);
}

/** Feedback amount above RES_FB_START (0 .. FB_GAIN): lowers the damping to k (1 - fb); >= 1 self-oscillates. */
inline float resFeedback(float res) {
	res = clamp01(res);
	return res > RES_FB_START ? FB_GAIN * (res - RES_FB_START) / (1.f - RES_FB_START) : 0.f;
}

/** TYPE 0..3 -> weights for LP, BP, HP, notch: each step crossfades to the next. */
inline void typeWeights(float type, float w[4]) {
	type = std::min(std::max(type, 0.f), 3.f);
	int i = std::min((int) type, 2);
	float f = type - i;
	w[0] = w[1] = w[2] = w[3] = 0.f;
	w[i] = 1.f - f;
	w[i + 1] = f;
}

/** tanh, Pade approximation (exact enough for a feedback clip; cheap per sample). */
inline float fastTanh(float x) {
	x = std::min(std::max(x, -3.f), 3.f);
	float x2 = x * x;
	return x * (27.f + x2) / (27.f + 9.f * x2);
}

/** Exactly linear up to 40% of `lim`, then a tanh knee that approaches `lim` (waveArtifact's recipe: a clipper
that bends at low levels acts like a leak and dulls the filter). */
inline float softLimit(float x, float lim) {
	float knee = 0.4f * lim;
	float a = std::fabs(x);
	if (a <= knee)
		return x;
	float room = lim - knee;
	float y = knee + room * std::tanh((a - knee) / room);
	return (x < 0.f) ? -y : y;
}

/** Per-core coefficients, worked out once per control block. */
struct CoreCoefs {
	/** Integrator gain tan(pi fc / sr) */
	float g = 0.f;
	/** Damping 1 / Q */
	float k = 1.f;
	/** BP output gain sqrt(Q / 0.7): keeps broadband loudness as the band narrows */
	float bpGain = 1.f;
	/** Feedback loop gain, HF-eased */
	float fb = 0.f;
};

/** One state-variable core: LP, BP, HP and notch at once, morphed by TYPE weights. */
struct Core {
	float ic1 = 0.f;
	float ic2 = 0.f;
	/** Last raw band state v1 (the normalized band-pass is k v1), for the feedback */
	float lastV1 = 0.f;

	void reset() {
		ic1 = ic2 = lastV1 = 0.f;
	}

	/** Adds energy straight into the resonator (a ping): the ring that follows has about this amplitude, whatever the
	frequency or Q. Where it goes decides how the ring starts in each output: energy in the band state (ic1) starts
	LP and HP at zero but makes BP / notch jump; energy in the low state (ic2) does the opposite. `bpShare` (the
	TYPE weight of BP + notch) splits it, so every output's ring starts from zero: no click, even on a hard strike or
	on a core that is still ringing. */
	void excite(float amount, float bpShare = 0.f) {
		ic1 += amount * (1.f - bpShare);
		ic2 += amount * bpShare;
	}

	/** Amplitude of the ringing (the two states are a quadrature pair at resonance) */
	float ringLevel() const {
		return 0.5f * std::sqrt(ic1 * ic1 + ic2 * ic2);
	}

	/** Level-dependent resonance (ANALOG): the damping grows with the core's own ringing level, k (1 + resSat v1^2),
	like an OTA / ladder loop running into saturation: a loud input or a hard ping widens and softens the peak while
	quiet signals ring freely (and a loud ring "blooms": it settles faster, then rings on). Kept out of process() and
	used only by the SAT instantiation of the engine's core loop: an optional term inside the plain loop made the
	compiler regroup its maths (cpu.md bit-identity trap). */
	float saturatedK(float k, float resSat) const {
		return k * (1.f + resSat * std::min(lastV1 * lastV1, RES_SAT_CAP));
	}

	/** One sample. `w` = TYPE weights; `lim` > 0 saturates the integrators above 40% of it (analog). */
	float process(float u, float g, float k, const float w[4], float bpGain, float fb, float lim) {
		float x = u;
		if (fb > 0.f)
			x += fb * k * fastTanh(FB_CLIP * lastV1) * (1.f / FB_CLIP);
		float a1 = 1.f / (1.f + g * (g + k));
		float a2 = g * a1;
		float a3 = g * a2;
		float v3 = x - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		if (lim > 0.f) {
			ic1 = softLimit(ic1, lim);
			ic2 = softLimit(ic2, lim);
		}
		float lp = v2;
		float bp = k * v1;
		float hp = x - bp - lp;
		lastV1 = v1;
		return w[0] * lp + w[1] * bp * bpGain + w[2] * hp + w[3] * (u - bp);
	}
};

/** Coefficients for a core at `fc` Hz with resonance `res` (0..1); `qScale` is the analog mismatch on Q. */
inline CoreCoefs coreCoefs(float fc, float res, float qScale, float sampleRate) {
	CoreCoefs c;
	fc = std::min(std::max(fc, 8.f), 0.45f * sampleRate);
	c.g = std::tan(3.14159265f * fc / sampleRate);
	float q = resQ(res) * qScale;
	c.k = 1.f / q;
	c.bpGain = std::sqrt(q / Q_MIN);
	float hf = fc / FB_HF_CORNER;
	c.fb = resFeedback(res) / (1.f + hf * hf);
	return c;
}

/** BAND: a steep band-pass after a core, at the core's own pitch: two 2-pole band-passes in series (12 dB/oct skirts),
unity at the centre. Turns the four cores into a filter bank of narrow slices; the core's own ring (RES, PING) is at
the centre, so it passes. */
struct BandStage {
	float s[2][2] = {};

	void reset() {
		s[0][0] = s[0][1] = s[1][0] = s[1][1] = 0.f;
	}

	float process(float x, float g, float k) {
		float a1 = 1.f / (1.f + g * (g + k));
		float a2 = g * a1;
		float a3 = g * a2;
		for (int st = 0; st < 2; st++) {
			float v3 = x - s[st][1];
			float v1 = a1 * s[st][0] + a2 * v3;
			float v2 = s[st][1] + a2 * s[st][0] + a3 * v3;
			s[st][0] = 2.f * v1 - s[st][0];
			s[st][1] = 2.f * v2 - s[st][1];
			x = k * v1;
		}
		return x;
	}
};

/** BAND 0..1 -> the band's -3 dB width in octaves: 3 octaves down to 1/24 octave (log). */
inline float bandOctaves(float band) {
	return 3.f * std::exp(-4.276666f * clamp01(band)); // ln(72)
}

/** Damping of each of the two stages for a cascade `oct` octaves wide (-3 dB). One 2-pole band-pass of that width
has Q = sqrt(r) / (r - 1), r = 2^oct; two in series are narrower by sqrt(sqrt(2) - 1), so each stage is that much wider. */
inline float bandStageK(float oct) {
	float r = std::exp2(oct);
	float q = std::sqrt(r) / (r - 1.f) * 0.6435943f;
	return 1.f / q;
}

/** Make-up gain for a band `oct` octaves wide: a narrow slice carries less of a broadband sound, so give some back
(about +10 dB at the narrowest; the gain compensation and the limiter do the rest). */
inline float bandMakeup(float oct) {
	return std::max(1.f, std::pow(2.f / oct, 0.35f));
}

/** How loud a ping rings in the output for this TYPE mix, per unit of excitation (LP and HP ring at ~1, the
normalized BP at k * bpGain, the notch hardly at all). Floored so the excitation never needs to be huge. */
inline float pingResponse(const float w[4], float k, float bpGain) {
	return std::max(w[0] + w[2] + w[1] * k * bpGain + 0.1f * w[3], 0.45f);
}

} // namespace np
