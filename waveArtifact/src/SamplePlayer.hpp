#pragma once
#include <algorithm>
#include <cmath>

#include "Lfo.hpp"
#include "WavetableBank.hpp"

// SAMPLE engine voice: plays the loaded sample as it is, one read head per marker. No Rack dependencies (tested in
// tests/test_sample.cpp).
//
// The head starts at the marker's START and runs through its slice (START to the next marker's START, or LENGTH) at
// the pitch rate: at 0 st and the sample's own rate it reads the sample frame by frame (cubic interpolation, exact on
// whole frames). Pitch changes speed, like tape. Above PLAYER_LEVEL_UP[0] (about +7 st) it reads the sample's
// decimated copies, so a high note does not alias.
//
// End of the slice: ONE stops there (a short fade, PLAYER_EDGE_SECONDS), LOOP goes back to the LOOP point with a
// crossfade (FADE: share of the slice, at least PLAYER_MIN_XFADE_SECONDS), PING turns round there. The first run
// always plays the whole slice from START (an attack, then a sustained loop). Reverse plays from the slice end
// towards START, and its loop point is the mirror one.
//
// A note fades in over PLAYER_ATTACK_SECONDS, read from that far before START, so it never starts on a step.
// Every jump of the read head is a crossfade, never a cut: a new note over a sounding one (the old head fades out),
// the loop point (equal power: the two places are unrelated), a change of decimated copy while the pitch moves
// (complementary: the two copies are the same sound). Up to PLAYER_HEADS heads sound at once while they fade.

