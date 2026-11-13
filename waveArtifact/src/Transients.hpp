#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "WavetableBank.hpp"

// Transient (hit) detection and zero-crossing snapping on the loaded sample, for the SAMPLE engine. No Rack
// dependencies (tested in tests/test_sample.cpp). Runs on the UI thread when a sample loads and when the HITS slider
// moves (about 5 ms for 60 s of sample); the module hands the list to the audio thread for HIT.
//
// A hit is what the eye sees on the waveform: the level jumps up from the dip just before it. The sample (mono) is
// cut into hops of TRANSIENT_HOP frames (about 3 ms), and two levels are taken per hop, in dB: the plain one and the
// one of the second difference (the top end, where the click of an attack is, so a hat over a loud bass still
// shows). A hop starts a hit when either level stands transientRiseDb(sensitivity) above its lowest point over the
// TRANSIENT_VALLEY_SECONDS before, and the hit's peak is no more than transientRangeDb(sensitivity) under the
// loudest around it (TRANSIENT_LOCAL_SECONDS each side; for a top-end hit, on both levels). The next hit needs the
// rise to have settled first and transientGapSeconds to pass. Unlike a threshold over the average onset strength,
// dense passages (rolls, fast hats) do not raise the bar: every hit only has to stand out from what is just before
// it. And unlike a range under the loudest of the whole sample, the shakes between a shaker's beats are not hits at
// low settings however loud the whole loop is: at 0 % a 70 bpm shaker loop gives its 16 beats.
//
// Exact place: from where the rise began (a slow swell is walked back to its start), the first frame half way (in
// dB) from the dip to the peak, moved back to the zero crossing before it, so a slice starting there starts clean.

