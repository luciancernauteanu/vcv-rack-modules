#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// "Lay out the loop on the row" (SAMPLE + the step sequencer): turns a loop into a row of trigs that plays it back as
// it is. Each trig starts on a hit (START locked exactly there) and plays up to the next trig (LENGTH locked to the
// gap), so the row is the loop in pieces that can be edited, at any tempo (slower: small gaps; faster: each slice is
// cut by the next, like REX loops). No Rack dependency (tested in tests/test_sample.cpp).

namespace wa {

/** The tempo written in a file name ("RainShake-70bpm", "Loop 128 BPM", "bpm_90"), or 0 when there is none.
Only a number right next to "bpm" counts, so keys and take numbers are not taken for a tempo. */
inline double bpmFromName(const std::string& name) {
	std::string s = name;
	for (char& c : s)
		c = (char) std::tolower((unsigned char) c);
	auto inRange = [](double v) { return v >= 30.0 && v <= 300.0; };
	for (size_t at = s.find("bpm"); at != std::string::npos; at = s.find("bpm", at + 3)) {
		// Before: "70bpm", "70 bpm", "70_bpm", "70-bpm"
		size_t i = at;
		while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '_' || s[i - 1] == '-'))
			i--;
		size_t end = i;
		while (i > 0 && (std::isdigit((unsigned char) s[i - 1]) || s[i - 1] == '.'))
			i--;
		if (i < end) {
			double v = std::atof(s.substr(i, end - i).c_str());
			if (inRange(v))
				return v;
		}
		// After: "bpm120", "bpm 120", "bpm_120"
		size_t j = at + 3;
		while (j < s.size() && (s[j] == ' ' || s[j] == '_' || s[j] == '-'))
			j++;
		size_t from = j;
		while (j < s.size() && (std::isdigit((unsigned char) s[j]) || s[j] == '.'))
			j++;
		if (j > from) {
			double v = std::atof(s.substr(from, j - from).c_str());
			if (inRange(v))
				return v;
		}
	}
	return 0.0;
}

/** The longest loop the layout takes, in beats (16 steps of /8 = 128 beats) */
static const int LAYOUT_MAX_BEATS = 128;

/** How many beats a loop of `seconds` lasts. With a tempo in its file name (`nameBpm` > 0): at that tempo. Otherwise
the power of two (1 .. 128 beats) whose tempo is closest to `tempo` (the module's), so a 3.4 s loop is 8 beats
(140 bpm) at 120 and 4 beats (70 bpm) at 80. */
inline int guessLoopBeats(double seconds, double nameBpm, double tempo) {
	if (seconds <= 0.0)
		return 1;
	if (nameBpm > 0.0)
		return std::min(std::max((int) std::lround(seconds * nameBpm / 60.0), 1), LAYOUT_MAX_BEATS);
	int best = 1;
	double bestD = 1e9;
	for (int b = 1; b <= LAYOUT_MAX_BEATS; b *= 2) {
		double d = std::fabs(std::log(60.0 * b / seconds / std::max(tempo, 1.0)));
		if (d < bestD) {
			bestD = d;
			best = b;
		}
	}
	return best;
}

// --- The tempo, found in the audio ------------------------------------------------------------------------------
// From the sample's onset strength curve (Transients.hpp onsetStrength: how much the level rises, every hop), so
// loud attacks weigh more than soft shakes. No file name needed.

/** Tempos looked for (an octave and more, so every tempo has one of its octaves in it) */
static constexpr double TEMPO_MIN_BPM = 60.0, TEMPO_MAX_BPM = 200.0;
/** Below this a beat grid is too little stronger than the rest of the curve: no clear beat */
static constexpr double TEMPO_MIN_SCORE = 0.5;

/** A tempo found in the audio */
struct FoundTempo {
	bool found = false;
	double bpm = 0.0;
	/** How much stronger the attacks are on its grid than on average (0: no stronger; 1: twice as strong) */
	double score = 0.0;
	/** A loop: its length in beats at that tempo */
	int beats = 0;
};

