#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Lfo.hpp"
#include "WavetableBank.hpp"

// Per-sample morph oscillator. No Rack dependencies, so it can be tested offline (tests/).
//
// Signal path: each marker has its own pitch and phase -> read the two neighboring markers'
// tables (band-limited mip level picked from each marker's frequency) -> crossfade by position
// -> band-pass with dry/wet -> equal-power pan -> declick gain.
//
// The morph is audio-only: pitch is NOT interpolated between markers, so a marker at -12 st and
// a marker at 0 st crossfade as two separate pitches, never sliding (glide could be a later option).
//
// Click prevention:
// - Position is slewed (5 ms). The crossfade weight is continuous in position, and so are
//   cutoff and pan, which are interpolated between the two markers.
// - A marker's wavetable position glides (small moves) or crossfades over 20 ms (jumps).
// - Structural changes (marker added/removed/reordered, bank swapped) fade out 5 ms, apply
//   while silent, fade back in 5 ms.
// - Per-marker gain is smoothed (5 ms). A linked peak limiter keeps the output within 10 V when
//   gain or resonance push it hot.

namespace wa {

static const int NUM_MARKERS = 5;
/** WAVE glide range: the minimum is the default knob-step smoothing, the maximum a slow scan. */
static const float MIN_WAVE_SMOOTH = 0.02f;
static const float MAX_WAVE_SMOOTH = 2.f;

struct MarkerInput {
	bool active = false;
	/** 0..1 along the bar */
	float pos = 0.f;
	/** Wavetable position 0..1 across the bank. Fractional values morph between neighboring tables. */
	float wave = 0.f;
	/** Glide time for WAVE moves in seconds, from MIN_WAVE_SMOOTH (default) up to MAX_WAVE_SMOOTH. */
	float waveSmooth = 0.02f;
	/** WAVE LFO output -1..1. Added after the glide, so its depth and timing are exact. */
	float lfo = 0.f;
	/** LFO depth 0..1 in WAVE units (1 = swing across the whole sample). Smoothed in the engine. */
	float lfoAmount = 0.f;
	/** 0 = filter off, otherwise center frequency 20 Hz * 1000^bp (20 Hz..20 kHz) */
	float bp = 0.f;
	/** Band-pass resonance 0..1: Q 0.7 (wide) .. 20 (narrow, ringing) */
	float res = 0.f;
	/** Linear gain applied to this marker's audio, 0..4 (400%) */
	float gain = 1.f;
	/** Semitones */
	float pitch = 0.f;
	/** -1 (left) .. 1 (right) */
	float pan = 0.f;
};

struct EngineInput {
	MarkerInput markers[NUM_MARKERS];
	/** 0..1 along the bar */
	float position = 0.f;
	/** Octaves relative to C4 */
	float voct = 0.f;
	const WavetableBank* bank = nullptr;
	/** Fade out and stay silent, e.g. while a bank swap is pending. */
	bool holdSilent = false;
};

class MorphEngine {
public:
	MorphEngine() {
		setSampleRate(44100.f);
	}

	void setSampleRate(float sr) {
		sampleRate = sr;
		smoothCoef = 1.f - std::exp(-1.f / (SMOOTH_TIME * sr));
		fadeStep = 1.f / (DECLICK_TIME * sr);
		tableFadeStep = 1.f / (TABLE_FADE_TIME * sr);
		for (int i = 0; i < NUM_MARKERS; i++)
			tables[i].smoothTime = -1.f; // recompute glide coefficients for the new rate
		lfoAmountCoef = 1.f - std::exp(-1.f / (LFO_AMOUNT_SMOOTH_TIME * sr));
		limReleaseCoef = 1.f - std::exp(-1.f / (LIMIT_RELEASE_TIME * sr));
		limHoldSamples = (int) (LIMIT_HOLD_TIME * sr);
	}

	float getSampleRate() const {
		return sampleRate;
	}

	/** True once the declick fade has reached silence. */
	bool isSilent() const {
		return gain <= 0.f;
	}

	/** Smoothed playhead, for display. */
	float getPosition() const {
		return posS;
	}

	/** Current (gliding) fractional table index of a marker's WAVE knob, before the LFO. */
	float getTableIndex(int marker) const {
		return tables[marker].cur;
	}

	/** Table index actually played, including the LFO. Updated only while the marker is heard. */
	float getPlayedIndex(int marker) const {
		return playedIndex[marker];
	}