namespace wa {

enum PlayerLoop {
	PLAYER_ONE,
	PLAYER_LOOP,
	PLAYER_PING,
	NUM_PLAYER_LOOPS
};

static const char* const PLAYER_LOOP_NAMES[NUM_PLAYER_LOOPS] = {"ONE", "LOOP", "PING"};

/** The BEAT button: the slice in free time, cut to a note value (BEAT), or played at the speed that fits it (FIT) */
enum SampleBeatMode {
	SAMPLE_BEAT_OFF,
	SAMPLE_BEAT_CUT,
	SAMPLE_BEAT_FIT,
	NUM_SAMPLE_BEAT_MODES
};

/** BEAT note values for LENGTH, longest first: the LFO's (Lfo.hpp), on down to 1/128. Its own list, so the LFO knobs
keep their steps. */
static const SyncDivision SAMPLE_DIVISIONS[] = {
	{"4/1D", 24.f, DOTTED}, {"4/1", 16.f, STRAIGHT}, {"2/1D", 12.f, DOTTED}, {"4/1T", 32.f / 3.f, TRIPLET},
	{"2/1", 8.f, STRAIGHT}, {"1/1D", 6.f, DOTTED}, {"2/1T", 16.f / 3.f, TRIPLET}, {"1/1", 4.f, STRAIGHT},
	{"1/2D", 3.f, DOTTED}, {"1/1T", 8.f / 3.f, TRIPLET}, {"1/2", 2.f, STRAIGHT}, {"1/4D", 1.5f, DOTTED},
	{"1/2T", 4.f / 3.f, TRIPLET}, {"1/4", 1.f, STRAIGHT}, {"1/8D", 0.75f, DOTTED}, {"1/4T", 2.f / 3.f, TRIPLET},
	{"1/8", 0.5f, STRAIGHT}, {"1/16D", 0.375f, DOTTED}, {"1/8T", 1.f / 3.f, TRIPLET}, {"1/16", 0.25f, STRAIGHT},
	{"1/32D", 0.1875f, DOTTED}, {"1/16T", 1.f / 6.f, TRIPLET}, {"1/32", 0.125f, STRAIGHT}, {"1/64D", 0.09375f, DOTTED},
	{"1/32T", 1.f / 12.f, TRIPLET}, {"1/64", 0.0625f, STRAIGHT}, {"1/128D", 0.046875f, DOTTED}, {"1/64T", 1.f / 24.f, TRIPLET},
	{"1/128", 0.03125f, STRAIGHT}, {"1/128T", 1.f / 48.f, TRIPLET},
};
static const int NUM_SAMPLE_DIVISIONS = sizeof(SAMPLE_DIVISIONS) / sizeof(SAMPLE_DIVISIONS[0]);

/** BEAT: the LENGTH knob (0..1, longer clockwise) as a note value: an index into SAMPLE_DIVISIONS, stepping through
the straight values (1/128 .. 4 bars) plus the triplet / dotted ones when on. */
inline int sampleBeatDivision(float knob, bool triplets, bool dotted) {
	auto enabled = [&](int i) {
		return SAMPLE_DIVISIONS[i].kind == STRAIGHT || (SAMPLE_DIVISIONS[i].kind == TRIPLET && triplets) || (SAMPLE_DIVISIONS[i].kind == DOTTED && dotted);
	};
	int n = 0;
	for (int i = 0; i < NUM_SAMPLE_DIVISIONS; i++)
		n += enabled(i) ? 1 : 0;
	// Clockwise = longer: the list runs longest first
	int k = (int) std::lround((1.f - std::min(std::max(knob, 0.f), 1.f)) * (n - 1));
	for (int i = 0; i < NUM_SAMPLE_DIVISIONS; i++) {
		if (enabled(i) && k-- == 0)
			return i;
	}
	return NUM_SAMPLE_DIVISIONS - 1;
}

/** BEAT / FIT: how many places the LOOP point can take in a slice of `beats` (from START, 0 = the whole slice): 1/128
notes, but at least 8 (so a 1/128 slice still has a loop to set) and at most 64 (a long slice: coarser steps). */
inline int sampleLoopSteps(double beats) {
	return std::min(std::max((int) std::lround(beats * 32.0), 8), 64);
}

/** Shortest loop crossfade (FADE at 0), so the loop point never clicks */
static constexpr double PLAYER_MIN_XFADE_SECONDS = 0.002;
/** ONE: the fade at the slice end */
static constexpr double PLAYER_EDGE_SECONDS = 0.002;
/** Every note fades in over this, read from this far before START: a note never starts on a step (a hit in noise,
a shaker, has none near a zero crossing), and the hit itself, from START on, plays untouched */
static constexpr double PLAYER_ATTACK_SECONDS = 0.001;
/** The choices of the note fade-in (SAMPLE_FADE_IN_PARAM) */
static constexpr double SAMPLE_FADE_IN_SECONDS[4] = {0.001, 0.003, 0.010, 0.030};
/** Rate (source frames per output sample) above which the next decimated copy is read; down again 3% lower */
static constexpr double PLAYER_LEVEL_UP[SAMPLE_LEVELS - 1] = {1.5, 3.0, 6.0};

/** What the voice plays this sample. Positions in frames of the sample's level 0. */
struct PlayerSettings {
	double start = 0.0;
	/** Last frame of the slice (start < end) */
	double end = 0.0;
	int loop = PLAYER_LOOP;
	/** LOOP / PING: where the repeated part begins, as a share of the slice from where the note starts (0 = the whole
	slice repeats). Forward it is START + share x slice; reverse is the mirror, measured back from the slice end. */
	double loopPoint = 0.0;
	/** Or the repeated part itself, frames (both >= 0: used instead of loopPoint; the engine snaps them) */
	double loopLo = -1.0, loopHi = -1.0;
	/** Direction of a new note: 1 forward, -1 reverse */
	int dir = 1;
	/** Source frames per output sample (pitch and the two sample rates) */
	double rate = 1.0;
	/** LOOP crossfade, source frames */
	double xfade = 64.0;
	/** ONE end fade, source frames */
	double edge = 64.0;
};

struct PlayerVoice {
	static const int PLAYER_HEADS = 4;

	struct Head {
		bool on = false;
		double pos = 0.0;
		int dir = 1;
		int level = 0;
		/** 0..1, and its change per output sample (> 0 fading in, < 0 fading out) */
		float fade = 1.f;
		float step = 0.f;
		/** Equal-power curve (two unrelated sounds) instead of the complementary one (the same sound) */
		bool power = false;
		/** A head a new note took over from: the note's level (envelope) when that happened, else -1. It fades out
		at that level, not at the new note's: its attack must not bring the old tail back up. */
		float envHeld = -1.f;
	};

	Head heads[PLAYER_HEADS];
	int mainHead = -1;
	/** The note's direction (PING turns the head round; the loop point stays on the note's side) */
	int noteDir = 1;

	bool sounding() const {
		for (const Head& h : heads)
			if (h.on)
				return true;
		return false;
	}

	void stop() {
		for (Head& h : heads)
			h.on = false;
		mainHead = -1;
	}