namespace tempodetail {
/** The curve at a fractional hop (linear), 0 outside */
inline double at(const std::vector<float>& e, double pos) {
	if (pos < 0.0)
		return 0.0;
	size_t i = (size_t) pos;
	if (i + 1 >= e.size())
		return i < e.size() ? e[i] : 0.0;
	double f = pos - (double) i;
	return e[i] + (e[i + 1] - e[i]) * f;
}
inline double meanOf(const std::vector<float>& e, long a, long b) {
	a = std::max(a, 0L);
	b = std::min(b, (long) e.size());
	if (b <= a)
		return 0.0;
	double s = 0.0;
	for (long i = a; i < b; i++)
		s += e[(size_t) i];
	return s / (double) (b - a);
}
/** How much stronger the curve is on a beat grid (`beats` beats of `period` hops from `start`) than `mean`: straight
(beats, 8ths, 16ths: weights 1, 1/2, 1/4) or swung (beats 2, the swung 8th at 2/3 of the beat 1), the better one.
0: no stronger than anywhere. */
inline double gridScore(const std::vector<float>& e, double start, double period, int beats, double mean) {
	if (beats <= 0 || mean <= 0.0)
		return 0.0;
	double mb = 0.0, m8 = 0.0, m16 = 0.0, m3 = 0.0;
	for (int k = 0; k < beats; k++) {
		double p = start + k * period;
		mb += at(e, p);
		m8 += at(e, p + 0.5 * period);
		m16 += 0.5 * (at(e, p + 0.25 * period) + at(e, p + 0.75 * period));
		m3 += at(e, p + period * 2.0 / 3.0);
	}
	double straight = (mb + 0.5 * m8 + 0.25 * m16) / (1.75 * beats), swung = (2.0 * mb + m3) / (3.0 * beats);
	return std::max(straight, swung) / mean - 1.0;
}
} // namespace tempodetail

/** Of `bpm` and its octaves (x2, /2 …) inside the range, the one nearest to `prefer` */
inline double nearestOctave(double bpm, double prefer) {
	double best = bpm, bestD = 1e9;
	for (int k = -4; k <= 4; k++) {
		double b = bpm * std::pow(2.0, k);
		if (b < TEMPO_MIN_BPM - 1e-9 || b > TEMPO_MAX_BPM + 1e-9)
			continue;
		double d = std::fabs(std::log(b / std::max(prefer, 1.0)));
		if (d < bestD) {
			bestD = d;
			best = b;
		}
	}
	return best;
}

/** The two tempos are the same up to octaves (within 2 %): a file name's tempo against the one found */
inline bool sameTempo(double a, double b) {
	if (a <= 0.0 || b <= 0.0)
		return false;
	double r = std::log2(a / b);
	return std::fabs(r - std::round(r)) < 0.03;
}

