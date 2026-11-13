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
// Each artifact can also have its own output: its chain alone, before the master, at its share of
// the position crossfade (renderDirect).
//
// The morph is audio-only: pitch is NOT interpolated between markers, so a marker at -12 st and
// a marker at 0 st crossfade as two separate pitches, never sliding (glide could be a later option).
//
// POLY play mode (EngineInput::poly) replaces the morph: every marker runs its own whole chain at the level of
// its ADSR (see PolyVoices.hpp), with nothing interpolated between markers, and the voices add up.
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
	/** Filter cutoff 20 Hz * 1000^cutoff (0 = 20 Hz, 1 = 20 kHz, wide open). The filter is always on. */
	float cutoff = 1.f;
	/** FilterMode: LP, BP, HP, notch */
	int filterMode = MODE_LP;
	/** Resonance 0..1: Q 0.7 (wide) .. 20 (narrow, ringing) */
	float res = 0.f;
	/** Pre-filter soft-clip drive 0..1 (0 = clean). Works with the filter off too. */
	float drive = 0.f;
	/** Linear gain applied to this marker's audio, 0..4 (400%) */
	float gain = 1.f;
	/** Semitones (-36..36 on the panel) */
	float pitch = 0.f;
	/** Fine tune in semitones (-1..1) */
	float fine = 0.f;
	/** Extra octaves for this artifact only (expander V/OCT), on top of the main V/OCT */
	float voct = 0.f;
	/** -1 (left) .. 1 (right) */
	float pan = 0.f;
};

