#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// SPREAD shapes (where the four cores sit around CUTOFF), scales, chords and the noise colours.
// No Rack dependencies (tested in tests/).

namespace np {

enum SpreadShape {
	SHAPE_EVEN,
	SHAPE_HARMONIC,
	SHAPE_SCALE,
	SHAPE_RANDOM,
	SHAPE_CHORD,
	/** Tuned bell partials hum 1 : prime 2 : tierce 2.4 : nominal 4 (the minor-third tierce), exact at SPREAD 50% */
	SHAPE_BELL,
	/** Vowel formants (F1, F2, F3, F4); SPREAD morphs A > E > I > O > U, CUTOFF shifts the voice (1 kHz = adult) */
	SHAPE_VOWEL,
	NUM_SPREAD_SHAPES
};

/** Just intonation: octaves from C of `semis` semitones as a pure ratio (5-limit; with `harmonic7` the minor seventh
is the harmonic 7/4 instead of 9/5). A major third is 5/4 (-13.7 cents from 12-TET), a fifth 3/2 (+2 cents). */
inline float justOctaves(int semis, bool harmonic7) {
	static const float RATIO[12] = {1.f, 16.f / 15.f, 9.f / 8.f, 6.f / 5.f, 5.f / 4.f, 4.f / 3.f, 45.f / 32.f, 3.f / 2.f,
		8.f / 5.f, 5.f / 3.f, 9.f / 5.f, 15.f / 8.f};
	int oct = semis >= 0 ? semis / 12 : -((-semis + 11) / 12);
	int i = semis - 12 * oct;
	float r = (i == 10 && harmonic7) ? 7.f / 4.f : RATIO[i];
	return (float) oct + std::log2(r);
}

/** Formant frequencies (Hz) F1..F4 for A, E, I, O, U (F1-F3 after Winer, The Audio Circuits Cookbook, table 5.3) */
static const float VOWELS[5][4] = {
	{600.f, 1050.f, 2350.f, 3300.f},
	{400.f, 1600.f, 2425.f, 3300.f},
	{250.f, 1750.f, 2600.f, 3300.f},
	{400.f, 750.f, 2600.f, 3300.f},
	{350.f, 600.f, 2700.f, 3300.f},
};

struct Scale {
	const char* name;
	int count;
	int semis[12];
};

static const Scale SCALES[] = {
	{"Major", 7, {0, 2, 4, 5, 7, 9, 11}},
	{"Minor", 7, {0, 2, 3, 5, 7, 8, 10}},
	{"Dorian", 7, {0, 2, 3, 5, 7, 9, 10}},
	{"Phrygian", 7, {0, 1, 3, 5, 7, 8, 10}},
	{"Lydian", 7, {0, 2, 4, 6, 7, 9, 11}},
	{"Mixolydian", 7, {0, 2, 4, 5, 7, 9, 10}},
	{"Major pentatonic", 5, {0, 2, 4, 7, 9}},
	{"Minor pentatonic", 5, {0, 3, 5, 7, 10}},
	{"Hirajoshi", 5, {0, 2, 3, 7, 8}},
	{"Whole tone", 6, {0, 2, 4, 6, 8, 10}},
	{"Chromatic", 12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
};
static const int NUM_SCALES = sizeof(SCALES) / sizeof(SCALES[0]);

struct Chord {
	const char* name;
	int semis[4];
};

static const Chord CHORDS[] = {
	{"Major 7", {0, 4, 7, 11}},
	{"Minor 7", {0, 3, 7, 10}},
	{"Dominant 7", {0, 4, 7, 10}},
	{"Minor 9 (no 5th)", {0, 3, 10, 14}},
	{"Major 9 (no 3rd)", {0, 7, 11, 14}},
	{"Major", {0, 4, 7, 12}},
	{"Minor", {0, 3, 7, 12}},
	{"Sus2", {0, 2, 7, 12}},
	{"Sus4", {0, 5, 7, 12}},
	{"Fifths", {0, 7, 12, 19}},
	{"Quartal", {0, 5, 10, 15}},
	{"Octaves", {0, 12, 24, 36}},
};
static const int NUM_CHORDS = sizeof(CHORDS) / sizeof(CHORDS[0]);

/** Semitones of scale degree `deg` (0 = root; wraps into higher octaves). */
inline int scaleSemis(int scale, int deg) {
	const Scale& s = SCALES[std::min(std::max(scale, 0), NUM_SCALES - 1)];
	int oct = deg >= 0 ? deg / s.count : -((-deg + s.count - 1) / s.count);
	int i = deg - oct * s.count;
	return 12 * oct + s.semis[i];
}

/** Nearest note of `scale` (root C) to `semis` semitones from C. */
inline float quantizeToScale(int scale, float semis) {
	const Scale& s = SCALES[std::min(std::max(scale, 0), NUM_SCALES - 1)];
	float oct = std::floor(semis / 12.f);
	float within = semis - 12.f * oct;
	float best = 0.f, bestDist = 1e9f;
	for (int i = 0; i <= s.count; i++) {
		float n = (i < s.count) ? (float) s.semis[i] : 12.f;
		float d = std::fabs(within - n);
		if (d < bestDist) {
			bestDist = d;
			best = n;
		}
	}
	return 12.f * oct + best;
}

/** Each core's offset from CUTOFF in octaves, for SPREAD `s` (0..1).
- Even: centred, equal steps, +-2.5 octaves at full SPREAD.
- Harmonic: root on CUTOFF, the others at harmonics 2, 3, 4 at SPREAD 50% (stretched or squeezed around it).
- Scale: root on CUTOFF, the others every `step` scale degrees (SPREAD 0..1 = 0..7 degrees apart).
- Random: centred, frozen random places (`random[]` in -1..1), +-2.5 octaves at full SPREAD.
- Chord: the chord's tones from CUTOFF; SPREAD climbs through inversions (each step lifts the lowest note an octave). */
inline void spreadOffsets(int shape, float s, int scale, int chord, const float random[4], float out[4],
		bool just = false) {
	s = std::min(std::max(s, 0.f), 1.f);
	switch (shape) {
		case SHAPE_BELL: {
			static const float BELL[4] = {1.f, 2.f, 2.4f, 4.f};
			for (int i = 0; i < 4; i++)
				out[i] = 2.f * s * std::log2(BELL[i]);
			break;
		}
		case SHAPE_VOWEL: {
			// Offsets from CUTOFF relative to 1 kHz, so CUTOFF at 1 kHz gives the table's voice
			float pos = s * 4.f;
			int a = std::min((int) pos, 3);
			float f = pos - a;
			for (int i = 0; i < 4; i++) {
				float la = std::log2(VOWELS[a][i] / 1000.f), lb = std::log2(VOWELS[a + 1][i] / 1000.f);
				out[i] = la + (lb - la) * f;
			}
			break;
		}
		case SHAPE_HARMONIC:
			for (int i = 0; i < 4; i++)
				out[i] = 2.f * s * std::log2((float) (i + 1));
			break;
		case SHAPE_SCALE: {
			float step = s * 7.f;
			for (int i = 0; i < 4; i++) {
				int semis = scaleSemis(scale, (int) std::lround(i * step));
				out[i] = just ? justOctaves(semis, false) : semis / 12.f;
			}
			break;
		}
		case SHAPE_RANDOM:
			for (int i = 0; i < 4; i++)
				out[i] = 2.5f * s * random[i];
			break;
		case SHAPE_CHORD: {
			const Chord& c = CHORDS[std::min(std::max(chord, 0), NUM_CHORDS - 1)];
			int n[4] = {c.semis[0], c.semis[1], c.semis[2], c.semis[3]};
			int inversions = (int) std::lround(s * 8.f);
			for (int v = 0; v < inversions; v++) {
				int lo = 0;
				for (int i = 1; i < 4; i++)
					if (n[i] < n[lo])
						lo = i;
				n[lo] += 12;
			}
			// Dominant 7 (index 2) takes the harmonic seventh 7/4 in just tuning
			for (int i = 0; i < 4; i++)
				out[i] = just ? justOctaves(n[i], chord == 2) : n[i] / 12.f;
			break;
		}
		default: {
			static const float EVEN[4] = {-1.f, -1.f / 3.f, 1.f / 3.f, 1.f};
			for (int i = 0; i < 4; i++)
				out[i] = 2.5f * s * EVEN[i];
			break;
		}
	}
}

/** xorshift32 */
struct Rng {
	uint32_t s = 0x9E3779B9u;
	void seed(uint32_t v) {
		s = v ? v : 0x9E3779B9u;
	}
	uint32_t next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}
	/** -1..1 */
	float uniform() {
		return (float) (next() >> 8) * (1.f / 8388608.f) - 1.f;
	}
	/** 0..1 */
	float unit() {
		return (float) (next() >> 8) * (1.f / 16777216.f);
	}
};

/** Digital noise: a one-bit shift-register (LFSR) sequence, as in old consoles and the hardware noise generators in
Winer's book: a +-1 square level changing at the sample rate. A long register (17 bits) is white; a short one repeats
every 2^n - 1 steps, so it turns into a pitched, metallic buzz (4 bits: a buzz at sample rate / 15; 3 bits: / 7).
COLOR 0..1 steps the register length 17, 15, 11, 9, 7, 5, 4, 3. RMS ~0.3, like the coloured noise. */
struct ShiftNoise {
	static const int LENGTHS = 8;
	uint32_t reg = 1;
	int idx = 0;

