#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Wavetable extraction from a WAV sample.
// This file has no Rack dependencies so it can be unit-tested offline (see tests/).

namespace wa {

static const size_t MAX_FILE_BYTES = 2 * 1024 * 1024;
static const int FRAME_SIZE = 2048;
/** Band-limited levels per octave of pitch. With one per octave, the notes just above each switch
point lose up to half their top end (an audible brightness step between neighboring semitones);
two per octave halve that loss. */
static const int LEVELS_PER_OCTAVE = 2;
/** 10 octaves (1024 harmonics down to 1) plus level 0 */
static const int NUM_LEVELS = 10 * LEVELS_PER_OCTAVE + 1;

/** Highest harmonic kept in level `l`: FRAME_SIZE / 2 at level 0, halving every octave of levels,
down to a pure fundamental at the last level. */
inline int levelHarmonics(int l) {
	double h = (FRAME_SIZE / 2) * std::exp2(-(double) l / LEVELS_PER_OCTAVE);
	return std::max(1, (int) std::floor(h + 1e-9));
}
/** Artifact loudness: every table is scaled to this RMS (level 0), unless that would push its peak
above ARTIFACT_PEAK_CAP. Equal loudness means moving between artifacts of different shapes does not
swell or dip. A sine-like table comes out at about 0.78 peak, a saw at about 0.95. */
static const float ARTIFACT_RMS = 0.55f;
static const float ARTIFACT_PEAK_CAP = 1.6f;
static const int MIN_TABLES = 50;
static const int MAX_TABLES = 100;
/** Shorter sources cannot fill a frame plus the wrap crossfade. */
static const int MIN_SOURCE_FRAMES = 4096;

enum TableCountMode {
	TABLES_AUTO,
	TABLES_50,
	TABLES_64,
	TABLES_100,
	TABLE_MODES_LEN
};

/** How much the highest harmonics of every artifact are softened when the bank is built. */
enum TableTone {
	TONE_NATURAL,
	/** Gentle roll-off above about harmonic 48 (the default): low notes lose their buzz, high notes keep their sparkle */
	TONE_SMOOTH,
	/** Stronger roll-off above about harmonic 20 */
	TONE_SOFT,
	TABLE_TONES_LEN
};

struct WavetableBank {
	int numTables = 0;
	/** Layout: [table][level][FRAME_SIZE]. Each table is loudness-normalized at level 0 (ARTIFACT_RMS). */
	std::vector<float> data;
	/** Spectral centroid per table, mapped to 0..1 (for display). */
	std::vector<float> brightness;
	/** 1 if a fundamental was detected for the table, 0 if it was taken as a raw window. */
	std::vector<uint8_t> pitched;
	double sourceSeconds = 0.0;
	unsigned sourceSampleRate = 0;

	const float* frame(int table, int level) const {
		return &data[((size_t) table * NUM_LEVELS + level) * FRAME_SIZE];
	}
	float* frame(int table, int level) {
		return &data[((size_t) table * NUM_LEVELS + level) * FRAME_SIZE];
	}
};

int tableCountFor(TableCountMode mode, double seconds);

/** Decodes WAV bytes and builds the bank.
Returns nullptr and sets `error` on failure.
Slow (hundreds of ms): call it from a worker thread, never from process().
*/
WavetableBank* analyzeWav(const uint8_t* bytes, size_t size, TableCountMode mode, std::string* error, TableTone tone = TONE_SMOOTH);

} // namespace wa
