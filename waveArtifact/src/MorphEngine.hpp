#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Filters.hpp"
#include "Lfo.hpp"
#include "Limiter.hpp"
#include "WavetableBank.hpp"

// Per-sample morph oscillator. No Rack dependencies, so it can be tested offline (tests/).
//
// Signal path: each marker has its own pitch and phase -> read the two neighboring markers'
// tables (band-limited mip level picked from each marker's frequency) -> crossfade by position
// -> drive -> multimode filter (its mode mix morphs between the markers) -> declick gain
// -> equal-power pan -> preview mix -> stereo master filter -> limiter.
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
// - Per-marker gain is smoothed (5 ms). A look-ahead limiter (SafetyLimiter) keeps the output within 10 V when
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
	/** WAVE LFO output -1..1. Moves the WAVE glide target, so the marker's SMOOTH applies to it. */
	float lfo = 0.f;
	/** LFO depth 0..1 in WAVE units (1 = swing across the whole sample). Smoothed in the engine. */
	float lfoAmount = 0.f;
	/** 0 = filter off, otherwise cutoff 20 Hz * 1000^cutoff (20 Hz..20 kHz) */
	float cutoff = 0.f;
	/** FilterMode: LP, BP, HP, notch */
	int filterMode = MODE_LP;
	/** Resonance 0..1: Q 0.7 (wide) .. 20 (narrow, ringing) */
	float res = 0.f;
	/** Pre-filter soft-clip drive 0..1 (0 = clean). Works with the filter off too. */
	float drive = 0.f;
	/** Linear gain applied to this marker's audio, 0..4 (400%) */
	float gain = 1.f;
	/** Semitones */
	float pitch = 0.f;
	/** Fine tune in semitones (-1..1) */
	float fine = 0.f;
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
	/** Table to audition instead of the morph, or -1. Switching crossfades over 20 ms. */
	int previewTable = -1;

	// Master section (stereo, on the whole output): drive -> filter. See MasterFilter. The *Mod values
	// are CV, added after smoothing so they can run at audio rate.
	/** -1 = off, else FilterMode */
	int masterMode = -1;
	float masterCutoff = 0.7f;
	float masterRes = 0.f;
	/** -1..1 = -2..+2 octaves between left and right */
	float masterSpread = 0.f;
	float masterFdbk = 0.f;
	float masterDrive = 0.f;
	/** Output gain, 0..2 (100% = unity) */
	float masterGain = 1.f;
	float masterCutoffMod = 0.f;
	float masterResMod = 0.f;
	float masterSpreadMod = 0.f;
	float masterFdbkMod = 0.f;
	float masterDriveMod = 0.f;
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
		limiter.setSampleRate(sr);
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

	/** Current (gliding) fractional table index of a marker, including its LFO. Updated for every
	marker, heard or not. */
	float getTableIndex(int marker) const {
		return tables[marker].cur;
	}

	/** How much of the morph output comes from a marker right now (0..1). */
	float getHeard(int marker) const {
		return heard[marker];
	}

	/** 0 = morph only, 1 = preview only. */
	float getPreviewLevel() const {
		return previewEnv;
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
				markerSvf.reset();
				master.reset();
			}
			structureApplied = true;
		}

		float target = std::min(std::max(in.position, 0.f), 1.f);
		posS += (target - posS) * smoothCoef;

		// Wavetable position per marker: the WAVE knob plus the marker's LFO, folded back at the
		// sample's ends. That sum is the glide target, so SMOOTH shapes the LFO too: a long SMOOTH
		// slows and softens the LFO, and at minimum it only trims the fastest motion.
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
			float t = tableIndex(fold01(in.markers[i].wave + s.lfoAmountS * in.markers[i].lfo), in.bank);
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

		float l = 0.f, r = 0.f;
		if (in.bank && gain > 0.f)
			renderMorph(in, *in.bank, &l, &r);

		// Preview (audition) of one table: replaces the morph with a 20 ms crossfade, plays at the
		// base pitch, centered, unfiltered. A different table while previewing crossfades too.
		// It fades out like the morph while a bank swap is pending.
		bool wantPreview = in.previewTable >= 0 && in.bank && in.bank == appliedBank && !in.holdSilent;
		if (wantPreview) {
			int t = std::min(in.previewTable, in.bank->numTables - 1);
			if (previewEnv <= 0.f) {
				previewCur = previewPrev = t;
				previewFade = 1.f;
			}
			else if (t != previewCur && previewFade >= 1.f) {
				previewPrev = previewCur;
				previewCur = t;
				previewFade = 0.f;
			}
		}
		previewEnv = wantPreview ? std::min(1.f, previewEnv + tableFadeStep) : std::max(0.f, previewEnv - tableFadeStep);
		previewFade = std::min(1.f, previewFade + tableFadeStep);
		if (previewEnv > 0.f && in.bank && in.bank == appliedBank) {
			const WavetableBank& bank = *in.bank;
			float f = FREQ_C4 * std::exp2(in.voct);
			f = std::min(std::max(f, 0.01f), 0.45f * sampleRate);
			int maxT = bank.numTables - 1;
			float p = readAt(bank, (float) std::min(previewCur, maxT), previewPhase, f);
			if (previewFade < 1.f) {
				float old = readAt(bank, (float) std::min(previewPrev, maxT), previewPhase, f);
				p = old + (p - old) * previewFade;
			}
			previewPhase += f / sampleRate;
			if (previewPhase >= 1.0)
				previewPhase -= std::floor(previewPhase);
			l += (p - l) * previewEnv;
			r += (p - r) * previewEnv;
		}

		// Master filter on the whole output (preview included).
		MasterParams mp;
		mp.mode = in.masterMode;
		mp.cutoff = in.masterCutoff;
		mp.res = in.masterRes;
		mp.spread = in.masterSpread;
		mp.fdbk = in.masterFdbk;
		mp.drive = in.masterDrive;
		mp.cutoffMod = in.masterCutoffMod;
		mp.resMod = in.masterResMod;
		mp.spreadMod = in.masterSpreadMod;
		mp.fdbkMod = in.masterFdbkMod;
		mp.driveMod = in.masterDriveMod;
		master.process(&l, &r, mp, smoothCoef, sampleRate);

		// Master gain (0..200%), then output protection: a look-ahead limiter at LIMIT (x5 V = 10 V), a
		// DC blocker, a NaN guard and a final clamp. See SafetyLimiter: nothing the module does upstream
		// (artifact gains, drive, resonance, feedback, master gain) can send more than 10 V out.
		masterGainS += (std::min(std::max(in.masterGain, 0.f), (float) MAX_MASTER_GAIN) - masterGainS) * smoothCoef;
		limiter.process(l * masterGainS, r * masterGainS, LIMIT, outL, outR);
	}

	/** Output gain reduction of the limiter (1 = not limiting), for a meter */
	float getLimiterGain() const {
		return limiter.getGain();
	}