	static int bitsFor(int i) {
		static const int B[LENGTHS] = {17, 15, 11, 9, 7, 5, 4, 3};
		return B[i];
	}
	/** Feedback taps (bit 0 and one more: maximal length, period 2^n - 1; checked in tests/) */
	static uint32_t maskFor(int i) {
		static const uint32_t M[LENGTHS] = {0x9, 0x3, 0x5, 0x11, 0x3, 0x5, 0x3, 0x3};
		return M[i];
	}

	float process(float color) {
		int want = std::min((int) (std::min(std::max(color, 0.f), 1.f) * LENGTHS), LENGTHS - 1);
		if (want != idx) {
			idx = want;
			reg = (reg & ((1u << bitsFor(idx)) - 1u)) ? (reg & ((1u << bitsFor(idx)) - 1u)) : 1u;
		}
		int n = bitsFor(idx);
		float out = (reg & 1u) ? 0.3f : -0.3f;
		uint32_t x = reg & maskFor(idx);
		uint32_t fb = 0;
		while (x) {
			fb ^= 1u;
			x &= x - 1u;
		}
		reg = (reg >> 1) | (fb << (n - 1));
		return out;
	}
};

/** Noise in four colours, crossfaded by COLOR 0..1: white (0), pink (1/3), brown (2/3), crackle (1). Each colour
is scaled to about the same loudness (RMS ~0.3). */
struct ColoredNoise {
	Rng rng;
	float b0 = 0.f, b1 = 0.f, b2 = 0.f;
	float brown = 0.f;
	float crackle = 0.f;

