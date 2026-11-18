#pragma once
#include <algorithm>
#include <cmath>

// Copied from waveArtifact (same author): the warm drive (tanh with ADAA at 2x oversampling). No Rack dependencies.

namespace np {


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

} // namespace np
