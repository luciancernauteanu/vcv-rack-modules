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

/** Largest WAV accepted. Analysis time hardly depends on it (50-100 short slices are examined);
loading needs up to ~8x the file size in RAM for a moment (decoding to float). */
static const int MAX_FILE_MB = 10;
static const size_t MAX_FILE_BYTES = (size_t) MAX_FILE_MB * 1024 * 1024;
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

/** The decoded source sample, kept for the granular engine (the wavetables are built from it, then it
would be thrown away). Mono or stereo float. `levels[k]` is the sample low-passed and decimated by 2^k,
so a grain played at a high rate reads a level with no content above its own Nyquist (no aliasing). */
static const int SAMPLE_LEVELS = 4;
/** Columns of the min / max overview used by the display */
static const int OVERVIEW_COLUMNS = 1024;
/** Above this many samples (frames x channels, 64 MB of float) a stereo file is kept as mono. */
static const size_t MAX_SAMPLE_VALUES = 16u * 1024u * 1024u;

struct SampleBuffer {
	int channels = 0;
	unsigned sampleRate = 0;
	/** Frames of level 0 */
	size_t frames = 0;
	/** Gain that brings the sample to a comfortable level (RMS 0.4, no peak above 1) */
	float normGain = 1.f;
	/** Interleaved samples of each level; level k has (frames + 2^k - 1) >> k frames */
	std::vector<float> levels[SAMPLE_LEVELS];
	/** Per column: minimum, maximum (level 0, before normGain) */
	std::vector<float> overview;

	bool valid() const {
		return channels > 0 && frames > 0;
	}
	size_t levelFrames(int k) const {
		return levels[k].size() / (size_t) std::max(channels, 1);
	}
};

struct WavetableBank {
	/** The source sample (granular engine) */
	SampleBuffer sample;
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

/** Fills `s` from interleaved PCM (also used by analyzeWav; public for tests). */
void buildSampleBuffer(const float* pcm, size_t frames, unsigned channels, unsigned sampleRate, SampleBuffer& s);

int tableCountFor(TableCountMode mode, double seconds);

/** Decodes WAV bytes and builds the bank.
Returns nullptr and sets `error` on failure.
Slow (hundreds of ms): call it from a worker thread, never from process().
*/
WavetableBank* analyzeWav(const uint8_t* bytes, size_t size, TableCountMode mode, std::string* error, TableTone tone = TONE_SMOOTH);

} // namespace wa