/** The tempo of a loop (`from` .. `from + frames`, frames of the sample; `env` its onset strength, one value per
`hop` frames): a loop lasts a whole number of beats, so each count of beats whose tempo is in the range is scored by
how much stronger the curve is on its grid (from the loop's start) than on average. Of the best one and its octaves
(whole beats only), the tempo nearest to `prefer` (the module's): 70 and 140 fit the same loop, and 140 lays it out
nearer to how it sounds at 120. Not found: no grid clearly stronger. */
inline FoundTempo loopTempo(const std::vector<float>& env, int hop, long from, long frames, double sampleRate, double prefer) {
	FoundTempo t;
	if (env.empty() || hop <= 0 || frames <= 0 || sampleRate <= 0.0)
		return t;
	double start = (double) from / hop, len = (double) frames / hop;
	double mean = tempodetail::meanOf(env, (long) start, (long) (start + len));
	if (mean <= 0.0 || len < 8.0)
		return t;
	double seconds = (double) frames / sampleRate;
	int bestBeats = 0;
	double best = -1.0;
	for (int b = 1; b <= LAYOUT_MAX_BEATS; b++) {
		double bpm = 60.0 * b / seconds;
		if (bpm < TEMPO_MIN_BPM || bpm > TEMPO_MAX_BPM)
			continue;
		double s = tempodetail::gridScore(env, start, len / b, b, mean);
		// Ties go to the fewer beats
		if (s > best + 1e-9) {
			best = s;
			bestBeats = b;
		}
	}
	if (bestBeats == 0 || best < TEMPO_MIN_SCORE)
		return t;
	int pick = bestBeats;
	double pickD = 1e9;
	for (int k = -7; k <= 7; k++) {
		double b = bestBeats * std::pow(2.0, k);
		if (b < 1.0 || b > LAYOUT_MAX_BEATS || std::fabs(b - std::round(b)) > 1e-9)
			continue;
		double bpm = 60.0 * b / seconds;
		if (bpm < TEMPO_MIN_BPM - 1e-9 || bpm > TEMPO_MAX_BPM + 1e-9)
			continue;
		double d = std::fabs(std::log(bpm / std::max(prefer, 1.0)));
		if (d < pickD) {
			pickD = d;
			pick = (int) std::lround(b);
		}
	}
	t.found = true;
	t.beats = pick;
	t.bpm = 60.0 * pick / seconds;
	t.score = best;
	return t;
}

/** The tempo of a whole file (a song, a long take; `env` its onset strength, one value per `hop` frames), whatever its
length. First the curve's own repeats (autocorrelation, with its 2x and 4x periods, which a steady beat repeats too)
give a few candidate periods; then a beat grid slides along the whole file at each, finely enough in tempo that a beat
cannot drift over the file, and at every phase: the tempo whose grid is strongest wins. Of it and its octaves, the one
nearest to `prefer`. Not found: no grid clearly stronger. */
inline FoundTempo fileTempo(const std::vector<float>& env, int hop, double sampleRate, double prefer) {
	FoundTempo t;
	long n = (long) env.size();
	double hopSec = (double) hop / std::max(sampleRate, 1.0);
	if (n < 16 || hop <= 0 || sampleRate <= 0.0 || n * hopSec < 4.0)
		return t;
	double mean = tempodetail::meanOf(env, 0, n);
	if (mean <= 0.0)
		return t;
	long lagMin = (long) std::floor(60.0 / TEMPO_MAX_BPM / hopSec), lagMax = (long) std::ceil(60.0 / TEMPO_MIN_BPM / hopSec);
	long maxLag = std::min(4 * lagMax, n / 2);
	if (lagMax >= maxLag)
		return t;
	std::vector<double> d((size_t) n);
	double ac0 = 0.0;
	for (long i = 0; i < n; i++) {
		d[(size_t) i] = env[(size_t) i] - mean;
		ac0 += d[(size_t) i] * d[(size_t) i];
	}
	if (ac0 <= 0.0)
		return t;
	std::vector<double> ac((size_t) maxLag + 1, 0.0);
	for (long L = 1; L <= maxLag; L++) {
		double s = 0.0;
		for (long i = 0; i + L < n; i++)
			s += d[(size_t) i] * d[(size_t) (i + L)];
		ac[(size_t) L] = s / (double) (n - L) * (double) n / ac0;
	}
	auto acScore = [&](long L) {
		double s = ac[(size_t) L], w = 1.0;
		if (2 * L <= maxLag) {
			s += 0.5 * ac[(size_t) (2 * L)];
			w += 0.5;
		}
		if (4 * L <= maxLag) {
			s += 0.25 * ac[(size_t) (4 * L)];
			w += 0.25;
		}
		return s / w;
	};
	// The three strongest peaks (local maxima) of the period score
	std::vector<std::pair<double, long>> peaks;
	for (long L = std::max(lagMin, 2L); L <= lagMax && L < maxLag; L++) {
		double s = acScore(L);
		if (s > acScore(L - 1) && s >= acScore(L + 1))
			peaks.push_back({s, L});
	}
	std::sort(peaks.begin(), peaks.end(), [](const std::pair<double, long>& a, const std::pair<double, long>& b) { return a.first > b.first; });
	if (peaks.size() > 3)
		peaks.resize(3);
	// Slide a beat grid along the file around each (its first minute at most: about 0.1 bpm, and quick), in tempo
	// steps that drift at most an 8th of a beat over it
	long used = std::min(n, (long) std::lround(60.0 / hopSec));
	double span = used * hopSec;
	double step = 60.0 / (8.0 * span);
	double bestBpm = 0.0, best = -1.0;
	for (const auto& pk : peaks) {
		double bpm0 = 60.0 / (pk.second * hopSec);
		for (double bpm = bpm0 * 0.975; bpm <= bpm0 * 1.025; bpm += step) {
			double period = 60.0 / bpm / hopSec;
			int beats = (int) std::floor((double) (used - 1) / period) - 1;
			if (beats < 4)
				continue;
			for (double phase = 0.0; phase < period; phase += 1.0) {
				double s = tempodetail::gridScore(env, phase, period, beats, mean);
				if (s > best) {
					best = s;
					bestBpm = bpm;
				}
			}
		}
	}
	if (bestBpm <= 0.0 || best < TEMPO_MIN_SCORE)
		return t;
	t.found = true;
	t.bpm = nearestOctave(bestBpm, prefer);
	t.score = best;
	return t;
}