	void process(const EngineInput& in, float* outL, float* outR) {
		*outL = 0.f;
		*outR = 0.f;

		// Desired structure: active markers sorted by position. Marker 1 always exists.
		int ord[NUM_MARKERS];
		int n = 0;
		for (int i = 0; i < NUM_MARKERS; i++) {
			if (in.markers[i].active)
				ord[n++] = i;
		}
		if (n == 0)
			ord[n++] = 0;
		for (int i = 1; i < n; i++) {
			int v = ord[i];
			int j = i - 1;
			while (j >= 0 && in.markers[ord[j]].pos > in.markers[v].pos) {
				ord[j + 1] = ord[j];
				j--;
			}
			ord[j + 1] = v;
		}
		uint32_t sig = n;
		for (int i = 0; i < n; i++)
			sig = sig * 8 + ord[i];

		bool changed = !structureApplied || sig != appliedSig || in.bank != appliedBank;
		bool wantSilent = in.holdSilent || changed || !in.bank;
		gain = wantSilent ? std::max(0.f, gain - fadeStep) : std::min(1.f, gain + fadeStep);

		if (changed && gain <= 0.f) {
			appliedSig = sig;
			numActive = n;
			std::copy(ord, ord + n, order);
			if (in.bank != appliedBank) {
				// appliedBank is only an identity here; it may already be freed. Never read it.
				appliedBank = in.bank;
				for (int i = 0; i < NUM_MARKERS; i++) {
					float t = tableIndex(in.markers[i].wave, in.bank);
					tables[i].cur = tables[i].prev = tables[i].lastTarget = t;
					tables[i].fade = 1.f;
				}
				ic1 = ic2 = 0.f;
			}
			structureApplied = true;
		}

		float target = std::min(std::max(in.position, 0.f), 1.f);
		posS += (target - posS) * smoothCoef;

		// Wavetable position per marker (the WAVE knob; the LFO is added later, after this glide).
		// Moves glide (one-pole, time set by the marker's SMOOTH), so the read passes smoothly
		// through neighboring tables.
		// At minimum SMOOTH, a target that jumps by more than JUMP_TABLES in one step (a fast knob
		// flick or a menu/dot pick) crossfades old -> new over 20 ms instead, because sweeping
		// through every table in 20 ms sounds like a zip. Jumps are detected from the target's own
		// step, not from how far the glide trails, so steady fast movement keeps gliding.
		// With a longer SMOOTH the sweep through the tables is the point, so everything glides.
		// A move that arrives during a running crossfade waits for it to finish; restarting would
		// jump by up to half the difference.
		for (int i = 0; i < NUM_MARKERS; i++) {
			TableState& s = tables[i];
			float amt = std::min(std::max(in.markers[i].lfoAmount, 0.f), 1.f);
			s.lfoAmountS += (amt - s.lfoAmountS) * lfoAmountCoef;
			if (s.fade < 1.f) {
				s.fade = std::min(1.f, s.fade + tableFadeStep);
				continue;
			}
			float smooth = std::min(std::max(in.markers[i].waveSmooth, MIN_WAVE_SMOOTH), MAX_WAVE_SMOOTH);
			if (smooth != s.smoothTime) {
				s.smoothTime = smooth;
				s.smoothCoef = 1.f - std::exp(-1.f / (smooth * sampleRate));
			}
			float t = tableIndex(in.markers[i].wave, in.bank);
			bool atMinimum = smooth <= MIN_WAVE_SMOOTH * 1.05f;
			bool jumped = std::fabs(t - s.lastTarget) > JUMP_TABLES;
			s.lastTarget = t;
			if (atMinimum && jumped) {
				s.prev = s.cur;
				s.cur = t;
				s.fade = 0.f;
			}
			else {
				s.cur += (t - s.cur) * s.smoothCoef;
			}
		}

		if (!in.bank || gain <= 0.f)
			return;
		const WavetableBank& bank = *in.bank;

		// Neighboring markers a, b and blend weight w, using the applied (current) structure.
		int a = order[0], b = order[0];
		float w = 0.f;
		if (numActive > 1 && posS > in.markers[order[0]].pos) {
			if (posS >= in.markers[order[numActive - 1]].pos) {
				a = b = order[numActive - 1];
			}
			else {
				for (int k = 0; k + 1 < numActive; k++) {
					float pa = in.markers[order[k]].pos;
					float pb = in.markers[order[k + 1]].pos;
					if (posS <= pb) {
						a = order[k];
						b = order[k + 1];
						w = (pb - pa > 1e-6f) ? (posS - pa) / (pb - pa) : 0.f;
						break;
					}
				}
			}
		}
		const MarkerInput& A = in.markers[a];
		const MarkerInput& B = in.markers[b];

		// Pitch belongs to each marker. The morph only crossfades audio between the two markers'
		// oscillators, so the pitch never slides while the position moves.
		// Each marker has its own phase, and all five keep running so that markers with the same
		// pitch stay phase-locked (their tables then blend without cancelling) whenever they meet.
		float freqs[NUM_MARKERS];
		for (int m = 0; m < NUM_MARKERS; m++) {
			float f = FREQ_C4 * std::exp2(in.voct + in.markers[m].pitch / 12.f);
			freqs[m] = std::min(std::max(f, 0.01f), 0.45f * sampleRate);
		}

		auto readMarker = [&](int m) {
			// Mip level: one octave of headroom so the highest harmonic stays below Nyquist.
			float maxHarmonic = sampleRate / (2.f * freqs[m]);
			float lv = std::log2((FRAME_SIZE / 2) / maxHarmonic) + 1.f;
			lv = std::min(std::max(lv, 0.f), (float) (NUM_LEVELS - 1));
			int l0 = (int) lv;
			int l1 = std::min(l0 + 1, NUM_LEVELS - 1);
			float lf = lv - l0;

			float x = (float) (phases[m] * FRAME_SIZE);
			int i0 = std::min((int) x, FRAME_SIZE - 1);
			int i1 = (i0 + 1) & (FRAME_SIZE - 1);
			float fr = x - i0;
			auto readTable = [&](int t) {
				const float* f0 = bank.frame(t, l0);
				const float* f1 = bank.frame(t, l1);
				float s0 = f0[i0] + (f0[i1] - f0[i0]) * fr;
				float s1 = f1[i0] + (f1[i1] - f1[i0]) * fr;
				return s0 + (s1 - s0) * lf;
			};
			// Fractional index: blend the two neighboring tables.
			auto readIndex = [&](float idx) {
				int t0 = std::min(std::max((int) idx, 0), bank.numTables - 1);
				int t1 = std::min(t0 + 1, bank.numTables - 1);
				float tf = idx - t0;
				float v0 = readTable(t0);
				return (tf > 0.f && t1 != t0) ? v0 + (readTable(t1) - v0) * tf : v0;
			};
			// The LFO offset is added to the glided position and folded back at the sample's ends.
			const TableState& s = tables[m];
			float span = (float) (bank.numTables - 1);
			float mod = s.lfoAmountS * in.markers[m].lfo;
			auto modulated = [&](float idx) {
				return (span > 0.f && mod != 0.f) ? fold01(idx / span + mod) * span : idx;
			};
			float idx = modulated(s.cur);
			playedIndex[m] = idx;
			float v = readIndex(idx);
			if (s.fade < 1.f) {
				float old = readIndex(modulated(s.prev));
				v = old + (v - old) * s.fade;
			}
			return v;
		};
		for (int m = 0; m < NUM_MARKERS; m++) {
			// (float) copy: std::min takes references, and C++11 has no inline constexpr members.
			float g = std::min(std::max(in.markers[m].gain, 0.f), (float) MAX_GAIN);
			gainS[m] += (g - gainS[m]) * smoothCoef;
		}
		float va = readMarker(a) * gainS[a];
		float vb = (b == a) ? va : readMarker(b) * gainS[b];
		float dry = va + (vb - va) * w;

		for (int m = 0; m < NUM_MARKERS; m++) {
			phases[m] += freqs[m] / sampleRate;
			if (phases[m] >= 1.0)
				phases[m] -= std::floor(phases[m]);
		}

		// Band-pass: wet amount and cutoff are interpolated; an "Off" marker contributes dry.
		bool onA = A.bp > 0.f;
		bool onB = B.bp > 0.f;
		float wetT = (onA ? 1.f - w : 0.f) + (onB ? w : 0.f);
		float bpT = (onA && onB) ? A.bp + (B.bp - A.bp) * w : onA ? A.bp : onB ? B.bp : bpS;
		float resT = (onA && onB) ? A.res + (B.res - A.res) * w : onA ? A.res : onB ? B.res : resS;
		wetS += (wetT - wetS) * smoothCoef;
		bpS += (bpT - bpS) * smoothCoef;
		resS += (std::min(std::max(resT, 0.f), 1.f) - resS) * smoothCoef;
		float panT = A.pan + (B.pan - A.pan) * w;
		panS += (panT - panS) * smoothCoef;

		float fc = 20.f * std::exp(bpS * LN_1000);
		fc = std::min(fc, 0.45f * sampleRate);
		float q = Q_MIN * std::exp(resS * LN_Q_RANGE);
		float wet = bandpass(dry, fc, q);
		float out = (dry + (wet - dry) * wetS) * gain;

		if (!std::isfinite(out)) {
			ic1 = ic2 = 0.f;
			return;
		}

		// Equal-power pan, unity at center.
		float angle = (panS + 1.f) * (float) (M_PI_4_F);
		float l = out * std::cos(angle) * SQRT2;
		float r = out * std::sin(angle) * SQRT2;

		// Peak limiter at LIMIT (x5 V = 10 V), linked on both channels. Instant attack, 50 ms hold
		// (longer than one cycle down to 20 Hz, so the gain stays steady within a waveform instead of
		// clipping each peak), then a 200 ms release. It turns the level down instead of shaping the
		// waveform, so a steady tone stays clean, and signals below the limit pass untouched.
		float peak = std::max(std::fabs(l), std::fabs(r));
		if (peak >= limEnv) {
			limEnv = peak;
			limHoldLeft = limHoldSamples;
		}
		else if (limHoldLeft > 0) {
			limHoldLeft--;
		}
		else {
			limEnv += (peak - limEnv) * limReleaseCoef;
		}
		if (limEnv > LIMIT) {
			float g = LIMIT / limEnv;
			l *= g;
			r *= g;
		}
		*outL = l;
		*outR = r;
	}

private:
	static constexpr float SMOOTH_TIME = 0.005f;
	static constexpr float DECLICK_TIME = 0.005f;
	static constexpr float TABLE_FADE_TIME = 0.02f;
	/** AMT smoothing. Slow enough that a 0 -> 100% jump does not sweep the tables in a few ms (a
	zip), fast enough to follow the knob's 60 Hz steps without lag you would notice. */
	static constexpr float LFO_AMOUNT_SMOOTH_TIME = 0.05f;
	/** At minimum SMOOTH, moves larger than this many tables crossfade instead of gliding. */
	static constexpr float JUMP_TABLES = 2.f;
	static constexpr float FREQ_C4 = 261.6256f;
	static constexpr float LN_1000 = 6.907755279f;
	static constexpr float SQRT2 = 1.414213562f;
	static constexpr float M_PI_4_F = 0.785398163f;
	static constexpr float PI_F = 3.141592654f;
	/** Resonance 0 = Q 0.7 (wide band), 1 = Q 20. */
	static constexpr float Q_MIN = 0.7f;
	static constexpr float LN_Q_RANGE = 3.352407217f; // ln(20 / 0.7)
	static constexpr float MAX_GAIN = 4.f;
	/** Output ceiling in engine units; the module scales by 5 V, so this is 10 V. */
	static constexpr float LIMIT = 2.f;
	static constexpr float LIMIT_HOLD_TIME = 0.05f;
	static constexpr float LIMIT_RELEASE_TIME = 0.2f;