	/** Where the main head reads (frames of level 0), or -1 when it has stopped */
	double position() const {
		return (mainHead >= 0 && heads[mainHead].on) ? heads[mainHead].pos : -1.0;
	}

	/** A new note: from START (forward) or the slice end (reverse). fadeOld: the heads still sounding fade out over
	`fadeSamples` output samples instead of stopping at once. attackSamples: the new head fades in over that many
	output samples, starting that far before START (so the fade is over the quiet just before a hit, and from START on
	the sample plays as it is; a whole number of samples, so at rate 1 the head reaches START on a frame).
	envNow: the note's level (envelope) now, which the old heads keep while they fade (see process()). */
	void start(const PlayerSettings& s, bool fadeOld, float fadeSamples, float attackSamples = 0.f, float envNow = -1.f) {
		for (Head& h : heads) {
			if (!h.on)
				continue;
			if (fadeOld) {
				h.step = std::min(h.step, -1.f / std::max(fadeSamples, 1.f));
				h.power = false;
				if (h.envHeld < 0.f && envNow >= 0.f)
					h.envHeld = envNow;
			}
			else {
				h.on = false;
			}
		}
		Head* h = freeHead();
		h->on = true;
		h->dir = s.dir < 0 ? -1 : 1;
		noteDir = h->dir;
		h->pos = h->dir > 0 ? s.start : s.end;
		h->level = levelFor(s.rate, 0);
		h->fade = 1.f;
		h->step = 0.f;
		h->power = false;
		h->envHeld = -1.f;
		if (attackSamples >= 1.f) {
			// The read before START: as far as the head moves during the fade (none before the sample's start)
			double pre = (double) attackSamples * std::max(s.rate, 0.0);
			h->pos = h->dir > 0 ? std::max(s.start - pre, 0.0) : s.end + pre;
			h->fade = 0.f;
			h->step = 1.f / attackSamples;
		}
		mainHead = (int) (h - heads);
	}

	/** One output sample (before the sample's normGain), added to *l / *r. envNow: the level (envelope) the caller
	scales the whole output by after this; a head a new note took over from is scaled by its held level over it (never
	up), so it fades at the level it had. */
	void process(const SampleBuffer& S, const PlayerSettings& s, float* l, float* r, float envNow = -1.f) {
		*l = *r = 0.f;
		if (mainHead >= 0 && heads[mainHead].on)
			moveMain(s);
		for (Head& h : heads) {
			if (!h.on)
				continue;
			int lv = std::min(std::max(h.level, 0), SAMPLE_LEVELS - 1);
			const std::vector<float>& d = S.levels[lv];
			long frames = (long) S.levelFrames(lv);
			float a, b;
			readCubic(d.data(), frames, S.channels, h.pos / (double) (1 << lv), &a, &b);
			float g = curve(h);
			if (h.envHeld >= 0.f && envNow > 0.f)
				g *= std::min(h.envHeld / envNow, 1.f);
			*l += a * g;
			*r += b * g;
			h.pos += h.dir * s.rate;
			if (h.step != 0.f) {
				h.fade += h.step;
				if (h.fade >= 1.f) {
					h.fade = 1.f;
					h.step = 0.f;
				}
				else if (h.fade <= 0.f) {
					h.on = false;
				}
			}
		}
		if (mainHead >= 0 && !heads[mainHead].on)
			mainHead = -1;
	}

	/** Decimated copy for a rate, from the one in use (`from`), with 3% hysteresis */
	static int levelFor(double rate, int from) {
		int lv = std::min(std::max(from, 0), SAMPLE_LEVELS - 1);
		while (lv < SAMPLE_LEVELS - 1 && rate > PLAYER_LEVEL_UP[lv])
			lv++;
		while (lv > 0 && rate < PLAYER_LEVEL_UP[lv - 1] * 0.97)
			lv--;
		return lv;
	}

