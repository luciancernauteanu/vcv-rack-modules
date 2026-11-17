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
	// A sample-pack name that starts with its tempo: "90_Blok_01", "120 C White" (2 or 3 digits, then _ space or -;
	// 60..200 only, so take numbers like "01_" are not a tempo)
	size_t d = 0;
	while (d < s.size() && std::isdigit((unsigned char) s[d]))
		d++;
	if ((d == 2 || d == 3) && d < s.size() && (s[d] == '_' || s[d] == ' ' || s[d] == '-')) {
		double v = std::atof(s.substr(0, d).c_str());
		if (v >= 60.0 && v <= 200.0)
			return v;
	}
	return 0.0;
}

/** The tempo in a loop file's name, checked against its length: bpmFromName's, else a number anywhere in the name
("ARSS_120_A_Bouzuki_String": 120), 60..200 and on its own between separators, when the file lasts a whole number of
its beats (2 or more) within 5 ms. Sample packs cut loops to the sample, so a 4.000 s file is 8 beats at 120; a take
number or a key almost never fits that closely by chance. 0 when nothing fits. */
inline double bpmFromLoopName(const std::string& name, double fileSeconds) {
	double v = bpmFromName(name);
	if (v > 0.0 || fileSeconds <= 0.0)
		return v;
	auto fits = [&](double bpm) {
		double beats = fileSeconds * bpm / 60.0;
		double whole = std::round(beats);
		return whole >= 2.0 && std::fabs(beats - whole) * 60.0 / bpm <= 0.005;
	};
	// The file's extension is not part of the name
	size_t end = name.size();
	size_t dot = name.find_last_of('.');
	if (dot != std::string::npos && name.size() - dot <= 5)
		end = dot;
	for (size_t i = 0; i < end;) {
		if (!std::isdigit((unsigned char) name[i])) {
			i++;
			continue;
		}
		size_t j = i;
		while (j < end && std::isdigit((unsigned char) name[j]))
			j++;
		// On its own: no letter or digit right before or after ("A120", "120A", "v2.120" are not tempos)
		bool before = i > 0 && (std::isalnum((unsigned char) name[i - 1]) || name[i - 1] == '.');
		bool after = j < end && (std::isalnum((unsigned char) name[j]) || name[j] == '.');
		if ((j - i == 2 || j - i == 3) && !before && !after) {
			double t = std::atof(name.substr(i, j - i).c_str());
			if (t >= 60.0 && t <= 200.0 && fits(t))
				return t;
		}
		i = j;
	}
	return 0.0;
}

