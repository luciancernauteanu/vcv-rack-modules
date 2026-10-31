#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Wavetable extraction from a WAV sample.
// This file has no Rack dependencies so it can be unit-tested offline (see tests/).

namespace wa {

static const size_t MAX_FILE_BYTES = 2 * 1024 * 1024;
static const int FRAME_SIZE = 2048;
/** Level L keeps harmonics 1..(FRAME_SIZE / 2 >> L), so the last level is a pure fundamental. */
static const int NUM_LEVELS = 11;
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

struct WavetableBank {
	int numTables = 0;
	/** Layout: [table][level][FRAME_SIZE]. Each table is peak-normalized at level 0. */
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
WavetableBank* analyzeWav(const uint8_t* bytes, size_t size, TableCountMode mode, std::string* error);

} // namespace wa
