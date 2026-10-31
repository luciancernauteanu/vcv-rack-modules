#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Output protection, the last stage of the chain. No Rack dependencies (tested in tests/).

namespace wa {

/** Look-ahead peak limiter plus a final safety stage.

The signal is delayed by LOOKAHEAD (1.5 ms) while the limiter looks at what is coming. The gain is
the minimum of the needed gain over the last HOLD (50 ms, longer than one cycle down to 20 Hz, so
the gain stays steady inside a waveform), smoothed by a moving average as long as the look-ahead, so
it is already down by the time a peak reaches the output: no gain step, no click, and, because the
averaging window is shorter than the hold window, never an overshoot above the ceiling. Gain comes
back up over RELEASE (200 ms). Signals below the ceiling pass untouched (only delayed).

After it: a DC blocker (about 4 Hz) so a filter or drive can never park a DC offset on the output,
a guard that turns NaN or infinity into silence, and a hard clamp at the ceiling as the last word.
*/
class SafetyLimiter {
public:
	static constexpr float LOOKAHEAD = 0.0015f;
	static constexpr float HOLD = 0.05f;
	static constexpr float RELEASE = 0.2f;
	static constexpr float DC_BLOCK = 0.9995f;
	/** Sizes of the fixed buffers: enough for sample rates up to 384 kHz */
	static const int MAX_LOOKAHEAD = 2048;
	static const int MAX_HOLD = 32768;

	SafetyLimiter() {
		setSampleRate(44100.f);
	}

	void setSampleRate(float sr) {
		lookahead = std::min(std::max((int) std::lround(LOOKAHEAD * sr), 1), MAX_LOOKAHEAD - 1);
		hold = std::min(std::max((int) std::lround(HOLD * sr), lookahead + 2), MAX_HOLD - 1);
		releaseCoef = 1.f - std::exp(-1.f / (RELEASE * sr));
		reset();
	}

	/** Latency in samples */
	int getLatency() const {
		return lookahead;
	}

	void reset() {
		std::fill(delayL.begin(), delayL.end(), 0.f);
		std::fill(delayR.begin(), delayR.end(), 0.f);
		std::fill(boxBuf.begin(), boxBuf.end(), 1.f);
		boxSum = (double) lookahead;
		writePos = 0;
		head = tail = 0;
		time = 0;
		gain = 1.f;
		dcInL = dcOutL = dcInR = dcOutR = 0.f;
	}

	/** Current gain (1 = not limiting), for a meter */
	float getGain() const {
		return gain;
	}

	/** `ceiling` is the largest allowed absolute sample value. */
	void process(float l, float r, float ceiling, float* outL, float* outR) {
		if (!std::isfinite(l) || !std::isfinite(r)) {
			l = r = 0.f;
		}
		// Gain this sample needs on its own.
		float peak = std::max(std::fabs(l), std::fabs(r));
		float need = (peak > ceiling) ? ceiling / peak : 1.f;

		// Sliding minimum over the last `hold` needs (monotonic queue: indices increase, values rise).
		while (head != tail && dqVal[(tail - 1) & (MAX_HOLD - 1)] >= need)
			tail--;
		dqVal[tail & (MAX_HOLD - 1)] = need;
		dqIdx[tail & (MAX_HOLD - 1)] = time;
		tail++;
		while (dqIdx[head & (MAX_HOLD - 1)] <= time - hold)
			head++;
		float held = dqVal[head & (MAX_HOLD - 1)];
		time++;

		// Moving average of the held gain over the look-ahead: it slopes down as the peak approaches.
		int bi = (int) (time % lookahead);
		boxSum += held - boxBuf[bi];
		boxBuf[bi] = held;
		float target = (float) (boxSum / lookahead);

		// Falls follow the target at once; rises go back up slowly.
		gain = (target < gain) ? target : gain + (target - gain) * releaseCoef;

		// Delay line for the audio
		delayL[writePos] = l;
		delayR[writePos] = r;
		int readPos = writePos - lookahead;
		if (readPos < 0)
			readPos += MAX_LOOKAHEAD;
		float dl = delayL[readPos] * gain;
		float dr = delayR[readPos] * gain;
		writePos = (writePos + 1) % MAX_LOOKAHEAD;

		// DC blocker
		float ol = dl - dcInL + DC_BLOCK * dcOutL;
		dcInL = dl;
		dcOutL = ol;
		float orr = dr - dcInR + DC_BLOCK * dcOutR;
		dcInR = dr;
		dcOutR = orr;

		// Last word: never above the ceiling.
		*outL = std::min(std::max(ol, -ceiling), ceiling);
		*outR = std::min(std::max(orr, -ceiling), ceiling);
	}

private:
	int lookahead = 66;
	int hold = 2205;
	float releaseCoef = 0.f;
	float gain = 1.f;

	std::vector<float> delayL = std::vector<float>(MAX_LOOKAHEAD, 0.f);
	std::vector<float> delayR = std::vector<float>(MAX_LOOKAHEAD, 0.f);
	int writePos = 0;

	std::vector<float> boxBuf = std::vector<float>(MAX_LOOKAHEAD, 1.f);
	double boxSum = 0.0;

	std::vector<float> dqVal = std::vector<float>(MAX_HOLD, 1.f);
	std::vector<int64_t> dqIdx = std::vector<int64_t>(MAX_HOLD, 0);
	int64_t head = 0, tail = 0;
	int64_t time = 0;

	float dcInL = 0.f, dcOutL = 0.f, dcInR = 0.f, dcOutR = 0.f;
};

} // namespace wa