/** A loop file's own tempo: of the candidates (the name's, then the one found in the audio) and their octaves in the
tempo range, those at which the whole file lasts a whole number of beats (within 1 %); the octave nearest `prefer`.
0 when none fits (a take, not a loop). The 16ths of a 90 bpm loop are also 8th triplets at 120: the whole beats tell
them apart (a 10.667 s loop is 16 beats at 90, 21.3 at 120). */
inline double loopFileBpm(double fileSeconds, double nameBpm, double foundBpm, double prefer) {
	if (fileSeconds <= 0.0)
		return 0.0;
	double cands[2] = {nameBpm, foundBpm};
	for (int ci = 0; ci < 2; ci++) {
		double c = cands[ci];
		if (c <= 0.0)
			continue;
		// The name's own tempo, as written, when the file is whole beats of it: a "174_..." loop counts its beats and bars
		// at 174, not at the octave nearest the module (LAY keeps its speed either way)
		if (ci == 0 && c >= 60.0 - 1e-9 && c <= 200.0 + 1e-9) {
			double beats = fileSeconds * c / 60.0;
			if (beats >= 1.0 && std::fabs(beats - std::round(beats)) <= 0.01 * beats)
				return c;
		}
		double best = 0.0, bestD = 1e9;
		for (int k = -3; k <= 3; k++) {
			double b = c * std::pow(2.0, k);
			if (b < 60.0 - 1e-9 || b > 200.0 + 1e-9)
				continue;
			double beats = fileSeconds * b / 60.0;
			if (beats < 1.0 || std::fabs(beats - std::round(beats)) > 0.01 * beats)
				continue;
			double dd = std::fabs(std::log(b / std::max(prefer, 1.0)));
			if (dd < bestD) {
				bestD = dd;
				best = b;
			}
		}
		if (best > 0.0)
			return best;
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
/** How many steps LAY may use for a part of `beats` beats: one page (16) up to 4 beats, as it always was, then as many as
16th notes need, over the next pages, up to `rowSteps` (64: 4 pages). So a long selection keeps 16th-note steps and
continues onto pages 2-4 (8 beats: 32 steps, 16 beats: 64); only past 16 beats do the steps grow coarser. */
inline int layMaxSteps(int beats, int pageSteps = 16, int rowSteps = 64) {
	return std::min(std::max(beats * 4, pageSteps), rowSteps);
}

/** LAY never picks a triplet row clock (/3, x3: index 2 and 6 of the clock list): laid on them a loop sounds off the beat */
inline bool layClockAllowed(int spbIndex) {
	return spbIndex != 2 && spbIndex != 6;
}

/** SLICE on a note value (1..6 = 1/1, 1/2, 1/4, 1/8, 1/16, 1/32): the note's division of a bar */
inline int sliceNoteDivision(int mode) {
	static const int DIV[7] = {0, 1, 2, 4, 8, 16, 32};
	return DIV[std::min(std::max(mode, 0), 6)];
}
/** One slice's frames: a 1/`division` note at `bpm` (4 beats a bar); no tempo (0): the whole file is one bar */
inline double sliceFrames(long total, double sampleRate, double bpm, int division) {
	if (division <= 0 || total <= 0)
		return 0.0;
	if (bpm > 0.0)
		return sampleRate * 60.0 / bpm * 4.0 / (double) division;
	return (double) total / (double) division;
}
/** The file cut into equal slices from its start (a loop starts on the beat): their starts, frames. A last piece shorter
than 10 ms is no slice. */
inline std::vector<long> sliceGrid(long total, double sampleRate, double bpm, int division) {
	std::vector<long> cuts;
	double d = sliceFrames(total, sampleRate, bpm, division);
	if (d < 1.0)
		return cuts;
	long minTail = (long) (0.01 * sampleRate);
	for (long k = 0;; k++) {
		long at = std::lround(k * d);
		if (at >= total - minTail)
			break;
		cuts.push_back(at);
	}
	return cuts;
}
/** LAY of a part cut into SLICE slices: one slice per step, when a row clock plays them at their own speed. `moduleBeats`:
the part's length in the module's beats at its own speed (layModuleBeats). The clock whose steps per beat is the slices'
per beat within 2 % (no triplets), at most 64 steps. */
inline bool layOneSlicePerStep(double partFrames, double sliceFrames, double moduleBeats, int* clockIndex, int* steps) {
	static const double SPB[9] = {1.0 / 8.0, 1.0 / 4.0, 1.0 / 3.0, 1.0 / 2.0, 1.0, 2.0, 3.0, 4.0, 8.0};
	if (sliceFrames <= 0.0 || moduleBeats <= 0.0)
		return false;
	long n = std::lround(partFrames / sliceFrames);
	if (n < 1 || n > 64)
		return false;
	double perBeat = (double) n / moduleBeats;
	for (int i = 8; i >= 0; i--) {
		if (!layClockAllowed(i) || std::fabs(SPB[i] - perBeat) > 0.02 * perBeat)
			continue;
		*clockIndex = i - 4;
		*steps = (int) n;
		return true;
	}
	return false;
}

/** LAY keeps a loop's own tempo: how many of the module's beats the part lasts when it plays at its own speed (a 174 bpm
loop of 8 beats, 2.76 s, lasts 5.52 beats at 120). Laid on that many beats, it plays as fast as the file, whatever the
module's tempo; on the file's beats instead, it would follow the module (1.38x faster as 87 bpm, 0.69x as 174). */
inline double layModuleBeats(double seconds, double moduleBpm) {
	return seconds * std::max(moduleBpm, 1.0) / 60.0;
}
/** The part lasts a whole number of the module's beats (within 1 %): its tempo is the module's, or an octave of it, and
LAY lays it out exactly as on whole beats */
inline bool layWholeBeats(double moduleBeats, int* beats) {
	long b = std::lround(moduleBeats);
	if (b < 1 || std::fabs(moduleBeats - (double) b) > 0.01 * moduleBeats)
		return false;
	*beats = (int) b;
	return true;
}
/** A loop whose tempo is near the module's (within 8 %, octaves too) goes on the module's grid as it always did: its
`loopBeats` (or a half / double of them) as whole module beats, so it follows the module's tempo by those few percent
(a 100 bpm loop at 103: 8 beats on 32 steps, not 33 steps of 8.24 beats). Only a tempo clearly apart keeps its own
speed with an exact step count. Returns `moduleBeats` itself when no octave of the loop's beats is that near. */
inline double layNearTempoBeats(double moduleBeats, int loopBeats) {
	if (loopBeats < 1 || moduleBeats <= 0.0)
		return moduleBeats;
	double best = moduleBeats, bestErr = 0.08;
	for (int k = -3; k <= 3; k++) {
		double target = std::ldexp((double) loopBeats, k);
		if (target < 1.0 || std::fabs(target - std::round(target)) > 1e-9)
			continue;
		double err = std::fabs(moduleBeats - target) / moduleBeats;
		if (err <= bestErr) {
			bestErr = err;
			best = target;
		}
	}
	return best;
}

/** The row clocks a part of `moduleBeats` (not whole) can be laid on at its own speed: the whole number of steps nearest
moduleBeats x steps-per-beat, when that keeps the speed within 2 %, and at most `maxSteps`. `use(clockIndex, steps)` for
each, finest clock first. */
template <class Use>
inline void layTimedClocks(double moduleBeats, int maxSteps, Use use) {
	static const double SPB[9] = {1.0 / 8.0, 1.0 / 4.0, 1.0 / 3.0, 1.0 / 2.0, 1.0, 2.0, 3.0, 4.0, 8.0};
	for (int i = 8; i >= 0; i--) {
		if (!layClockAllowed(i)) // no triplet clocks (x3, /3)
			continue;
		double n = moduleBeats * SPB[i];
		long whole = std::lround(n);
		if (whole < 1 || whole > maxSteps || std::fabs((double) whole - n) > 0.02 * n)
			continue;
		use(i - 4, (int) whole);
	}
}

inline bool layoutRow(int beats, int maxSteps, int* clockIndex, int* steps) {
	static const double SPB[9] = {1.0 / 8.0, 1.0 / 4.0, 1.0 / 3.0, 1.0 / 2.0, 1.0, 2.0, 3.0, 4.0, 8.0};
	for (int i = 8; i >= 0; i--) {
		if (!layClockAllowed(i)) // no triplet clocks (x3, /3)
			continue;
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
the nearest hit or to the sample's start / end within `snapFrames`. With a tempo (`bpm` > 0) the length snaps to whole
beats: 1, 2, 4 … 128, or all the beats there is room for up to the sample's edge on the side dragged to (when LAY can
lay that count out), the nearest in log to the drag. A count that is short of the room by up to 1 % still fits (the
part then ends at the edge): a 16-beat loop a hair short, or pressed a few pixels in, gives all 16 beats, not 8.
Without a tempo it is the drag. Dragging left, the part ends at the press. */
inline LaySelection laySelection(const std::vector<long>& hits, long total, double sampleRate, double bpm, long anchor, long to,
	long snapFrames) {
	LaySelection sel;
	if (total <= 1)
		return sel;
	anchor = std::min(std::max(anchor, 0L), total);
	to = std::min(std::max(to, 0L), total);
	auto it = std::lower_bound(hits.begin(), hits.end(), anchor);
	long best = -1;
	if (it != hits.end())
		best = *it;
	if (it != hits.begin() && (best < 0 || anchor - *(it - 1) < best - anchor))
		best = *(it - 1);
	// The sample's start and end snap as hits do (a press near the edge means "from the edge")
	for (long edge : {0L, total})
		if (best < 0 || std::labs(edge - anchor) < std::labs(best - anchor))
			best = edge;
	if (best >= 0 && std::labs(best - anchor) <= snapFrames)
		anchor = best;
	bool right = to >= anchor;
	if (right && anchor >= total)
		anchor = total - 1;
	long room = right ? total - anchor : anchor;
	long span = std::max(std::labs(to - anchor), 1L);
	long length = span;
	if (bpm > 0.0 && sampleRate > 0.0) {
		double perBeat = sampleRate * 60.0 / bpm;
		double want = std::max((double) span / perBeat, 1e-6);
		// The most whole beats that fit (1 % short of the room still fits)
		int maxBeats = std::min(std::max((int) std::floor((double) room * 1.01 / perBeat + 1e-9), 1), LAYOUT_MAX_BEATS);
		int beats = 1;
		double bestD = 1e9;
		auto consider = [&](int b) {
			double d = std::fabs(std::log((double) b / want));
			if (d < bestD) {
				bestD = d;
				beats = b;
			}
		};
		for (int b = 1; b <= maxBeats; b *= 2)
			consider(b);
		// Up to the edge: all of it, when LAY has a row for that many beats
		int c, n;
		if (layoutRow(maxBeats, layMaxSteps(maxBeats), &c, &n))
			consider(maxBeats);
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

/** How far from its step (in steps, and in seconds) a hit may be for its slice to start exactly on it. A trig plays on
its step, so a slice starting on a hit further off would play it early or late by as much, and the slice before would
stop short (a gap) or be cut. Such a hit (a live break's 16th 21 ms early, a 16th between the steps of an 8th row) still
gets its trig on its nearest step, but its slice starts on the step's own place in the audio: the hit plays inside it,
or at the end of the slice before, at its own time. 12 ms: the detector places a hit a few ms into its attack (about
7 ms on a drum loop), so hits on the beat start their slices. */
static constexpr double LAY_SNAP_STEPS = 0.2;
static constexpr double LAY_SNAP_SECONDS = 0.012;
inline double laySnapSteps(double stepSeconds) {
	return stepSeconds > 0.0 ? std::min(LAY_SNAP_STEPS, LAY_SNAP_SECONDS / stepSeconds) : LAY_SNAP_STEPS;
}

/** The trigs for the loop `from` .. `from + frames` laid over `steps` steps. `grid`: every step plays its own
share of the loop. Otherwise each hit inside the loop goes on its nearest step (of several on a step, the one nearest
to it; the others play inside its slice; a hit that rounds to the loop's end belongs to the next pass); its slice
starts on the hit when it is within `snap` steps of the step, else on the step's own place (see LAY_SNAP_STEPS). Step 1
always plays from the loop's start, so what comes before the first hit is heard too. Each trig plays up to the next
one's start (the last: up to the loop's end). `onHits`: how many trigs after step 1 start right on their hit. */
inline std::vector<LayoutTrig> layoutTrigs(const std::vector<long>& hits, long from, long frames, int steps, bool grid, double snap = 0.5,
	int* onHits = nullptr) {
	std::vector<LayoutTrig> out;
	if (onHits)
		*onHits = 0;
	if (frames <= 0 || steps <= 0)
		return out;
	auto gridAt = [&](int s) {
		return from + (long) std::llround((double) s * (double) frames / steps);
	};
	if (grid) {
		for (int s = 0; s < steps; s++)
			out.push_back({s, gridAt(s), 0});
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
		if (at[0] < 0) {
			at[0] = from;
			off[0] = 0.0;
		}
		for (int s = 0; s < steps; s++) {
			if (at[(size_t) s] < 0)
				continue;
			// Close to its step: the slice starts on the hit. Further off: on the step's own place, so the hit plays
			// inside its slice (late hit) or at the end of the one before (early hit), at its own time either way.
			bool onHit = off[(size_t) s] <= snap;
			if (onHit && onHits && s > 0)
				(*onHits)++;
			out.push_back({s, onHit ? at[(size_t) s] : gridAt(s), 0});
		}
	}
	for (size_t i = 0; i < out.size(); i++) {
		long next = (i + 1 < out.size()) ? out[i + 1].start : from + frames;
		out[i].length = std::max(next - out[i].start, 1L);
	}
	return out;
}

/** The row for a loop of `beats` (`from` .. `from + frames`, `seconds` long) that fits `hits` best: of the row clocks
whose steps cover it in whole steps (at most `maxSteps`), the one with the most hits right on a step (within
laySnapSteps), then the most trigs; a tie goes to the finer clock. A triplet loop gets a triplet clock (x3, /3), a 16th
one x4. `placed`: its trigs. False when no clock fits. */
inline bool layoutRowForHits(const std::vector<long>& hits, long from, long frames, double seconds, int beats, int maxSteps,
	int* clockIndex, int* steps, int* placed = nullptr) {
	static const double SPB[9] = {1.0 / 8.0, 1.0 / 4.0, 1.0 / 3.0, 1.0 / 2.0, 1.0, 2.0, 3.0, 4.0, 8.0};
	int bestOn = -1, bestCount = -1;
	for (int i = 8; i >= 0; i--) {
		if (!layClockAllowed(i)) // no triplet clocks (x3, /3)
			continue;
		double n = beats * SPB[i];
		long whole = std::lround(n);
		if (std::fabs(n - (double) whole) > 1e-9 || whole < 1 || whole > maxSteps)
			continue;
		int on = 0;
		int count = (int) layoutTrigs(hits, from, frames, (int) whole, false, laySnapSteps(seconds / (double) whole), &on).size();
		if (on > bestOn || (on == bestOn && count > bestCount)) {
			bestOn = on;
			bestCount = count;
			*clockIndex = i - 4;
			*steps = (int) whole;
		}
	}
	if (placed)
		*placed = std::max(bestCount, 0);
	return bestCount >= 0;
}

/** LAY at the loop's own speed: the part lasts `moduleBeats` of the module's beats (layModuleBeats). A whole number of
them: exactly layoutRow / layoutRowForHits on that many beats (as always). Otherwise the row clock and step count
(any count, up to 64, e.g. 44 steps of x8) that keep the speed within 2 % and put the most hits on trigs; `grid`
(every step): the finest of them. */
inline bool layoutRowAtOwnTempo(const std::vector<long>& hits, long from, long frames, double seconds, double moduleBeats, bool grid,
	int* clockIndex, int* steps, int* placed = nullptr) {
	int whole;
	if (layWholeBeats(moduleBeats, &whole)) {
		if (grid) {
			if (!layoutRow(whole, layMaxSteps(whole), clockIndex, steps))
				return false;
			if (placed)
				*placed = 0;
			return true;
		}
		return layoutRowForHits(hits, from, frames, seconds, whole, layMaxSteps(whole), clockIndex, steps, placed);
	}
	int bestOn = -1, bestCount = -1;
	layTimedClocks(moduleBeats, 64, [&](int ci, int n) {
		if (grid) {
			if (bestCount < 0) {
				bestCount = 0;
				bestOn = 0;
				*clockIndex = ci;
				*steps = n;
			}
			return;
		}
		int on = 0;
		int count = (int) layoutTrigs(hits, from, frames, n, false, laySnapSteps(seconds / (double) n), &on).size();
		if (on > bestOn || (on == bestOn && count > bestCount)) {
			bestOn = on;
			bestCount = count;
			*clockIndex = ci;
			*steps = n;
		}
	});
	if (placed)
		*placed = std::max(bestCount, 0);
	return bestCount >= 0;
}

} // namespace wa