namespace wa {

static const int TRANSIENT_HOP = 128;
/** HITS slider 0..1 (0 % = only the main hits, 100 % = the softest too): how far a hop must rise above the dip before
it, dB (8 dB at 0 %, 6.3 at 50 %, 3 at 100 %; at Normal a bass swelling back after a kick, about 5 dB, is not a hit) */
inline double transientRiseDb(double s) {
	s = std::min(std::max(s, 0.0), 1.0);
	return 8.0 * std::pow(0.375, s * s);
}
/** How far under the loudest hit around it (TRANSIENT_LOCAL_SECONDS each side) a hit may be, dB (6 dB at 0 %, 19 at
50 %, 60 at 100 %). Judged against its neighbours, not the whole sample: a shake next to a beat is not a hit at low
settings, however loud the shakes are. */
inline double transientRangeDb(double s) {
	return 6.0 * std::pow(10.0, std::min(std::max(s, 0.0), 1.0));
}
/** Shortest time between two hits (250 ms at 0 %: an 8th at 120 bpm, so a shake just after a beat is not a hit; 125 ms
at 20 %, 42 ms at 50 %, 12 ms at 100 %) */
inline double transientGapSeconds(double s) {
	s = std::min(std::max(s, 0.0), 1.0);
	return s < 0.5 ? 0.25 * std::pow(0.042 / 0.25, 2.0 * s) : 0.042 * std::pow(0.012 / 0.042, 2.0 * s - 1.0);
}
/** The neighbourhood a hit is compared with, each side */
static constexpr double TRANSIENT_LOCAL_SECONDS = 0.6;
/** Never a hit this far under the loudest of the whole sample (silence, hiss) */
static constexpr double TRANSIENT_FLOOR_DB = 60.0;
/** The HITS slider's default */
static constexpr float TRANSIENT_DEFAULT_SENSITIVITY = 0.5f;
/** The dip a hit rises from: the lowest level over this long before it */
static constexpr double TRANSIENT_VALLEY_SECONDS = 0.02;
/** Zero-crossing snap: how far a place may move to reach one (beyond, it stays where it is) */
static constexpr double ZERO_SNAP_SECONDS = 0.005;

inline float monoFrame(const SampleBuffer& S, long i) {
	const std::vector<float>& d = S.levels[0];
	return S.channels == 2 ? 0.5f * (d[(size_t) (2 * i)] + d[(size_t) (2 * i + 1)]) : d[(size_t) i];
}

/** The zero crossing nearest to `frame` (the frame of the two around the sign change that is closer to 0), within
`maxFrames`; `frame` itself when there is none. */
inline long nearestZeroCrossing(const SampleBuffer& S, long frame, long maxFrames) {
	long n = (long) S.frames;
	if (n < 2)
		return frame;
	frame = std::min(std::max(frame, 0L), n - 1);
	auto crossingAt = [&](long i, long* at) {
		// A sign change between i - 1 and i (or an exact 0 at i)
		if (i < 1 || i >= n)
			return false;
		float a = monoFrame(S, i - 1), b = monoFrame(S, i);
		if (b == 0.f) {
			*at = i;
			return true;
		}
		if ((a < 0.f) == (b < 0.f))
			return false;
		*at = std::fabs(a) < std::fabs(b) ? i - 1 : i;
		return true;
	};
	for (long d = 0; d <= maxFrames; d++) {
		long at;
		if (crossingAt(frame + d, &at) || crossingAt(frame - d, &at))
			return at;
	}
	return frame;
}

/** The zero crossing at or before `frame` (up to `maxFrames` back), else `frame` */
inline long zeroCrossingBefore(const SampleBuffer& S, long frame, long maxFrames) {
	long n = (long) S.frames;
	frame = std::min(std::max(frame, 0L), std::max(n - 1, 0L));
	for (long i = frame; i >= 1 && frame - i <= maxFrames; i--) {
		float a = monoFrame(S, i - 1), b = monoFrame(S, i);
		if (b == 0.f || (a < 0.f) != (b < 0.f))
			return std::fabs(a) < std::fabs(b) ? i - 1 : i;
	}
	return frame;
}

/** The largest value of `v` within `w` places each side of every place (a monotonic queue: linear time) */
inline std::vector<double> slidingMax(const std::vector<double>& v, long w) {
	long n = (long) v.size();
	std::vector<double> out((size_t) n);
	std::vector<long> q((size_t) n);
	long head = 0, tail = 0, next = 0;
	for (long i = 0; i < n; i++) {
		// Take in everything up to i + w, drop what fell behind i - w
		for (; next < n && next <= i + w; next++) {
			while (tail > head && v[(size_t) q[(size_t) (tail - 1)]] <= v[(size_t) next])
				tail--;
			q[(size_t) tail++] = next;
		}
		while (q[(size_t) head] < i - w)
			head++;
		out[(size_t) i] = v[(size_t) q[(size_t) head]];
	}
	return out;
}

/** The hit nearest to `frame` in a sorted list, or -1 when the list is empty */
inline long nearestHit(const std::vector<long>& hits, double frame) {
	if (hits.empty())
		return -1;
	auto it = std::lower_bound(hits.begin(), hits.end(), (long) std::ceil(frame));
	if (it == hits.end())
		return hits.back();
	if (it == hits.begin())
		return *it;
	long after = *it, before = *(it - 1);
	return (frame - (double) before <= (double) after - frame) ? before : after;
}

/** Hits of the sample, frames of level 0, sorted */
inline std::vector<long> findTransients(const SampleBuffer& S, float sensitivity) {
	std::vector<long> out;
	if (!S.valid() || S.frames < (size_t) (4 * TRANSIENT_HOP))
		return out;
	long n = (long) S.frames;
	long hops = n / TRANSIENT_HOP;
	// Two outlines of the waveform per hop, peak followers (dB): the plain one falls over 50 ms (so even a sub-bass
	// does not ripple it); the top end's follows the second difference (a steep high-pass: a 55 Hz bass is 84 dB down
	// there, an 8 kHz hat is not) and falls over 10 ms, so a hat right after a kick still shows.
	std::vector<double> lb((size_t) hops), lh((size_t) hops);
	const double relB = std::exp(-1.0 / (0.05 * S.sampleRate)), relH = std::exp(-1.0 / (0.01 * S.sampleRate));
	double fb = 0.0, fh = 0.0;
	float x1 = 0.f, x2 = 0.f;
	double loudest = -400.0, loudestHigh = -400.0;
	for (long h = 0; h < hops; h++) {
		double mb = 0.0, mh = 0.0;
		for (long i = h * TRANSIENT_HOP; i < (h + 1) * TRANSIENT_HOP; i++) {
			float x = monoFrame(S, i);
			fb = std::max((double) std::fabs(x), fb * relB);
			fh = std::max((double) std::fabs(x - 2.f * x1 + x2), fh * relH);
			x2 = x1;
			x1 = x;
			mb = std::max(mb, fb);
			mh = std::max(mh, fh);
		}
		lb[(size_t) h] = 20.0 * std::log10(mb + 1e-10);
		lh[(size_t) h] = 20.0 * std::log10(mh + 1e-10);
		loudest = std::max(loudest, lb[(size_t) h]);
		loudestHigh = std::max(loudestHigh, lh[(size_t) h]);
	}
	double rise = transientRiseDb(sensitivity);
	double range = transientRangeDb(sensitivity);
	// Anything under this counts as silence for the dips (so a hit out of silence rises from here, not -200 dB)
	double silence = loudest - 90.0, silenceHigh = loudestHigh - 90.0;
	long valley = std::max(2L, (long) std::lround(TRANSIENT_VALLEY_SECONDS * S.sampleRate / TRANSIENT_HOP));
	// The loudest of each outline around every hop (TRANSIENT_LOCAL_SECONDS each side)
	long local = std::max(1L, (long) std::lround(TRANSIENT_LOCAL_SECONDS * S.sampleRate / TRANSIENT_HOP));
	std::vector<double> localB = slidingMax(lb, local), localH = slidingMax(lh, local);
	long gap = std::max(1L, (long) std::lround(transientGapSeconds(sensitivity) * S.sampleRate / TRANSIENT_HOP));
	long peakAhead = std::max(1L, (long) std::lround(0.008 * S.sampleRate / TRANSIENT_HOP));
	// After a hit: wait for its peak to pass (armed again), then the next hit is measured from the dip after that
	// peak, not from the quiet before the last hit.
	bool armed = true, trigHigh = false;
	double lastStrength = -400.0;
	bool lastByB = false;
	long lastHop = -gap - 1, afterPeak = 0;
	for (long h = 0; h < hops; h++) {
		if (!armed) {
			// The outline that rose has turned down (or the hit is long past: a swelling sound under it keeps the
			// other outline rising, which must not hide the next hit)
			const std::vector<double>& l = trigHigh ? lh : lb;
			if ((h > 0 && l[(size_t) h] < l[(size_t) (h - 1)]) || h - lastHop > 2 * valley) {
				armed = true;
				afterPeak = h;
			}
			continue;
		}
		double vb = silence, vh = silenceHigh;
		long from = std::max(afterPeak, h - valley);
		if (from < h) {
			vb = vh = 1e9;
			for (long k = from; k < h; k++) {
				vb = std::min(vb, lb[(size_t) k]);
				vh = std::min(vh, lh[(size_t) k]);
			}
			vb = std::max(vb, silence);
			vh = std::max(vh, silenceHigh);
		}
		double rb = lb[(size_t) h] - vb, rh = lh[(size_t) h] - vh;
		// The hit's peak just ahead, against the loudest of the sample, for the outline that rose
		double pb = -400.0, ph = -400.0;
		for (long k = h; k < std::min(hops, h + peakAhead); k++) {
			pb = std::max(pb, lb[(size_t) k]);
			ph = std::max(ph, lh[(size_t) k]);
		}
		bool byB = rb >= rise && pb >= std::max(localB[(size_t) h] - range, loudest - TRANSIENT_FLOOR_DB);
		// The top end finds what the plain outline does not show rising (a hat over a bass), but the hit must not be
		// soft on the plain outline either: a bright shake between beats is loud on the top end only.
		bool byH = rh >= rise && ph >= std::max(localH[(size_t) h] - range, loudestHigh - TRANSIENT_FLOOR_DB)
			&& pb >= localB[(size_t) h] - range;
		if (!byB && !byH)
			continue;
		// How strong, against the loudest of the sample. Within the gap after the last hit, the stronger one wins: a
		// bump in a noisy tail just before a real hit must not take its place.
		double strength = std::max(byB ? pb - loudest : -400.0, byH ? ph - loudestHigh : -400.0);
		bool replace = false;
		if (h - lastHop < gap) {
			if (out.empty() || strength < lastStrength + 3.0)
				continue;
			replace = true;
		}
		armed = false;
		lastHop = h;
		bool keepPlace = replace && lastByB && !byB; // the same swell seen again by the top end: keep where it starts
		lastStrength = strength;
		lastByB = byB;
		// Exact place: from where the rise began, the first frame of the outline that rose that is half way (in dB)
		// from the dip to the peak; then back to the zero crossing before it.
		// Placed on the plain outline when it clearly rose too (smooth: it follows a slow swell back to its start). The
		// top end's when the plain one only just rose and the top end far more: a hat over a bass, where the plain
		// outline is mostly the bass, and half way up it a bass peak comes before the hat.
		bool high = byH && (!byB || (rb < 2.0 * rise && rh > 2.0 * rb));
		trigHigh = high;
		// Where the rise began: a hit is often recognised a few hops into its attack (it had to rise far enough), and
		// starting there would cut the attack.
		const std::vector<double>& outline = high ? lh : lb;
		long k = h;
		long back = std::max(1L, (long) std::lround(0.08 * S.sampleRate / TRANSIENT_HOP)); // a slow swell (a shaker): up to 80 ms
		while (k > afterPeak && k > h - back && k > 0 && outline[(size_t) (k - 1)] < outline[(size_t) k] - 0.5)
			k--;
		// The dip is the lowest before the rise began, not before the hop that crossed: in a slow swell that hop is far
		// up, and a dip measured inside the swell would put the half-way level late. Before the sample is silence.
		double dip = high ? vh : vb;
		for (long j = std::max(afterPeak, k - valley); j < k; j++)
			dip = std::min(dip, std::max(outline[(size_t) j], high ? silenceHigh : silence));
		if (k - valley < 0 && afterPeak == 0)
			dip = high ? silenceHigh : silence;
		double level = std::pow(10.0, 0.5 * (dip + (high ? ph : pb)) / 20.0);
		long first = std::max(0L, (k - 1) * TRANSIENT_HOP), last = std::min(n, (h + 2) * TRANSIENT_HOP); // one hop on: a hit that starts at the end of hop h
		long at = h * TRANSIENT_HOP;
		for (long i = std::max(first, 2L); i < last; i++) {
			float x = monoFrame(S, i);
			double v = high ? std::fabs(x - 2.f * monoFrame(S, i - 1) + monoFrame(S, i - 2)) : std::fabs(x);
			if (v > level) {
				at = i;
				break;
			}
		}
		// A zero crossing just before (up to 1 ms back: over a low sound there may be none near, and a hit must not
		// start early)
		at = zeroCrossingBefore(S, at, (long) (0.001 * S.sampleRate));
		if (replace && keepPlace)
			continue;
		if (replace) {
			out.back() = std::max(at, out.size() > 1 ? out[out.size() - 2] + 1 : 0L);
			continue;
		}
		if (!out.empty() && at <= out.back())
			continue;
		out.push_back(at);
	}
	return out;
}

/** A hit's loudness: the peak of the sample (mono, level 0) over `frames` frames from `frame`, in dB (-200 for
silence) */
inline double hitPeakDb(const SampleBuffer& S, long frame, long frames) {
	if (!S.valid())
		return -200.0;
	long a = std::max(frame, 0L), b = std::min(frame + std::max(frames, 1L), (long) S.frames);
	float peak = 0.f;
	for (long i = a; i < b; i++)
		peak = std::max(peak, std::fabs(monoFrame(S, i)));
	return peak > 1e-10f ? 20.0 * std::log10((double) peak) : -200.0;
}

/** Hop of the onset strength curve (about 5.8 ms at 44.1 kHz) */
static const int ONSET_HOP = 256;

/** The sample's onset strength, one value per ONSET_HOP frames: how much its level rose there, on the plain level and
the top end's (where a hat's or a click's attack is), in dB, rises only. A slow average (1 s) is taken off, so a swell
or a steady texture is not a pulse, and it is smoothed a little (about 12 ms), so a hit a few ms off the grid still
counts. Loud attacks weigh more than soft shakes, which a list of hits cannot tell apart. For the tempo
(LoopLayout.hpp). */
inline std::vector<float> onsetStrength(const SampleBuffer& S) {
	std::vector<float> env;
	if (!S.valid() || S.frames < (size_t) (16 * ONSET_HOP))
		return env;
	long hops = (long) S.frames / ONSET_HOP;
	std::vector<double> eb((size_t) hops), eh((size_t) hops);
	float x1 = 0.f, x2 = 0.f;
	double loudB = -400.0, loudH = -400.0;
	for (long h = 0; h < hops; h++) {
		double sb = 0.0, sh = 0.0;
		for (long i = h * ONSET_HOP; i < (h + 1) * ONSET_HOP; i++) {
			float x = monoFrame(S, i);
			float d = x - 2.f * x1 + x2;
			x2 = x1;
			x1 = x;
			sb += (double) x * x;
			sh += (double) d * d;
		}
		eb[(size_t) h] = 10.0 * std::log10(sb / ONSET_HOP + 1e-14);
		eh[(size_t) h] = 10.0 * std::log10(sh / ONSET_HOP + 1e-14);
		loudB = std::max(loudB, eb[(size_t) h]);
		loudH = std::max(loudH, eh[(size_t) h]);
	}
	// Nothing under 60 dB below the loudest counts (noise floors, reverb tails)
	std::vector<double> flux((size_t) hops, 0.0);
	for (long h = 1; h < hops; h++) {
		double b0 = std::max(eb[(size_t) (h - 1)], loudB - 60.0), b1 = std::max(eb[(size_t) h], loudB - 60.0);
		double h0 = std::max(eh[(size_t) (h - 1)], loudH - 60.0), h1 = std::max(eh[(size_t) h], loudH - 60.0);
		flux[(size_t) h] = std::max(b1 - b0, 0.0) + std::max(h1 - h0, 0.0);
	}
	// Minus its 1 s average, rises only
	long w = std::max(1L, (long) std::lround(0.5 * S.sampleRate / ONSET_HOP));
	std::vector<double> prefix((size_t) hops + 1, 0.0);
	for (long h = 0; h < hops; h++)
		prefix[(size_t) h + 1] = prefix[(size_t) h] + flux[(size_t) h];
	std::vector<double> rel((size_t) hops);
	for (long h = 0; h < hops; h++) {
		long a = std::max(0L, h - w), b = std::min(hops, h + w + 1);
		double mean = (prefix[(size_t) b] - prefix[(size_t) a]) / (double) (b - a);
		rel[(size_t) h] = std::max(flux[(size_t) h] - mean, 0.0);
	}
	// Smoothed: Gaussian, sigma about 12 ms
	double sigma = std::max(0.5, 0.012 * S.sampleRate / ONSET_HOP);
	int r = (int) std::ceil(3.0 * sigma);
	std::vector<double> kern((size_t) (2 * r + 1));
	double ks = 0.0;
	for (int k = -r; k <= r; k++)
		ks += kern[(size_t) (k + r)] = std::exp(-0.5 * k * k / (sigma * sigma));
	env.assign((size_t) hops, 0.f);
	for (long h = 0; h < hops; h++) {
		double s = 0.0;
		for (int k = -r; k <= r; k++) {
			long j = h + k;
			if (j >= 0 && j < hops)
				s += kern[(size_t) (k + r)] * rel[(size_t) j];
		}
		env[(size_t) h] = (float) (s / ks);
	}
	return env;
}

} // namespace wa