	struct TableState {
		/** Fractional table index */
		float cur = 0.f;
		float prev = 0.f;
		float fade = 1.f;
		/** Cached glide coefficient for smoothTime, recomputed only when SMOOTH changes. */
		float smoothTime = -1.f;
		float smoothCoef = 0.f;
		/** Target at the previous sample, to tell jumps from steady movement */
		float lastTarget = 0.f;
		/** Smoothed LFO depth (AMT knob steps would otherwise jump the modulation) */
		float lfoAmountS = 0.f;
	};

	static float tableIndex(float wave, const WavetableBank* bank) {
		int maxT = bank ? bank->numTables - 1 : 0;
		return std::min(std::max(wave, 0.f), 1.f) * std::max(maxT, 0);
	}

	/** Trapezoidal SVF band-pass (Cytomic/Simper), stable under cutoff/Q modulation.
	Center gain is sqrt(Q / 0.7): unity at the default Q, a moderate boost when resonant, so the
	peak rings without the full Q-times jump of a raw SVF band output. */
	float bandpass(float x, float fc, float q) {
		float k = 1.f / q;
		float g = std::tan(PI_F * fc / sampleRate);
		float a1 = 1.f / (1.f + g * (g + k));
		float a2 = g * a1;
		float a3 = g * a2;
		float v3 = x - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		return k * v1 * std::sqrt(q / Q_MIN);
	}

	float sampleRate = 44100.f;
	float smoothCoef = 0.f;
	float fadeStep = 0.f;
	float tableFadeStep = 0.f;
	float limReleaseCoef = 0.f;
	float lfoAmountCoef = 0.f;
	int limHoldSamples = 0;
	int limHoldLeft = 0;

	double phases[NUM_MARKERS] = {};
	float posS = 0.f;
	float panS = 0.f;
	float bpS = 0.5f;
	float wetS = 0.f;
	float resS = 0.f;
	float gainS[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	float limEnv = 0.f;
	/** Declick gain (structural fades) */
	float gain = 0.f;
	float ic1 = 0.f;
	float ic2 = 0.f;

	bool structureApplied = false;
	uint32_t appliedSig = 0;
	const WavetableBank* appliedBank = nullptr;
	int order[NUM_MARKERS] = {0, 1, 2, 3, 4};
	int numActive = 1;
	TableState tables[NUM_MARKERS];
	float playedIndex[NUM_MARKERS] = {};
};

} // namespace wa