	/** Measured RMS of each raw colour, so they can be matched */
	static constexpr float WHITE_GAIN = 0.52f;  // uniform -1..1 has RMS 0.577
	static constexpr float PINK_GAIN = 0.1765f;
	static constexpr float BROWN_GAIN = 1.18f;
	static constexpr float CRACKLE_GAIN = 2.32f;

	float process(float color) {
		float w = rng.uniform();
		color = std::min(std::max(color, 0.f), 1.f) * 3.f;
		int seg = std::min((int) color, 2);
		float f = color - seg;
		float a = 0.f, b = 0.f;
		// Only the two colours being crossfaded need their filters to run... but their states must stay warm, so
		// the cheap filters always run.
		b0 = 0.99765f * b0 + w * 0.0990460f;
		b1 = 0.96300f * b1 + w * 0.2965164f;
		b2 = 0.57000f * b2 + w * 1.0526913f;
		float pink = (b0 + b1 + b2 + w * 0.1848f) * PINK_GAIN;
		brown = 0.996f * brown + 0.04f * w;
		float br = brown * BROWN_GAIN;
		float imp = 0.f;
		if (rng.unit() < 0.006f)
			imp = rng.uniform() > 0.f ? 1.f : -1.f;
		crackle = imp + 0.8f * crackle;
		float cr = crackle * CRACKLE_GAIN;
		float wh = w * WHITE_GAIN;
		const float col[4] = {wh, pink, br, cr};
		a = col[seg];
		b = col[seg + 1];
		return a + (b - a) * f;
	}
};

} // namespace np