/** The row for a loop of `beats`: the finest row clock whose steps cover it in whole steps, at most `maxSteps`.
`clockIndex` is the sequencer's row clock (-4 .. 4: /8 /4 /3 /2 x1 x2 x3 x4 x8). False when none fits. */
inline bool layoutRow(int beats, int maxSteps, int* clockIndex, int* steps) {
	static const double SPB[9] = {1.0 / 8.0, 1.0 / 4.0, 1.0 / 3.0, 1.0 / 2.0, 1.0, 2.0, 3.0, 4.0, 8.0};
	for (int i = 8; i >= 0; i--) {
		double n = beats * SPB[i];
		long whole = std::lround(n);
		if (std::fabs(n - (double) whole) < 1e-9 && whole >= 1 && whole <= maxSteps) {
			*clockIndex = i - 4;
			*steps = (int) whole;
			return true;
		}
	}
	return false;
}

/** A part of the sample picked for LAY by dragging on the map (Shift + drag), frames of the sample. `beats`: its length
in beats when the file has a tempo, else 0. */
struct LaySelection {
	long from = 0, frames = 0;
	int beats = 0;
};

/** The part between the press (`anchor`) and the mouse (`to`), frames of a `total`-frame sample. The press snaps to
the nearest hit within `snapFrames`. With a tempo (`bpm` > 0) the length snaps to 1, 2, 4 … 128 beats (the nearest in
log), halved until it fits in the sample on the side dragged to; without one it is the drag. Dragging left, the part
ends at the press. */
inline LaySelection laySelection(const std::vector<long>& hits, long total, double sampleRate, double bpm, long anchor, long to,
	long snapFrames) {
	LaySelection sel;
	if (total <= 1)
		return sel;
	anchor = std::min(std::max(anchor, 0L), total - 1);
	to = std::min(std::max(to, 0L), total);
	auto it = std::lower_bound(hits.begin(), hits.end(), anchor);
	long best = -1;
	if (it != hits.end())
		best = *it;
	if (it != hits.begin() && (best < 0 || anchor - *(it - 1) < best - anchor))
		best = *(it - 1);
	if (best >= 0 && std::labs(best - anchor) <= snapFrames)
		anchor = best;
	bool right = to >= anchor;
	long room = right ? total - anchor : anchor;
	long span = std::max(std::labs(to - anchor), 1L);
	long length = span;
	if (bpm > 0.0 && sampleRate > 0.0) {
		double perBeat = sampleRate * 60.0 / bpm;
		double want = std::max((double) span / perBeat, 1e-6);
		int beats = 1;
		double bestD = 1e9;
		for (int b = 1; b <= LAYOUT_MAX_BEATS; b *= 2) {
			double d = std::fabs(std::log((double) b / want));
			if (d < bestD) {
				bestD = d;
				beats = b;
			}
		}
		while (beats > 1 && (double) beats * perBeat > (double) room + 0.5)
			beats /= 2;
		sel.beats = beats;
		length = std::lround(beats * perBeat);
	}
	length = std::max(std::min(length, room), 1L);
	sel.from = right ? anchor : anchor - length;
	sel.frames = length;
	return sel;
}