private:
	/** The morph itself: neighboring markers' oscillators crossfaded by position, band-pass, pan.
	Writes the stereo signal before the limiter. */
	void renderMorph(const EngineInput& in, const WavetableBank& bank, float* outL, float* outR) {
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
			float f = FREQ_C4 * std::exp2(in.voct + (in.markers[m].pitch + in.markers[m].fine) / 12.f);
			freqs[m] = std::min(std::max(f, 0.01f), 0.45f * sampleRate);
		}

		for (int m = 0; m < NUM_MARKERS; m++)
			heard[m] = 0.f;
		heard[a] += 1.f - w;
		heard[b] += w;

		auto readMarker = [&](int m) {
			// The LFO is already inside s.cur (it moves the glide target), so SMOOTH shapes it too.
			const TableState& s = tables[m];
			playedIndex[m] = s.cur;
			float v = readAt(bank, s.cur, phases[m], freqs[m]);
			if (s.fade < 1.f) {
				float old = readAt(bank, s.prev, phases[m], freqs[m]);
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

		// Marker filter: drive, then the multimode filter. The mode mix (dry/LP/BP/HP/notch) is
		// interpolated between the two markers, so moving from an LP marker to an HP marker morphs
		// the response. A marker with CUTOFF at "Off" contributes dry. Cutoff (log) and RES are
		// interpolated between the markers whose filter is on.
		bool onA = A.cutoff > 0.f;
		bool onB = B.cutoff > 0.f;
		float mixA[NUM_MIX], mixB[NUM_MIX];
		modeWeights(onA, A.filterMode, mixA);
		modeWeights(onB, B.filterMode, mixB);
		for (int k = 0; k < NUM_MIX; k++)
			mixS[k] += (mixA[k] + (mixB[k] - mixA[k]) * w - mixS[k]) * smoothCoef;
		float cutT = (onA && onB) ? A.cutoff + (B.cutoff - A.cutoff) * w : onA ? A.cutoff : onB ? B.cutoff : cutoffS;
		float resT = (onA && onB) ? A.res + (B.res - A.res) * w : onA ? A.res : onB ? B.res : resS;
		cutoffS += (cutT - cutoffS) * smoothCoef;
		resS += (std::min(std::max(resT, 0.f), 1.f) - resS) * smoothCoef;
		float driveT = A.drive + (B.drive - A.drive) * w;
		driveS += (std::min(std::max(driveT, 0.f), 1.f) - driveS) * smoothCoef;
		float panT = A.pan + (B.pan - A.pan) * w;
		panS += (panT - panS) * smoothCoef;

		float driven = drive.process(dry, driveS < 1e-5f ? 0.f : driveS);
		float out = markerSvf.process(driven, filterHz(cutoffS), filterQ(resS), sampleRate, mixS) * gain;

		if (!std::isfinite(out)) {
			markerSvf.reset();
			return;
		}

		// Equal-power pan, unity at center.
		float angle = (panS + 1.f) * (float) (M_PI_4_F);
		*outL = out * std::cos(angle) * SQRT2;
		*outR = out * std::sin(angle) * SQRT2;
	}

	/** One sample of a fractional table index at `phase`. The band-limited level comes from `freq`:
	the brighter of the two blended levels never holds a harmonic above Nyquist (one level of
	headroom, half an octave), so there is no aliasing while the top end reaches 70-100% of Nyquist
	at every pitch. Fractional indexes blend the two neighboring tables. */
	float readAt(const WavetableBank& bank, float idx, double phase, float freq) const {
		float maxHarmonic = sampleRate / (2.f * freq);
		float lv = LEVELS_PER_OCTAVE * std::log2((FRAME_SIZE / 2) / maxHarmonic) + MIP_HEADROOM;
		lv = std::min(std::max(lv, 0.f), (float) (NUM_LEVELS - 1));
		int l0 = (int) lv;
		int l1 = std::min(l0 + 1, NUM_LEVELS - 1);
		float lf = lv - l0;

		float x = (float) (phase * FRAME_SIZE);
		int i0 = std::min((int) x, FRAME_SIZE - 1);
		int im = (i0 - 1) & (FRAME_SIZE - 1);
		int i1 = (i0 + 1) & (FRAME_SIZE - 1);
		int i2 = (i0 + 2) & (FRAME_SIZE - 1);
		float fr = x - i0;
		// Cubic (Catmull-Rom) interpolation between table samples: linear interpolation leaves
		// a grainy, slightly harsh haze of images above the harmonics, cubic does not.
		auto cubic = [&](const float* f) {
			float ym = f[im], y0 = f[i0], y1 = f[i1], y2 = f[i2];
			float a = -0.5f * ym + 1.5f * y0 - 1.5f * y1 + 0.5f * y2;
			float b = ym - 2.5f * y0 + 2.f * y1 - 0.5f * y2;
			float c = -0.5f * ym + 0.5f * y1;
			return ((a * fr + b) * fr + c) * fr + y0;
		};
		auto readTable = [&](int t) {
			float s0 = cubic(bank.frame(t, l0));
			float s1 = (l1 == l0) ? s0 : cubic(bank.frame(t, l1));
			return s0 + (s1 - s0) * lf;
		};
		int t0 = std::min(std::max((int) idx, 0), bank.numTables - 1);
		int t1 = std::min(t0 + 1, bank.numTables - 1);
		float tf = idx - t0;
		float v0 = readTable(t0);
		return (tf > 0.f && t1 != t0) ? v0 + (readTable(t1) - v0) * tf : v0;
	}

	static constexpr float SMOOTH_TIME = 0.005f;
	static constexpr float DECLICK_TIME = 0.005f;
	static constexpr float TABLE_FADE_TIME = 0.02f;
	/** AMT smoothing. Slow enough that a 0 -> 100% jump does not sweep the tables in a few ms (a
	zip), fast enough to follow the knob's 60 Hz steps without lag you would notice. */
	static constexpr float LFO_AMOUNT_SMOOTH_TIME = 0.05f;
	/** At minimum SMOOTH, moves larger than this many tables crossfade instead of gliding. */
	static constexpr float JUMP_TABLES = 2.f;
	static constexpr float FREQ_C4 = 261.6256f;
	static constexpr float SQRT2 = 1.414213562f;
	static constexpr float M_PI_4_F = 0.785398163f;
	static constexpr float MAX_GAIN = 4.f;
	/** Output ceiling in engine units; the module scales by 5 V, so this is 10 V. */
	static constexpr float LIMIT = 2.f;
	static constexpr float MAX_MASTER_GAIN = 2.f;
	/** Levels of safety above the exact band limit (see readAt) */
	static constexpr float MIP_HEADROOM = 1.f;

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

	float sampleRate = 44100.f;
	float smoothCoef = 0.f;
	float fadeStep = 0.f;
	float tableFadeStep = 0.f;
	float lfoAmountCoef = 0.f;

	double phases[NUM_MARKERS] = {};
	float posS = 0.f;
	float panS = 0.f;
	// Marker filter (one path after the crossfade; settings interpolated between the two markers)
	// Starts as the panel default (low-pass, wide open) so there is no sweep at start-up.
	float cutoffS = 1.f;
	float resS = 0.f;
	float driveS = 0.f;
	float mixS[NUM_MIX] = {0.f, 1.f, 0.f, 0.f, 0.f};
	WarmDrive drive;
	Svf markerSvf;
	MasterFilter master;
	float gainS[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	SafetyLimiter limiter;
	float masterGainS = 1.f;
	/** Declick gain (structural fades) */
	float gain = 0.f;

	bool structureApplied = false;
	uint32_t appliedSig = 0;
	const WavetableBank* appliedBank = nullptr;
	int order[NUM_MARKERS] = {0, 1, 2, 3, 4};
	int numActive = 1;
	TableState tables[NUM_MARKERS];
	float playedIndex[NUM_MARKERS] = {};
	/** Morph weight of each marker in the current output (0..1, sums to 1) */
	float heard[NUM_MARKERS] = {1.f, 0.f, 0.f, 0.f, 0.f};

	// Preview (audition)
	float previewEnv = 0.f;
	float previewFade = 1.f;
	int previewCur = 0;
	int previewPrev = 0;
	double previewPhase = 0.0;
};

} // namespace wa