struct EngineInput {
	MarkerInput markers[NUM_MARKERS];
	/** 0..1 along the bar */
	float position = 0.f;
	/** Octaves relative to C4 */
	float voct = 0.f;
	/** The main sample's bank: every marker without its own plays it */
	const WavetableBank* bank = nullptr;
	/** A marker's own sample (null: the main one). Changing it fades that marker alone out and in (5 ms). */
	const WavetableBank* markerBank[NUM_MARKERS] = {};
	/** Fade this marker alone out and keep it silent (its own sample is about to be swapped) */
	bool markerHold[NUM_MARKERS] = {};
	/** The bank the preview's table is in (the file the screen shows), or null */
	const WavetableBank* previewBank = nullptr;
	/** Fade out and stay silent, e.g. while a bank swap is pending. */
	bool holdSilent = false;
	/** Table to audition instead of the morph, or -1. Switching crossfades over 20 ms. */
	int previewTable = -1;
	/** Measuring output (tests): render each artifact's own chain for getDirect. The module never sets it (its old
	ARTIFACT OUT jacks became SEND / RETURN), so it costs nothing there. */
	bool wantDirect[NUM_MARKERS] = {};
	/** SEND / RETURN (an expander's jacks): render each marker's send (getSend) */
	bool wantSend = false;
	/** The marker's filter is replaced by the returned signal (engine units, retR = retL when mono) */
	bool insert[NUM_MARKERS] = {};
	float retL[NUM_MARKERS] = {};
	float retR[NUM_MARKERS] = {};
	/** POLY play mode: the voices below instead of the morph. Switching fades like a structural change. */
	bool poly = false;
	/** Level of each marker's ADSR, 0..1 */
	float voiceEnv[NUM_MARKERS] = {};
	/** The marker started a new note on this sample */
	bool voiceStart[NUM_MARKERS] = {};

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
		master.setSampleRate(sr);
		for (int m = 0; m < NUM_MARKERS; m++) {
			direct[m].limiter.setSampleRate(sr);
			direct[m].running = false;
		}
	}

	float getSampleRate() const {
		return sampleRate;
	}

	/** True once the declick fade has reached silence. */
	bool isSilent() const {
		return gain <= 0.f;
	}

	/** True once a marker's own fade (markerHold, or its sample changing) has reached silence */
	bool isMarkerSilent(int m) const {
		return gain <= 0.f || markerGate[m] <= 0.f;
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
		for (int m = 0; m < NUM_MARKERS; m++)
			rawValid[m] = false;

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
		if (in.poly)
			sig |= 0x80000000u;

		bool changed = !structureApplied || sig != appliedSig || in.bank != appliedBank;
		// Silent without any sample at all; a marker without one stays silent on its own (markerGate)
		bool anyBank = in.bank != nullptr;
		for (int i = 0; i < NUM_MARKERS && !anyBank; i++)
			anyBank = in.markerBank[i] != nullptr;
		bool wantSilent = in.holdSilent || changed || !anyBank;
		gain = wantSilent ? std::max(0.f, gain - fadeStep) : std::min(1.f, gain + fadeStep);

		if (changed && gain <= 0.f) {
			appliedSig = sig;
			numActive = n;
			std::copy(ord, ord + n, order);
			if (in.poly != polyApplied) {
				polyApplied = in.poly;
				drive.reset();
				markerSvf.reset();
				// The chains start clean in the other mode.
				for (int m = 0; m < NUM_MARKERS; m++)
					direct[m].running = false;
			}
			if (in.bank != appliedBank) {
				// appliedBank is only an identity here; it may already be freed. Never read it.
				appliedBank = in.bank;
				for (int i = 0; i < NUM_MARKERS; i++) {
					appliedMarkerBank[i] = bankFor(in, i);
					float t = tableIndex(in.markers[i].wave, appliedMarkerBank[i]);
					tables[i].cur = tables[i].prev = tables[i].lastTarget = t;
					tables[i].fade = 1.f;
				}
				markerSvf.reset();
				master.reset();
			}
			structureApplied = true;
		}

		// Each marker's own sample: a change (or markerHold) fades that marker alone out, then it starts on the new
		// bank. While the whole engine is silent the change applies at once.
		for (int i = 0; i < NUM_MARKERS; i++) {
			const WavetableBank* want = bankFor(in, i);
			bool mChanged = want != appliedMarkerBank[i];
			bool mSilent = in.markerHold[i] || mChanged || !want;
			markerGate[i] = mSilent ? std::max(0.f, markerGate[i] - fadeStep) : std::min(1.f, markerGate[i] + fadeStep);
			if (mChanged && (markerGate[i] <= 0.f || gain <= 0.f)) {
				// appliedMarkerBank is only an identity; it may already be freed. Never read it.
				appliedMarkerBank[i] = want;
				float t = tableIndex(in.markers[i].wave, want);
				tables[i].cur = tables[i].prev = tables[i].lastTarget = t;
				tables[i].fade = 1.f;
			}
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
			float t = tableIndex(fold01(in.markers[i].wave + s.lfoAmountS * in.markers[i].lfo), bankFor(in, i));
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

		// RETURN: each marker's filter gives way to the returned sound over 20 ms (and back), so patching or
		// unpatching a return never clicks. The sends are written again by whatever renders below.
		for (int m = 0; m < NUM_MARKERS; m++) {
			float t = in.insert[m] ? 1.f : 0.f;
			insertS[m] = (t > insertS[m]) ? std::min(t, insertS[m] + tableFadeStep) : std::max(t, insertS[m] - tableFadeStep);
			sendL[m] = sendR[m] = 0.f;
		}

		float l = 0.f, r = 0.f;
		bool rendered = anyBank && gain > 0.f;
		if (rendered) {
			if (polyApplied)
				renderPoly(in, &l, &r);
			else
				renderMorph(in, &l, &r);
		}
		// MORPH: the artifacts' own chains (outputs, SEND, RETURN); a returned sound joins the mix here, before the
		// master section.
		if (!polyApplied)
			renderChains(in, &l, &r);

		// Preview (audition) of one table: replaces the morph with a 20 ms crossfade, plays at the
		// base pitch, centered, unfiltered. A different table while previewing crossfades too.
		// It fades out like the morph while a bank swap is pending.
		// The preview's file: the one given, else the main sample
		const WavetableBank* pb = in.previewBank ? in.previewBank : in.bank;
		bool wantPreview = in.previewTable >= 0 && pb && !in.holdSilent;
		if (wantPreview) {
			int t = std::min(in.previewTable, pb->numTables - 1);
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
		if (previewEnv > 0.f && pb) {
			const WavetableBank& bank = *pb;
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

		if (polyApplied && !rendered) {
			// Fully faded: the limiters look ahead, so they still hold the last faded samples.
			for (int m = 0; m < NUM_MARKERS; m++) {
				if (in.wantDirect[m])
					direct[m].limiter.process(0.f, 0.f, LIMIT, &directL[m], &directR[m]);
				else
					directL[m] = directR[m] = 0.f;
			}
		}
	}

	/** An artifact's own output (engine units, x5 V), 0 unless `wantDirect` was set for it.
	It is the artifact's whole chain (oscillator, GAIN, DRIVE, filter, PAN) before the master
	section, after the position: it carries the artifact's share of the crossfade (full on its
	marker, fading toward the neighbors, silent elsewhere), with its own 10 V output protection. */
	void getDirect(int m, float* l, float* r) const {
		*l = directL[m];
		*r = directR[m];
	}

	/** SEND: what this marker's filter gets this sample (engine units, x5 V), after GAIN and DRIVE; in MORPH at
	its position share. 0 when it does not sound. */
	void getSend(int m, float* l, float* r) const {
		*l = sendL[m];
		*r = sendR[m];
	}

	/** Output gain reduction of the limiter (1 = not limiting), for a meter */
	float getLimiterGain() const {
		return limiter.getGain();
	}

private:
	/** The morph itself: neighboring markers' oscillators crossfaded by position, band-pass, pan.
	Writes the stereo signal before the limiter. */
	void renderMorph(const EngineInput& in, float* outL, float* outR) {
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
			float f = FREQ_C4 * std::exp2(in.voct + in.markers[m].voct + (in.markers[m].pitch + in.markers[m].fine) / 12.f);
			freqs[m] = std::min(std::max(f, 0.01f), 0.45f * sampleRate);
		}

		for (int m = 0; m < NUM_MARKERS; m++)
			heard[m] = 0.f;
		heard[a] += 1.f - w;
		heard[b] += w;
		// The position crossfade share, before patched artifact outputs take their part out of the mix.
		std::copy(heard, heard + NUM_MARKERS, posWeight);

		auto readMarker = [&](int m) {
			// The LFO is already inside s.cur (it moves the glide target), so SMOOTH shapes it too.
			const TableState& s = tables[m];
			playedIndex[m] = s.cur;
			// Its own sample's tables (or the main one's), through its own fade
			const WavetableBank* bk = bankFor(in, m);
			if (!bk || markerGate[m] <= 0.f)
				return 0.f;
			float v = readAt(*bk, s.cur, phases[m], freqs[m]);
			if (s.fade < 1.f) {
				float old = readAt(*bk, s.prev, phases[m], freqs[m]);
				v = old + (v - old) * s.fade;
			}
			return v * markerGate[m];
		};
		for (int m = 0; m < NUM_MARKERS; m++) {
			// (float) copy: std::min takes references, and C++11 has no inline constexpr members.
			float g = std::min(std::max(in.markers[m].gain, 0.f), (float) MAX_GAIN);
			gainS[m] += (g - gainS[m]) * smoothCoef;
		}
		// An artifact with a patched output leaves the main mix: its share fades to 0 (20 ms), so its
		// sound comes only from its own jacks. The morph keeps its markers; over a separated one the
		// main output simply goes quiet.
		for (int m = 0; m < NUM_MARKERS; m++) {
			float t = in.wantDirect[m] ? 0.f : 1.f;
			mainShare[m] = (t > mainShare[m]) ? std::min(t, mainShare[m] + tableFadeStep) : std::max(t, mainShare[m] - tableFadeStep);
		}
		// A marker with a RETURN leaves the morph's voice (its filter is outside): renderChains plays it, through
		// the return, into the main mix.
		heard[a] *= mainShare[a] * (1.f - insertS[a]);
		if (b != a)
			heard[b] *= mainShare[b] * (1.f - insertS[b]);
		float ra = readMarker(a);
		float rb = (b == a) ? ra : readMarker(b);
		float va = ra * gainS[a] * mainShare[a] * (1.f - insertS[a]);
		float vb = rb * gainS[b] * mainShare[b] * (1.f - insertS[b]);
		// Artifacts with their own chain (a patched output, a SEND or a RETURN) are read every sample, heard in the
		// morph or not.
		for (int m = 0; m < NUM_MARKERS; m++) {
			rawValid[m] = needsChain(in, m);
			if (rawValid[m])
				rawRead[m] = (m == a) ? ra : (m == b) ? rb : readMarker(m);
		}
		float dry = va + (vb - va) * w;

		for (int m = 0; m < NUM_MARKERS; m++) {
			phases[m] += freqs[m] / sampleRate;
			if (phases[m] >= 1.0)
				phases[m] -= std::floor(phases[m]);
		}

		// Marker filter: drive, then the multimode filter. The mode mix (dry/LP/BP/HP/notch) is
		// interpolated between the two markers, so moving from an LP marker to an HP marker morphs
		// the response. The filter is always on: CUTOFF fully left is 20 Hz, fully right is wide
		// open. Cutoff (log) and RES are interpolated between the two markers.
		float mixA[NUM_MIX], mixB[NUM_MIX];
		modeWeights(true, A.filterMode, mixA);
		modeWeights(true, B.filterMode, mixB);
		for (int k = 0; k < NUM_MIX; k++)
			mixS[k] += (mixA[k] + (mixB[k] - mixA[k]) * w - mixS[k]) * smoothCoef;
		float cutT = std::min(std::max(A.cutoff + (B.cutoff - A.cutoff) * w, 0.f), 1.f);
		float resT = A.res + (B.res - A.res) * w;
		cutoffS += (cutT - cutoffS) * smoothCoef;
		resS += (std::min(std::max(resT, 0.f), 1.f) - resS) * smoothCoef;
		float driveT = A.drive + (B.drive - A.drive) * w;
		driveS += (std::min(std::max(driveT, 0.f), 1.f) - driveS) * smoothCoef;
		float panT = A.pan + (B.pan - A.pan) * w;
		panS += (panT - panS) * smoothCoef;

		float driven = drive.process(dry, driveS < 1e-5f ? 0.f : driveS);
		float q;
		float fc = markerParams.get(cutoffS, resS, &q);
		float out = markerSvf.process(driven, fc, q, sampleRate, mixS) * gain;

		if (!std::isfinite(out)) {
			markerSvf.reset();
			return;
		}

		// Equal-power pan, unity at center.
		float angle = (panS + 1.f) * (float) (M_PI_4_F);
		*outL = out * std::cos(angle) * SQRT2;
		*outR = out * std::sin(angle) * SQRT2;
	}

	/** POLY: every sounding marker runs its own chain (the same blocks as an artifact output): GAIN, DRIVE,
	filter, ADSR level, PAN. The level comes after the filter, so a released note keeps the filter's sound to
	the end. A patched artifact output takes its voice out of the main mix, as in the morph. */
	void renderPoly(const EngineInput& in, float* outL, float* outR) {
		float sumL = 0.f, sumR = 0.f;
		float power = 0.f;
		for (int m = 0; m < NUM_MARKERS; m++) {
			const MarkerInput& M = in.markers[m];
			DirectChain& d = direct[m];
			float f = FREQ_C4 * std::exp2(in.voct + M.voct + (M.pitch + M.fine) / 12.f);
			f = std::min(std::max(f, 0.01f), 0.45f * sampleRate);
			float g = std::min(std::max(M.gain, 0.f), (float) MAX_GAIN);
			gainS[m] += (g - gainS[m]) * smoothCoef;
			float t = in.wantDirect[m] ? 0.f : 1.f;
			mainShare[m] = (t > mainShare[m]) ? std::min(t, mainShare[m] + tableFadeStep) : std::max(t, mainShare[m] - tableFadeStep);
			float env = std::min(std::max(in.voiceEnv[m], 0.f), 1.f);
			heard[m] = env;
			posWeight[m] = env;
			power += env * env * mainShare[m];
			// A note from silence always starts the same way. One over a sounding voice keeps its phase.
			if (in.voiceStart[m] && !d.running)
				phases[m] = 0.0;

			float yl = 0.f, yr = 0.f;
			if (env > 0.f) {
				float target[NUM_MIX];
				modeWeights(true, M.filterMode, target);
				float cutT = std::min(std::max(M.cutoff, 0.f), 1.f);
				float resT = std::min(std::max(M.res, 0.f), 1.f);
				float driveT = std::min(std::max(M.drive, 0.f), 1.f);
				float panT = std::min(std::max(M.pan, -1.f), 1.f);
				if (!d.running) {
					std::copy(target, target + NUM_MIX, d.mixS);
					d.cutS = cutT;
					d.resS = resT;
					d.driveS = driveT;
					d.panS = panT;
					d.drive.reset();
					d.svf.reset();
					d.running = true;
				}
				for (int k = 0; k < NUM_MIX; k++)
					d.mixS[k] += (target[k] - d.mixS[k]) * smoothCoef;
				d.cutS += (cutT - d.cutS) * smoothCoef;
				d.resS += (resT - d.resS) * smoothCoef;
				d.driveS += (driveT - d.driveS) * smoothCoef;
				d.panS += (panT - d.panS) * smoothCoef;

				const TableState& s = tables[m];
				playedIndex[m] = s.cur;
				const WavetableBank* bk = bankFor(in, m);
				float v = 0.f;
				if (bk && markerGate[m] > 0.f) {
					v = readAt(*bk, s.cur, phases[m], f);
					if (s.fade < 1.f) {
						float old = readAt(*bk, s.prev, phases[m], f);
						v = old + (v - old) * s.fade;
					}
					v *= markerGate[m];
				}
				// SEND: what the filter gets; RETURN (insert) takes the filter's place, faded in and out over 20 ms
				float driven = d.drive.process(v * gainS[m], d.driveS < 1e-5f ? 0.f : d.driveS);
				sendL[m] = sendR[m] = driven;
				float k = insertS[m];
				float y = 0.f;
				if (k < 1.f) {
					float q;
					float fc = d.params.get(d.cutS, d.resS, &q);
					y = d.svf.process(driven, fc, q, sampleRate, d.mixS);
					if (!std::isfinite(y)) {
						d.svf.reset();
						y = 0.f;
					}
				}
				y *= env * gain;
				float angle = (d.panS + 1.f) * (float) (M_PI_4_F);
				float cl = std::cos(angle) * SQRT2, cr = std::sin(angle) * SQRT2;
				yl = y * cl;
				yr = y * cr;
				if (k > 0.f) {
					yl += (in.retL[m] * env * gain * cl - yl) * k;
					yr += (in.retR[m] * env * gain * cr - yr) * k;
				}
			}
			else {
				d.running = false;
			}
			phases[m] += f / sampleRate;
			if (phases[m] >= 1.0)
				phases[m] -= std::floor(phases[m]);

			sumL += yl * mainShare[m];
			sumR += yr * mainShare[m];
			if (in.wantDirect[m])
				d.limiter.process(yl, yr, LIMIT, &directL[m], &directR[m]);
			else
				directL[m] = directR[m] = 0.f;
		}
		// One or two voices play at full level. More share the headroom, so a big chord is not all squeezed by
		// the limiter (full-level tables: 8 dB of limiting with five voices without this, see tests/test_poly.cpp).
		float trim = (power > POLY_FULL_VOICES) ? std::sqrt(POLY_FULL_VOICES / power) : 1.f;
		polyTrimS += (trim - polyTrimS) * smoothCoef;
		*outL = sumL * polyTrimS;
		*outR = sumR * polyTrimS;
	}

	/** A marker that runs its own chain in the morph: a patched artifact output, a SEND, or a RETURN */
	bool needsChain(const EngineInput& in, int m) const {
		return in.wantDirect[m] || in.wantSend || insertS[m] > 0.f;
	}

	/** MORPH: each artifact's own chain, for its output, its SEND and its RETURN. Its own settings (no
	interpolation with a neighbor): GAIN, DRIVE, filter, its position share (posWeight, so it follows the playhead
	like the main mix), PAN; the output then goes through a safety limiter. It fades (5 ms) when the artifact is
	switched off, and with the structural declick when the bank changes.
	SEND is the marker's part of what the morph's filter gets (after DRIVE, at its position share). With a RETURN,
	the returned sound takes the filter's place (faded over 20 ms) and goes into the main mix (`l`, `r`, before the
	master section), as the marker has left the morph's voice. */
	void renderChains(const EngineInput& in, float* l, float* r) {
		for (int m = 0; m < NUM_MARKERS; m++) {
			DirectChain& d = direct[m];
			if (!needsChain(in, m)) {
				d.running = false;
				directL[m] = directR[m] = 0.f;
				continue;
			}
			if (gain <= 0.f && d.running) {
				// Fully faded (STOP, bank swap): freeze drive and filter like the main chain, whose
				// phases also stop, so the sound resumes exactly where it paused instead of refilling
				// an emptied filter. The limiter keeps running on silence: it looks ahead, so it still
				// holds the last few faded samples and must let them out.
				if (in.wantDirect[m])
					d.limiter.process(0.f, 0.f, LIMIT, &directL[m], &directR[m]);
				else
					directL[m] = directR[m] = 0.f;
				continue;
			}
			const MarkerInput& M = in.markers[m];
			float target[NUM_MIX];
			modeWeights(true, M.filterMode, target);
			float cutT = std::min(std::max(M.cutoff, 0.f), 1.f);
			float resT = std::min(std::max(M.res, 0.f), 1.f);
			float driveT = std::min(std::max(M.drive, 0.f), 1.f);
			float panT = std::min(std::max(M.pan, -1.f), 1.f);
			if (!d.running) {
				// Just patched: start from the current settings, silent, with clean filter memory.
				std::copy(target, target + NUM_MIX, d.mixS);
				d.cutS = cutT;
				d.resS = resT;
				d.driveS = driveT;
				d.panS = panT;
				d.env = 0.f;
				d.drive.reset();
				d.svf.reset();
				d.limiter.reset();
				d.running = true;
			}
			for (int k = 0; k < NUM_MIX; k++)
				d.mixS[k] += (target[k] - d.mixS[k]) * smoothCoef;
			d.cutS += (cutT - d.cutS) * smoothCoef;
			d.resS += (resT - d.resS) * smoothCoef;
			d.driveS += (driveT - d.driveS) * smoothCoef;
			d.panS += (panT - d.panS) * smoothCoef;
			d.env += ((M.active ? 1.f : 0.f) - d.env) * smoothCoef;

			float x = rawValid[m] ? rawRead[m] * gainS[m] : 0.f;
			float driven = d.drive.process(x, d.driveS < 1e-5f ? 0.f : d.driveS);
			float share = d.env * posWeight[m];
			sendL[m] = sendR[m] = driven * share;
			float k = insertS[m];
			float y = 0.f;
			if (k < 1.f) {
				float q;
				float fc = d.params.get(d.cutS, d.resS, &q);
				y = d.svf.process(driven, fc, q, sampleRate, d.mixS);
				if (!std::isfinite(y)) {
					d.svf.reset();
					y = 0.f;
				}
			}
			y *= share * gain;
			float angle = (d.panS + 1.f) * (float) (M_PI_4_F);
			float cl = std::cos(angle) * SQRT2, cr = std::sin(angle) * SQRT2;
			// The return already carries the position share (the send did)
			float el = in.retL[m] * gain * cl, er = in.retR[m] * gain * cr;
			float yl = y * cl + (el - y * cl) * k, yr = y * cr + (er - y * cr) * k;
			if (in.wantDirect[m])
				d.limiter.process(yl, yr, LIMIT, &directL[m], &directR[m]);
			else
				directL[m] = directR[m] = 0.f;
			*l += el * k * mainShare[m];
			*r += er * k * mainShare[m];
		}
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
	/** POLY: voices (as summed power) that play at full level before they share the headroom */
	static constexpr float POLY_FULL_VOICES = 2.f;

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

	/** The bank a marker plays: its own sample's, else the main one (null: none) */
	static const WavetableBank* bankFor(const EngineInput& in, int m) {
		return in.markerBank[m] ? in.markerBank[m] : in.bank;
	}

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
	/** CUTOFF / RES as Hz and Q, worked out again only when they move */
	FilterParams markerParams;
	MasterFilter master;
	float gainS[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	SafetyLimiter limiter;
	float masterGainS = 1.f;

	// Artifact outputs (see renderDirect)
	struct DirectChain {
		WarmDrive drive;
		Svf svf;
		FilterParams params;
		SafetyLimiter limiter;
		float mixS[NUM_MIX] = {0.f, 1.f, 0.f, 0.f, 0.f};
		float cutS = 1.f;
		float resS = 0.f;
		float driveS = 0.f;
		float panS = 0.f;
		/** 0..1: fades the output when the artifact is switched off */
		float env = 0.f;
		bool running = false;
	};
	DirectChain direct[NUM_MARKERS];
	float directL[NUM_MARKERS] = {};
	float directR[NUM_MARKERS] = {};
	/** This sample's oscillator read of each artifact with its own chain (before GAIN) */
	float rawRead[NUM_MARKERS] = {};
	bool rawValid[NUM_MARKERS] = {};
	/** SEND / RETURN: each marker's send this sample, and how far its return has replaced its filter (0..1) */
	float sendL[NUM_MARKERS] = {};
	float sendR[NUM_MARKERS] = {};
	float insertS[NUM_MARKERS] = {};
	/** Declick gain (structural fades) */
	float gain = 0.f;

	bool structureApplied = false;
	bool polyApplied = false;
	float polyTrimS = 1.f;
	uint32_t appliedSig = 0;
	const WavetableBank* appliedBank = nullptr;
	/** Each marker's bank as applied (identity only) and its own fade, 0..1 */
	const WavetableBank* appliedMarkerBank[NUM_MARKERS] = {};
	float markerGate[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};
	int order[NUM_MARKERS] = {0, 1, 2, 3, 4};
	int numActive = 1;
	TableState tables[NUM_MARKERS];
	float playedIndex[NUM_MARKERS] = {};
	/** Morph weight of each marker in the current output (0..1, sums to 1) */
	float heard[NUM_MARKERS] = {1.f, 0.f, 0.f, 0.f, 0.f};
	/** Position crossfade weight of each marker (heard before patched outputs leave the mix) */
	float posWeight[NUM_MARKERS] = {1.f, 0.f, 0.f, 0.f, 0.f};
	/** Each artifact's share of the main mix: 0 while its own output is patched (see renderMorph) */
	float mainShare[NUM_MARKERS] = {1.f, 1.f, 1.f, 1.f, 1.f};

	// Preview (audition)
	float previewEnv = 0.f;
	float previewFade = 1.f;
	int previewCur = 0;
	int previewPrev = 0;
	double previewPhase = 0.0;
};

} // namespace wa