/** LAY's velocities: each trig's from its hit's peak against the loop's loudest hit, straight in dB from 100 % (the
loudest) down to LAY_VELOCITY_MIN at LAY_VELOCITY_RANGE_DB under it and below. Gentle on purpose: velocity also
scales the level as it plays, and the soft hits are already soft in the sample (12 dB under: 72 %, about 3 dB more). */
static constexpr double LAY_VELOCITY_RANGE_DB = 30.0;
static constexpr float LAY_VELOCITY_MIN = 0.3f;
/** How long after its start a hit's peak is looked for */
static constexpr double LAY_PEAK_SECONDS = 0.03;
inline float layVelocity(double dbUnderLoudest) {
	double v = 1.0 - std::min(std::max(dbUnderLoudest, 0.0), LAY_VELOCITY_RANGE_DB) / LAY_VELOCITY_RANGE_DB * (1.0 - LAY_VELOCITY_MIN);
	return (float) v;
}

/** One trig of the layout: its step, and the slice it plays (frames of the sample) */
struct LayoutTrig {
	int step;
	long start;
	long length;
};

/** The trigs for the loop `from` .. `from + frames` laid over `steps` steps. `grid`: every step plays its own
share of the loop. Otherwise each hit inside the loop goes on its nearest step (of several on a step, the one nearest
to it; the others play inside its slice; a hit that rounds to the loop's end belongs to the next pass), and step 1
always plays from the loop's start, so what comes before the first hit is heard too. Each trig plays up to the next one's start (the last: up to the loop's end). */
inline std::vector<LayoutTrig> layoutTrigs(const std::vector<long>& hits, long from, long frames, int steps, bool grid) {
	std::vector<LayoutTrig> out;
	if (frames <= 0 || steps <= 0)
		return out;
	if (grid) {
		for (int s = 0; s < steps; s++)
			out.push_back({s, from + (long) std::llround((double) s * (double) frames / steps), 0});
	}
	else {
		// Of the hits that round to a step, the one nearest to it (a soft hit half a step before a beat is not the beat)
		std::vector<long> at(steps, -1);
		std::vector<double> off(steps, 1e9);
		for (long h : hits) {
			if (h < from || h >= from + frames)
				continue;
			double pos = (double) (h - from) / (double) frames * steps;
			long s = std::lround(pos);
			if (s >= steps || std::fabs(pos - (double) s) >= off[(size_t) s])
				continue;
			at[(size_t) s] = h;
			off[(size_t) s] = std::fabs(pos - (double) s);
		}
		if (at[0] < 0)
			at[0] = from;
		for (int s = 0; s < steps; s++)
			if (at[(size_t) s] >= 0)
				out.push_back({s, at[(size_t) s], 0});
	}
	for (size_t i = 0; i < out.size(); i++) {
		long next = (i + 1 < out.size()) ? out[i + 1].start : from + frames;
		out[i].length = std::max(next - out[i].start, 1L);
	}
	return out;
}

} // namespace wa