	static void readCubic(const float* d, long frames, int channels, double pos, float* l, float* r) {
		long i0 = (long) std::floor(pos);
		float f = (float) (pos - (double) i0);
		float yl[4], yr[4];
		for (int k = 0; k < 4; k++) {
			long i = i0 - 1 + k;
			if (i < 0 || i >= frames) {
				yl[k] = yr[k] = 0.f;
			}
			else if (channels == 2) {
				yl[k] = d[2 * i];
				yr[k] = d[2 * i + 1];
			}
			else {
				yl[k] = yr[k] = d[i];
			}
		}
		auto cubic = [&](const float* y) {
			float a = -0.5f * y[0] + 1.5f * y[1] - 1.5f * y[2] + 0.5f * y[3];
			float b = y[0] - 2.5f * y[1] + 2.f * y[2] - 0.5f * y[3];
			float c = -0.5f * y[0] + 0.5f * y[2];
			return ((a * f + b) * f + c) * f + y[1];
		};
		*l = cubic(yl);
		*r = (channels == 2) ? cubic(yr) : *l;
	}

private:
	static float curve(const Head& h) {
		float x = std::min(std::max(h.fade, 0.f), 1.f);
		if (h.power)
			return std::sin(x * 1.570796327f);
		return x * x * (3.f - 2.f * x);
	}

	/** A head to start: a free one, else the quietest one that is not the main head */
	Head* freeHead() {
		Head* best = nullptr;
		for (int i = 0; i < PLAYER_HEADS; i++) {
			if (!heads[i].on)
				return &heads[i];
			if (i != mainHead && (!best || heads[i].fade < best->fade))
				best = &heads[i];
		}
		return best ? best : &heads[0];
	}

	/** The main head again from `pos`, the old place fading out under it (both over `samples` output samples) */
	void jump(double pos, int dir, int level, float samples, bool power) {
		Head old = heads[mainHead];
		Head* tail = freeHead();
		if (tail == &heads[mainHead])
			return;
		*tail = old;
		tail->step = std::min(old.step, -1.f / std::max(samples, 1.f));
		tail->power = power;
		Head& h = heads[mainHead];
		h.pos = pos;
		h.dir = dir;
		h.level = level;
		h.fade = 0.f;
		h.step = 1.f / std::max(samples, 1.f);
		h.power = power;
	}

	/** The main head's slice end, loop and decimated copy */
	void moveMain(const PlayerSettings& s) {
		Head& h = heads[mainHead];
		double start = s.start, end = std::max(s.end, s.start + 1.0);
		double len = end - start;
		double rate = std::max(s.rate, 1e-6);
		int lv = levelFor(rate, h.level);
		if (lv != h.level && h.step == 0.f) {
			// Same place, other copy: a short complementary crossfade (the two are the same sound).
			jump(h.pos, h.dir, lv, 240.f, false);
			return;
		}
		// Frames left before the end the head is moving to
		double left = h.dir > 0 ? end - h.pos : h.pos - start;
		// The repeated part: [loop point, end] for a forward note, its mirror [start, loop point] for a reverse one
		double lp = std::min(std::max(s.loopPoint, 0.0), 0.99) * len;
		double lo = noteDir > 0 ? start + lp : start, hi = noteDir > 0 ? end : end - lp;
		if (s.loopLo >= 0.0 && s.loopHi > s.loopLo) {
			lo = std::min(std::max(s.loopLo, start), end - 1.0);
			hi = std::min(std::max(s.loopHi, lo + 1.0), end);
		}
		double loopLen = std::max(hi - lo, 1.0);
		if (s.loop == PLAYER_PING) {
			if (h.dir > 0 && h.pos >= hi) {
				h.pos = std::max(2.0 * hi - h.pos, lo);
				h.dir = -1;
			}
			else if (h.dir < 0 && h.pos <= lo) {
				h.pos = std::min(2.0 * lo - h.pos, hi);
				h.dir = 1;
			}
			// A head left outside the part (a forward note's first run, or the loop point moved): it plays on until
			// it reaches the part's far end, then turns there as above.
		}
		else if (s.loop == PLAYER_LOOP) {
			double x = std::min(std::max(s.xfade, 1.0), 0.5 * loopLen);
			if (left <= x && h.step >= 0.f && h.fade >= 1.f) {
				// Past the end already (START or LENGTH moved): a whole crossfade from here.
				float samples = (float) ((left > 1.0 ? left : x) / rate);
				jump(h.dir > 0 ? lo : hi, h.dir, h.level, samples, true);
			}
		}
		else {
			// ONE: fade out over the last frames, then stop.
			double e = std::min(std::max(s.edge, 1.0), 0.5 * len);
			if (left <= e && h.step >= 0.f) {
				h.step = -h.fade / (float) std::max(left / rate, 1.0);
				h.power = false;
			}
		}
	}
};

} // namespace wa
