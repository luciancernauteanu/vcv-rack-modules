#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Drive.hpp"
#include "Limiter.hpp"
#include "PrismaCore.hpp"
#include "Shapes.hpp"

// Nur Prisma's engine: four stereo filter cores, their places (CUTOFF, SPREAD, SHAPE), PING and the character
// (ANALOG, DIGITAL). No Rack dependencies (tested in tests/). Audio in engine units: 1.0 = 5 V.

namespace np {

static const int NUM_CORES = 4;

enum PingMode {
	PING_ALL,
	PING_CYCLE,
	PING_RANDOM,
	NUM_PING_MODES
};

/** Where WIDTH places core i in the stereo field (times WIDTH): low core left, top core right, the middle two near the
centre on the other sides. */
inline float widthPlace(int i) {
	static const float PAN[4] = {-1.f, 1.f / 3.f, -1.f / 3.f, 1.f};
	return PAN[i];
}

/** Where DRIVE sits (right-click menu) */
enum DrivePosition {
	/** Before the cores (the original): the cores filter the harmonics it adds */
	DRIVE_PRE,
	/** After the cores (on the wet signal): the harmonics stay, pings and resonances get driven */
	DRIVE_POST,
	/** Both, each at 70%: two stages pushed a little sound cleaner than one pushed hard */
	DRIVE_BOTH,
	NUM_DRIVE_POSITIONS
};

/** A cheap loudness weighting for the level compensation detector (a K-weighting-like curve): a 1-pole high-pass at
120 Hz (the ear hears little below it) and a +4 dB high shelf from ~1.7 kHz (it is most sensitive at 2-4 kHz).
Squared and averaged by the caller. */
struct LoudnessWeight {
	float hpLp = 0.f;
	float shelfLp = 0.f;
	float hpCoef = 0.f;
	float shelfCoef = 0.f;
	static constexpr float SHELF_GAIN = 0.5849625f; // 10^(4/20) - 1

	void setSampleRate(float sr) {
		hpCoef = 1.f - std::exp(-6.2831853f * 120.f / sr);
		shelfCoef = 1.f - std::exp(-6.2831853f * 1700.f / sr);
	}
	float process(float x) {
		hpLp += hpCoef * (x - hpLp);
		float h = x - hpLp;
		shelfLp += shelfCoef * (h - shelfLp);
		return h + SHELF_GAIN * (h - shelfLp);
	}
};

/** Output level compensation (right-click menu) */
enum LevelComp {
	LEVEL_OFF,
	/** +-6 dB */
	LEVEL_GENTLE,
	/** -12 .. +24 dB: narrow bands keep the input's loudness */
	LEVEL_FULL,
	NUM_LEVEL_COMPS
};

/** Poly input, one voice per core: voice v goes to core v mod 4 (voices 5..16 wrap and add to cores 1..4). */
inline void routeVoices(const float* voices, int count, float out[NUM_CORES]) {
	for (int i = 0; i < NUM_CORES; i++)
		out[i] = 0.f;
	for (int v = 0; v < count; v++)
		out[v % NUM_CORES] += voices[v];
}

/** What the module hands over: knob values (smoothed in here), CV (used as is), and menu settings. */
struct EngineParams {
	// Knobs
	float cutoff = 0.6667f;
	float spread = 0.3f;
	float res = 0.35f;
	float type = 0.f;
	float stack = 0.f;
	float drive = 0.f;
	float width = 0.5f;
	float mix = 1.f;
	/** Output gain (linear; the GAIN knob, -12 .. +12 dB), before the safety limiter */
	float outGain = 1.f;
	float noise = 0.f;
	float color = 0.3333f;
	float analog = 0.5f;
	float digital = 0.f;
	float strum = 0.f;
	float strike = 0.35f;
	/** 0 = off; up to 1 = each core narrowed to a steep 1/24-octave band */
	float band = 0.f;
	/** Each core's own share of SPREAD (1 = the shape's place; 0 = on CUTOFF; negative = the other side) */
	float coreSpread[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	/** Per-core TYPE / RES / BAND: used for a core whose "own" flag is set (it was edited in focus), else the core
	follows the global knob. In the smoothed copy (`PrismaEngine::s`) these hold each core's effective value. */
	float coreType[NUM_CORES] = {0.f, 0.f, 0.f, 0.f};
	float coreRes[NUM_CORES] = {0.35f, 0.35f, 0.35f, 0.35f};
	float coreBand[NUM_CORES] = {0.f, 0.f, 0.f, 0.f};
	bool ownType[NUM_CORES] = {};
	bool ownRes[NUM_CORES] = {};
	bool ownBand[NUM_CORES] = {};
	/** Per-core level (linear, 1 = as is) */
	float coreLevel[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	/** Per-core pan -1..1: once owned it replaces WIDTH's placement of the core, and moves both of its channels */
	float corePan[NUM_CORES] = {0.f, 0.f, 0.f, 0.f};
	bool ownPan[NUM_CORES] = {};
	/** Per-core CUTOFF (0..1 like the knob): once owned the core sits there, out of its SPREAD place; global CUTOFF
	no longer moves it (V/OCT still does) */
	float coreCutoff[NUM_CORES] = {0.6667f, 0.6667f, 0.6667f, 0.6667f};
	bool ownCutoff[NUM_CORES] = {};
	int shape = SHAPE_EVEN;
	// CV, not smoothed: pitch in octaves and RES (0..1 units) per core, SPREAD and STACK (0..1 units)
	float pitchCv[NUM_CORES] = {};
	float resCv[NUM_CORES] = {};
	float spreadCv = 0.f;
	float stackCv = 0.f;
	// Menu
	int scale = 0;
	int chord = 0;
	int pingMode = PING_ALL;
	bool quantizeCutoff = false;
	int levelComp = LEVEL_FULL;
	int driveVoice = VOICE_WARM;
	/** NOISE follows what: 1 = each core's ping, 2 = the input level, 3 = ping or input */
	int noiseFollow = 3;
	/** How much NOISE follows (0 = plain noise, no envelope; 1 = only while a ping / the input sounds, short release) */
	float noiseFollowAmt = 0.f;
	/** NOISE type: 0 = coloured (white, pink, brown, crackle), 1 = digital shift register */
	int noiseType = 0;
	/** Level compensation detector: 0 = flat RMS, 1 = ear (weighted like a loudness meter) */
	int levelWeight = 1;
	/** Low-pass gate: depth 0..1 (0 = off), release time 0..1 (40 ms .. 1.5 s), source 0 = ping, 1 = input, 2 = both */
	/** 24 dB slope per core: a second low-pass stage after the core */
	bool slope24[NUM_CORES] = {};
	float lpg = 0.f;
	float lpgDecay = 0.5f;
	int lpgSource = 2;
	/** SHAPE Chord / Scale in just intonation (pure ratios) instead of 12-TET */
	bool justTuning = false;
	int drivePos = DRIVE_PRE;
};

/** The excitation of one ping. The mallet is an impulse through two one-pole low-passes ("felt"): unit area, so low
notes always ring at full level, and STRIKE lowers the felt's corner from 20 kHz (a hard click) to 300 Hz (soft), so
higher notes ring softer and darker, smoothly (no dead notes, unlike a fixed-length burst whose spectrum has nulls).
Past half of STRIKE a windowed noise burst (breath) takes over. */
struct PingBurst {
	int pos = 0;
	int malletLen = 0;
	int noiseLen = 0;
	float amp = 0.f;
	float noiseBlend = 0.f;
	float coef = 1.f;
	float lp1 = 0.f, lp2 = 0.f;
	bool active = false;
	/** With DRIVE / ANALOG the strike is saturated: generated once at the start (and replayed), energy-matched */
	bool buffered = false;
	std::vector<float> buf;
	static const int MAX_BUF = 4608;

	/** Felt corner in Hz for STRIKE 0..1 */
	static float feltHz(float strike) {
		float m = std::min(clamp01(strike), 0.5f) * 2.f;
		return 20000.f * std::pow(0.015f, m);
	}

	/** `shapeG` > 1: the strike is saturated (tanh of gain shapeG on the burst, relative to its own peak), then scaled
	back to the same energy, so the ring keeps its loudness and gains harmonics: a brighter, grittier hit. */
	void start(float amplitude, float strike, float sampleRate, Rng& rng, float shapeG = 1.f) {
		strike = clamp01(strike);
		float fm = feltHz(strike);
		coef = (fm >= 0.45f * sampleRate) ? 1.f : 1.f - std::exp(-6.2831853f * fm / sampleRate);
		// Until the felt's response has decayed to e^-14
		malletLen = 1 + (int) (14.f / coef);
		lp1 = lp2 = 0.f;
		noiseBlend = std::max(strike - 0.5f, 0.f) * 2.f;
		noiseLen = (int) ((0.002f + 0.02f * noiseBlend) * sampleRate);
		amp = amplitude;
		pos = 0;
		active = true;
		buffered = false;
		if (shapeG > 1.01f) {
			int len = std::min(std::max(malletLen, noiseLen), (int) MAX_BUF);
			if ((int) buf.size() < MAX_BUF)
				buf.assign(MAX_BUF, 0.f);
			float peak = 0.f, e0 = 0.f;
			for (int k = 0; k < len; k++) {
				buf[k] = gen(rng);
				peak = std::max(peak, std::fabs(buf[k]));
				e0 += buf[k] * buf[k];
			}
			if (peak > 0.f) {
				float norm = 1.f / std::tanh(shapeG), e1 = 0.f;
				for (int k = 0; k < len; k++) {
					buf[k] = peak * std::tanh(shapeG * buf[k] / peak) * norm;
					e1 += buf[k] * buf[k];
				}
				float back = e1 > 0.f ? std::sqrt(e0 / e1) : 1.f;
				for (int k = 0; k < len; k++)
					buf[k] *= back;
			}
			pos = 0;
			lp1 = lp2 = 0.f;
			buffered = true;
		}
	}

	/** The next sample of the unshaped burst (not scaled by the amplitude) */
	float gen(Rng& rng) {
		float e = 0.f;
		if (pos < malletLen) {
			float in = (pos == 0) ? 1.f : 0.f;
			lp1 += coef * (in - lp1);
			lp2 += coef * (lp1 - lp2);
			e = lp2 * (1.f - noiseBlend);
		}
		if (noiseBlend > 0.f && pos < noiseLen) {
			float w = 0.5f - 0.5f * std::cos(6.2831853f * (pos + 0.5f) / noiseLen);
			// Unit expected spectrum: noise of variance 1/3 scaled by 1/sqrt(sum w^2) = 1/sqrt(3 len / 8)
			e += noiseBlend * w * rng.uniform() * std::sqrt(3.f) / std::sqrt(0.375f * noiseLen);
		}
		pos++;
		return e;
	}

	float next(Rng& rng) {
		float e;
		if (buffered) {
			e = pos < MAX_BUF ? buf[pos] : 0.f;
			pos++;
		}
		else
			e = gen(rng);
		if (pos >= std::max(malletLen, noiseLen))
			active = false;
		return e * amp;
	}
};

/** DRIVE before the cores. Always the same latency (DRIVE_LATENCY samples), driven or not: while DRIVE rests at 0
the oversampled drive sleeps and a plain delay stands in; waking up, it fades in after it has warmed up. */
struct DriveStage {
	static const int DRIVE_LATENCY = 31;
	WarmDrive drive;
	float delay[64] = {};
	int pos = 0;
	bool awake = false;
	int warm = 0;
	int idle = 0;

	void reset() {
		drive.reset();
		std::fill(delay, delay + 64, 0.f);
		pos = 0;
		awake = false;
	}

	float process(float x, float amount, int graceSamples) {
		delay[pos] = x;
		float delayed = delay[(pos - DRIVE_LATENCY) & 63];
		pos = (pos + 1) & 63;
		if (amount > 0.f)
			idle = 0;
		else if (awake && ++idle > graceSamples)
			awake = false;
		if (amount > 0.f && !awake) {
			awake = true;
			drive.reset();
			warm = 0;
		}
		if (!awake)
			return delayed;
		float y = drive.process(x, amount);
		if (warm < 128) {
			warm++;
			float t = std::min(std::max((warm - 64) / 64.f, 0.f), 1.f);
			return delayed + (y - delayed) * t;
		}
		return y;
	}
};

struct EngineOutput {
	float l = 0.f, r = 0.f;
	float cores[NUM_CORES] = {};
	/** Core frequencies, 0..1 = 20 Hz..20 kHz (log), for the display */
	float corePos[NUM_CORES] = {};
};

class PrismaEngine {
public:
	static const int BLOCK = 16;

	PrismaEngine() {
		shiftNoise[1].reg = 0x1A2B3u;
		noise[0].rng.seed(0x2545F491u);
		noise[1].rng.seed(0x6C8E9CF5u);
		pingRng.seed(0x51ED27Fu);
		setSampleRate(44100.f);
		seedCharacter(0x1234567u);
		seedRandomShape(0x89abcdu);
	}

	void setSampleRate(float sr) {
		sampleRate = sr;
		limiter.setSampleRate(sr);
		for (int c = 0; c < 2; c++) {
			for (int i = 0; i < NUM_CORES; i++)
				drives[c][i].drive.sampleRate = sr;
			postDrives[c].drive.sampleRate = sr;
		}
		driveFadeStep = 1.f / (0.005f * sr);
		randomGlide = 1.f - std::exp(-(float) BLOCK / (0.04f * sr));
		hissGateCoef = 1.f - std::exp(-(float) BLOCK / (0.05f * sr));
		lpgEnvDecay = std::exp(-1.f / (0.03f * sr));
		followDecay = std::exp(-1.f / (0.03f * sr));
		outGainCoef = 1.f - std::exp(-1.f / (0.005f * sr));
		for (int c = 0; c < 2; c++) {
			weightIn[c].setSampleRate(sr);
			weightOut[c].setSampleRate(sr);
		}
		blockCoef = 1.f - std::exp(-(float) BLOCK / (0.005f * sr));
		compCoef = 1.f - std::exp(-1.f / (0.3f * sr));
		compSmooth = 1.f - std::exp(-(float) BLOCK / (0.15f * sr));
		compRelax = 1.f - std::exp(-(float) BLOCK / (1.f * sr));
		grace = (int) sr;
		blockPos = 0;
	}

	/** The analog mismatch of this instance: each core slightly off in pitch, Q and level (saved in the patch). */
	void seedCharacter(uint32_t seed) {
		characterSeed = seed;
		Rng r;
		r.seed(seed);
		for (int i = 0; i < NUM_CORES; i++) {
			mismatchOct[i] = r.uniform() * (8.f / 1200.f);
			mismatchQ[i] = 1.f + 0.06f * r.uniform();
			mismatchGain[i] = std::pow(10.f, 0.5f * r.uniform() / 20.f);
			thermalRng[i].seed(seed * 2246822519u + 3266489917u * (i + 1));
			thermalFrom[i][0] = thermalFrom[i][1] = thermalTo[i][0] = thermalTo[i][1] = 0.f;
			thermalPhase[i][0] = thermalPhase[i][1] = 0.f;
		}
		hissRng.seed(seed ^ 0xa5a5a5a5u);
	}
	uint32_t getCharacterSeed() const {
		return characterSeed;
	}

	/** The frozen places of SHAPE Random (saved in the patch). `glide`: move there over ~40 ms (a re-roll while
	playing); else jump (load, start). */
	void seedRandomShape(uint32_t seed, bool glide = false) {
		randomSeed = seed;
		Rng r;
		r.seed(seed);
		for (int i = 0; i < NUM_CORES; i++) {
			randomTarget[i] = r.uniform();
			if (!glide)
				randomPlaces[i] = randomTarget[i];
		}
	}
	uint32_t getRandomSeed() const {
		return randomSeed;
	}

	void reset() {
		for (int c = 0; c < 2; c++)
			for (int i = 0; i < NUM_CORES; i++) {
				cores[c][i].reset();
				drives[c][i].reset();
				postDrives[c % 2].reset();
				holdVal[c][i] = 0.f;
				holdCount[c][i] = 0;
				bands[c][i].reset();
			}
		limiter.reset();
		compGain = 1.f;
		compStep = 0.f;
		compDb = 0.f;
		silentBlocks = 0;
		energyIn = energyOut = 0.f;
	}

	/** Ping one core (poly PING channel i), velocity 0..1. */
	void ping(int core, float velocity) {
		if (core < 0 || core >= NUM_CORES || velocity <= 0.f)
			return;
		pending[core] = 0;
		pendingVel[core] = velocity;
	}

	/** A mono PING trigger: all cores (strummed), the next one, or a random one, per the ping mode. */
	void pingAll(float velocity) {
		if (velocity <= 0.f)
			return;
		// Cores in frequency order (low to high), for strum and cycle
		int order[NUM_CORES] = {0, 1, 2, 3};
		std::sort(order, order + NUM_CORES, [this](int a, int b) { return oct[a] < oct[b]; });
		switch (p.pingMode) {
			case PING_CYCLE:
				ping(order[cycleIndex % NUM_CORES], velocity);
				cycleIndex = (cycleIndex + 1) % NUM_CORES;
				break;
			case PING_RANDOM:
				ping((int) (pingRng.unit() * NUM_CORES) % NUM_CORES, velocity);
				break;
			default: {
				float strum = std::min(std::max(p.strum, -1.f), 1.f);
				int gap = (int) (strum * strum * 0.15f * sampleRate);
				for (int n = 0; n < NUM_CORES; n++) {
					int core = (strum >= 0.f) ? order[n] : order[NUM_CORES - 1 - n];
					pending[core] = n * gap;
					pendingVel[core] = velocity;
				}
				break;
			}
		}
	}

	/** Settings for the next block (knobs are smoothed in here). */
	EngineParams p;

	/** One sample. `inL` / `inR`: one input per core when `perCore` (poly), else only [0] is used for all cores.
	`dryL` / `dryR`: the unprocessed input for MIX. */
	void process(const float* inL, const float* inR, bool perCore, float dryL, float dryR, EngineOutput& out) {
		if (blockPos == 0)
			updateBlock();
		blockPos = (blockPos + 1) % BLOCK;

		// Pings due now
		for (int i = 0; i < NUM_CORES; i++) {
			if (pending[i] >= 0 && pending[i]-- == 0) {
				// DRIVE (before the cores) and ANALOG act on the strike itself: it is saturated, so a driven ping is
				// brighter and grittier; ANALOG also gives every ping a little life (a random +-12% in strength)
				float preAmt = posNow == DRIVE_PRE ? s.drive : posNow == DRIVE_BOTH ? BOTH_SHARE * s.drive : 0.f;
				float shapeG = std::exp(preAmt * 2.48490665f) * (1.f + 1.5f * s.analog);
				float vary = s.analog > 0.f ? 1.f + 0.12f * s.analog * pingRng.uniform() : 1.f;
				burst[i].start(pendingVel[i] * vary * PING_LEVEL / pingResp[i], s.strike, sampleRate, pingRng, shapeG);
				// The ping also lights the core's vactrol (a short pulse, ~6 ms)
				lpgPulse[i] = (int) (0.006f * sampleRate);
				lpgVel[i] = pendingVel[i];
				// NOISE follows the ping: the core's ping envelope (velocity, decaying like the ring; see updateBlock)
				pingEnv[i] = std::max(pingEnv[i], std::min(1.f, 2.f * pendingVel[i]));
				pingHold = grace;
			}
		}
		float exc[NUM_CORES];
		for (int i = 0; i < NUM_CORES; i++)
			exc[i] = burst[i].active ? burst[i].next(pingRng) : 0.f;
		if (pingHold > 0)
			pingHold--;

		// Noise (independent per side, blended to identical as WIDTH goes to 0 so a mono input stays mono)
		float noiseL = 0.f, noiseR = 0.f;
		if (s.noise > 1e-6f) {
			float a, b;
			if (p.noiseType == 1) {
				a = shiftNoise[0].process(s.color);
				b = shiftNoise[1].process(s.color);
			}
			else {
				a = noise[0].process(s.color);
				b = noise[1].process(s.color);
			}
			float amt = s.noise * s.noise;
			noiseL = a * amt;
			noiseR = (a + (b - a) * s.width) * amt;
		}
		// ANALOG's noise floor follows activity (hissGate, per block): there under signal and rings, silent when nothing
		// plays (high RES would otherwise ring it up to an audible hiss with nothing connected)
		float hiss = 0.f;
		if (s.analog > 0.f && hissGate > 0.f)
			hiss = hissRng.uniform() * s.analog * HISS * hissGate;

		// DRIVE voicing / position: a change fades the output out (5 ms), switches while silent, fades back in
		if (p.driveVoice != voiceNow || p.drivePos != posNow) {
			driveFade -= driveFadeStep;
			if (driveFade <= 0.f) {
				driveFade = 0.f;
				voiceNow = p.driveVoice;
				posNow = p.drivePos;
				// Stay silent until the (reset) drive paths have filled, then fade in
				driveHold = 2 * DriveStage::DRIVE_LATENCY + 8;
				for (int c = 0; c < 2; c++) {
					for (int i = 0; i < NUM_CORES; i++)
						drives[c][i].drive.drive.voice = voiceNow;
					postDrives[c].drive.drive.voice = voiceNow;
					postDrives[c].reset();
				}
			}
		}
		else if (driveHold > 0)
			driveHold--;
		else if (driveFade < 1.f)
			driveFade = std::min(1.f, driveFade + driveFadeStep);
		float preAmt = s.drive;
		if (posNow == DRIVE_POST)
			preAmt = 0.f;
		else if (posNow == DRIVE_BOTH)
			preAmt = BOTH_SHARE * s.drive;

		// Inputs per core, through DRIVE
		float x[2][NUM_CORES];
		const float* in[2] = {inL, inR};
		if (p.noiseFollowAmt > 0.f && p.noiseFollow >= 2)
			for (int i = 0; i < NUM_CORES; i++) {
				float a = perCore ? std::max(std::fabs(inL[i]), std::fabs(inR[i])) : std::max(std::fabs(inL[0]), std::fabs(inR[0]));
				followIn[i] = std::max(a, followIn[i] * followDecay);
			}
		float noiseLR[2] = {noiseL, noiseR};
		noiseNow[0] = noiseL;
		noiseNow[1] = noiseR;
		for (int c = 0; c < 2; c++) {
			if (perCore) {
				for (int i = 0; i < NUM_CORES; i++) {
					noiseCore[c][i] = noiseLR[c] * noiseScale[i] * followGain[i];
					x[c][i] = drives[c][i].process(in[c][i], preAmt, grace) + noiseCore[c][i];
				}
			}
			else {
				float v = drives[c][0].process(in[c][0], preAmt, grace);
				for (int i = 0; i < NUM_CORES; i++) {
					noiseCore[c][i] = noiseLR[c] * noiseScale[i] * followGain[i];
					x[c][i] = v + noiseCore[c][i];
				}
			}
		}

		// Interpolate the integrator gains across the block (no zipper on fast moves)
		for (int c = 0; c < 2; c++)
			for (int i = 0; i < NUM_CORES; i++)
				gNow[c][i] += gStep[c][i];

		float wet[2];
		// Nothing to hear (no input, noise, hiss or ping, rings decayed, no self-oscillation): skip the cores. Their
		// state is cleared once on the way in, so they restart from exactly zero when signal returns.
		bool idle = coresQuiet && hiss == 0.f && (!noiseLive || (noiseL == 0.f && noiseR == 0.f));
		for (int c = 0; c < 2 && idle; c++)
			for (int i = 0; i < NUM_CORES && idle; i++)
				idle = x[c][i] == 0.f && exc[i] == 0.f;
		if (idle) {
			if (!coresIdle) {
				coresIdle = true;
				for (int c = 0; c < 2; c++)
					for (int i = 0; i < NUM_CORES; i++) {
						cores[c][i].reset();
						bands[c][i].reset();
						holdVal[c][i] = 0.f;
						lpgLp[c][i] = 0.f;
						slopeStage[c][i].reset();
					}
				for (int i = 0; i < NUM_CORES; i++)
					lpgV[i] = lpgEnv[i] = 0.f;
			}
			wet[0] = wet[1] = 0.f;
			for (int i = 0; i < NUM_CORES; i++)
				out.cores[i] = 0.f;
		}
		else
			runCoresAny(x, exc, hiss, wet, out);


		// DRIVE after the cores (After / Both): on the wet signal, before the level compensation. Adds its latency,
		// so the dry path is delayed by the same amount below.
		if (posNow != DRIVE_PRE) {
			float postAmt = (posNow == DRIVE_BOTH) ? BOTH_SHARE * s.drive : s.drive;
			for (int c = 0; c < 2; c++)
				wet[c] = postDrives[c].process(wet[c], postAmt, grace);
		}

		// Level compensation: the wet signal is brought back to the input's loudness (BAND, narrow BP, notch, a low LP...).
		// Energies are followed per sample (300 ms); the gain is worked out per block (see updateLevelComp) and
		// stepped smoothly across it.
		float ein, eout;
		if (p.levelWeight == 1 && p.levelComp != LEVEL_OFF) {
			// Ear weighting: the same curve on both sides, so brightness and bass are judged as they are heard
			float wl = weightIn[0].process(dryL), wr = weightIn[1].process(dryR);
			float ol = weightOut[0].process(wet[0]), orr = weightOut[1].process(wet[1]);
			ein = 0.5f * (wl * wl + wr * wr);
			eout = 0.5f * (ol * ol + orr * orr);
		}
		else {
			ein = 0.5f * (dryL * dryL + dryR * dryR);
			eout = 0.5f * (wet[0] * wet[0] + wet[1] * wet[1]);
		}
		energyIn += (ein - energyIn) * compCoef;
		energyOut += (eout - energyOut) * compCoef;
		if (++compCount >= BLOCK) {
			compCount = 0;
			updateLevelComp();
		}
		compGain += compStep;
		float dl = dryDelay(0, dryL);
		float dr = dryDelay(1, dryR);
		float l = dl + (wet[0] * compGain - dl) * s.mix;
		float r = dr + (wet[1] * compGain - dr) * s.mix;
		if (driveFade < 1.f) {
			l *= driveFade;
			r *= driveFade;
		}
		// GAIN: ramped per sample (5 ms) so a fast turn does not step; exactly 1 at 0 dB
		outGainNow += (s.outGain - outGainNow) * outGainCoef;
		l *= outGainNow;
		r *= outGainNow;
		limiter.process(l, r, CEILING, &out.l, &out.r);

		for (int i = 0; i < NUM_CORES; i++)
			out.corePos[i] = (oct[i] - LOG2_20) / LOG2_1000;
	}

	/** Output ceiling in engine units (2.0 = 10 V) */
	static constexpr float CEILING = 2.f;
	/** Ring amplitude of a full-velocity ping in one core */
	static constexpr float PING_LEVEL = 0.9f;
	/** Noise floor per core at full ANALOG (about -70 dB) */
	static constexpr float HISS = 3e-4f;
	/** Activity (input RMS + strongest ring, engine units) at which the noise floor is fully on (-34 dBFS of 10 V) */
	static constexpr float HISS_GATE_REF = 0.02f;
	/** Rings below this count as gone (-200 dB) */
	static constexpr float QUIET_LEVEL = 1e-10f;
	/** A core self-oscillates once its feedback loop gain (fbNow, see resFeedback) reaches 1 */
	static constexpr float FB_SELF_OSC = 1.f;

	/** True once the first block has run (place[] is valid) */
	bool placeReady() const {
		return !first;
	}

	/** Level compensation now, in dB (for tests and a future meter) */
	float levelCompDb() const {
		return compDb;
	}

	/** Smoothed settings (exposed for tests) */
	EngineParams s;
	/** Each core's frequency now, log2(Hz) */
	float oct[NUM_CORES] = {};
	/** Where SPREAD puts each core, in CUTOFF knob units (for taking an own cutoff without a jump; UI reads it) */
	float place[NUM_CORES] = {};

private:
	float sampleRate = 44100.f;
	float blockCoef = 0.1f;
	float compCoef = 0.f;
	int grace = 44100;
	int blockPos = 0;
	bool first = true;
	bool digitalOn = false;

	Core cores[2][NUM_CORES];
	DriveStage drives[2][NUM_CORES];
	/** DRIVE after the cores, one per side */
	DriveStage postDrives[2];
	int voiceNow = VOICE_WARM;
	int posNow = DRIVE_PRE;
	/** Output gain while a DRIVE voicing / position change fades out and back in (1 = steady) */
	float driveFade = 1.f;
	float driveFadeStep = 1.f / 240.f;
	int driveHold = 0;
	static constexpr float BOTH_SHARE = 0.7f;
	ColoredNoise noise[2];
	ShiftNoise shiftNoise[2];
	Rng hissRng;
	Rng pingRng;
	SafetyLimiter limiter;
	float outGainNow = 1.f;
	float outGainCoef = 0.004f;

	// Per block, per side and core
	float gNow[2][NUM_CORES] = {};
	float gStep[2][NUM_CORES] = {};
	float kNow[2][NUM_CORES] = {};
	float bpGain[NUM_CORES] = {};
	float fbNow[NUM_CORES] = {};
	float panGain[2][NUM_CORES] = {};
	/** Own pans: mid gains per side and the side's share */
	float panMid[2][NUM_CORES] = {};
	float panSide[NUM_CORES] = {};
	bool anyOwnPan = false;
	// 24 dB slope: second low-pass stage per core and side, its share (TYPE's LP weight, 0 = this core is 12 dB), the bass
	// loss of the ladder trait, and whether any core uses it
	LowStage slopeStage[2][NUM_CORES];
	float slopeShare[NUM_CORES] = {};
	float slopeGain[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	bool anySlope24 = false;
	static constexpr float SLOPE_K2 = 1.41421356f;
	// Low-pass gate (vactrol model, one per core): light state, input envelope, ping pulse, filter state, and the
	// gain / one-pole coefficient interpolated across each block
	bool lpgOn = false;
	float lpgV[NUM_CORES] = {};
	float lpgEnv[NUM_CORES] = {};
	int lpgPulse[NUM_CORES] = {};
	float lpgVel[NUM_CORES] = {};
	float lpgLp[2][NUM_CORES] = {};
	float lpgGainNow[NUM_CORES] = {}, lpgGainStep[NUM_CORES] = {};
	float lpgCoefNow[NUM_CORES] = {}, lpgCoefStep[NUM_CORES] = {};
	float lpgEnvDecay = 0.999f;
	bool lpgFirst = true;
	/** Noise-floor gate 0..1 (per block), its smoothing, idle state */
	float hissGate = 1.f;
	float hissGateCoef = 0.1f;
	bool selfOsc = false;
	bool coresQuiet = false;
	bool coresIdle = false;
	/** This sample's noise per side (already in every core's input; series cores get it again on top of the chain) */
	float noiseNow[2] = {};
	float coreGain[NUM_CORES] = {};
	float weights[NUM_CORES][4] = {{1.f, 0.f, 0.f, 0.f}, {1.f, 0.f, 0.f, 0.f}, {1.f, 0.f, 0.f, 0.f}, {1.f, 0.f, 0.f, 0.f}};
	float stackNow = 0.f;
	float limit = 0.f;
	/** DRIVE before the cores on each core's ring: on, gain, make-up (see updateBlock) */
	bool ringDriveOn = false;
	float ringG = 1.f;
	float ringMake = 1.f;
	float ringMix = 0.f;
	/** Level-dependent resonance amount (RES_SAT x ANALOG; 0 = off) */
	float resSat = 0.f;
	float pingResp[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	/** Ping scale per core for the gain the cores after it add at its pitch in series (1 = parallel) */
	float seriesNorm[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	/** The same for the noise that enters each core, per side (noiseCore) */
	float noiseScale[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	float slopeNoiseComp[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	// NOISE follows (ping ring / input level): per core, the input level follower (30 ms), the ring level (per block), and
	// the smoothed gain on the core's noise
	float followIn[NUM_CORES] = {};
	float pingEnv[NUM_CORES] = {};
	float followGain[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	float followDecay = 0.999f;
	bool noiseLive = true;
	float noiseCore[2][NUM_CORES] = {};

	// Character
	uint32_t characterSeed = 0, randomSeed = 0;
	float mismatchOct[NUM_CORES] = {};
	float mismatchQ[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	float mismatchGain[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	// Slow thermal drift (ANALOG): per core a random walk of pitch (+-4 cents) and of resonance (+-8%), each target
	// reached over 6-20 s on a smooth curve
	Rng thermalRng[NUM_CORES];
	float thermalFrom[NUM_CORES][2] = {}, thermalTo[NUM_CORES][2] = {}, thermalPhase[NUM_CORES][2] = {};
	float thermalNow[NUM_CORES][2] = {};
	float randomPlaces[NUM_CORES] = {};
	float randomTarget[NUM_CORES] = {};
	float randomGlide = 0.3f;
	float bloom[NUM_CORES] = {};

	// PING
	PingBurst burst[NUM_CORES];
	int pending[NUM_CORES] = {-1, -1, -1, -1};
	float pendingVel[NUM_CORES] = {};
	int cycleIndex = 0;
	int pingHold = 0;

	// DIGITAL sample-and-hold
	float holdVal[2][NUM_CORES] = {};
	int holdCount[2][NUM_CORES] = {};
	int holdLen = 1;
	float stateStep = 0.f;
	/** PCM gate threshold (engine units) at the top of DIGITAL */
	float digitalGate = 0.f;

	// BAND
	BandStage bands[2][NUM_CORES];
	bool bandOn = false;
	float bandK[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	float bandGain[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	float bandMix[NUM_CORES] = {};
	/** Make-up gain of the band on the core's output contribution (1 = none) */
	float bandOut[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};

	// Output
	float compGain = 1.f;
	float compStep = 0.f;
	float compDb = 0.f;
	int compCount = 0;
	/** Blocks without input (or right after a ping) */
	int silentBlocks = 0;
	float compSmooth = 0.1f, compRelax = 0.02f;
	float energyIn = 0.f, energyOut = 0.f;
	LoudnessWeight weightIn[2], weightOut[2];

	/** Below this input energy (about 16 mV RMS) there is nothing to match: hold the gain */
	static constexpr float COMP_GATE = 1e-5f;

	/** Level compensation, once per block. Learns only while there is input and no ping ring (the ring is not in the
	input); holds through gaps, and after 2 s of silence drifts back to 0 dB, so noise or self-oscillation alone is
	never left boosted. */
	void updateLevelComp() {
		float up = 0.f, down = 0.f;
		if (p.levelComp == LEVEL_FULL) {
			up = 24.f;
			down = -12.f;
		}
		else if (p.levelComp == LEVEL_GENTLE) {
			up = 6.f;
			down = -6.f;
		}
		float target = compDb;
		float coef = compSmooth;
		if (p.levelComp == LEVEL_OFF || s.lpg > 0.f) {
			// (with the low-pass gate on, the closed gate is the sound: do not learn a boost for it)
			target = 0.f;
		}
		else if (energyIn > COMP_GATE && pingHold == 0) {
			silentBlocks = 0;
			target = 10.f * std::log10((energyIn + 1e-12f) / (energyOut + 1e-12f));
			target = std::min(std::max(target, down), up);
		}
		else if (++silentBlocks * BLOCK > 2 * grace) {
			// grace = one second of samples
			target = 0.f;
			coef = compRelax;
		}
		compDb += (target - compDb) * coef;
		float g = std::pow(10.f, compDb * 0.05f);
		compStep = (g - compGain) / BLOCK;
	}
	float dryBuf[2][128] = {};
	int dryPos[2] = {};

	/** The dry signal, delayed like the wet path (DRIVE latency), so MIX never combs. */
	float dryDelay(int c, float x) {
		dryBuf[c][dryPos[c]] = x;
		// One DRIVE latency (before the cores), or two with DRIVE after the cores too
		int lat = (posNow == DRIVE_PRE) ? DriveStage::DRIVE_LATENCY : 2 * DriveStage::DRIVE_LATENCY;
		float y = dryBuf[c][(dryPos[c] - lat) & 127];
		dryPos[c] = (dryPos[c] + 1) & 127;
		return y;
	}

	void smooth(float& v, float target, float coef) {
		v += (target - v) * coef;
	}

	void updateBlock() {
		float c = first ? 1.f : blockCoef;
		// Knobs, smoothed. Switches and menu values go straight through.
		smooth(s.cutoff, p.cutoff, c);
		smooth(s.spread, p.spread, c);
		smooth(s.res, p.res, c);
		smooth(s.type, p.type, c);
		smooth(s.stack, p.stack, c);
		smooth(s.drive, p.drive, c);
		smooth(s.width, p.width, c);
		smooth(s.mix, p.mix, c);
		smooth(s.outGain, p.outGain, c);
		smooth(s.noise, p.noise, c);
		smooth(s.color, p.color, c);
		smooth(s.analog, p.analog, c);
		smooth(s.digital, p.digital, c);
		smooth(s.band, p.band, c);
		smooth(s.lpg, p.lpg, c);
		if (s.lpg < 1e-5f && p.lpg == 0.f)
			s.lpg = 0.f;
		for (int i = 0; i < NUM_CORES; i++) {
			smooth(s.coreSpread[i], p.coreSpread[i], c);
			// Each core's TYPE / RES / BAND: its own value, or the global knob it follows
			smooth(s.coreType[i], p.ownType[i] ? p.coreType[i] : p.type, c);
			smooth(s.coreRes[i], p.ownRes[i] ? p.coreRes[i] : p.res, c);
			float bandTarget = p.ownBand[i] ? p.coreBand[i] : p.band;
			smooth(s.coreBand[i], bandTarget, c);
			if (s.coreBand[i] < 1e-5f && bandTarget == 0.f)
				s.coreBand[i] = 0.f;
			smooth(s.coreLevel[i], p.coreLevel[i], c);
			// Pan: the owned value, or (so taking ownership starts where the core is) WIDTH's placement
			smooth(s.corePan[i], p.ownPan[i] ? p.corePan[i] : s.width * widthPlace(i), c);
		}
		if (s.band < 1e-5f && p.band == 0.f)
			s.band = 0.f;
		s.strum = p.strum;
		s.strike = p.strike;
		s.shape = p.shape;
		// Exactly 0 once close (so "off" really skips work)
		if (s.noise < 1e-5f && p.noise == 0.f)
			s.noise = 0.f;
		if (s.digital < 1e-5f && p.digital == 0.f)
			s.digital = 0.f;
		if (s.drive < 1e-5f && p.drive == 0.f)
			s.drive = 0.f;
		if (s.analog < 1e-5f && p.analog == 0.f)
			s.analog = 0.f;


		// Where the cores are: CUTOFF, SPREAD shape, character
		float base = LOG2_20 + s.cutoff * LOG2_1000;
		if (p.quantizeCutoff) {
			float semis = (base - std::log2(261.6256f)) * 12.f;
			base = std::log2(261.6256f) + quantizeToScale(p.scale, semis) / 12.f;
		}
		float spreadEff = clamp01(s.spread + p.spreadCv);
		float offs[NUM_CORES];
		for (int i = 0; i < NUM_CORES; i++)
			randomPlaces[i] += (randomTarget[i] - randomPlaces[i]) * randomGlide;
		spreadOffsets(s.shape, spreadEff, p.scale, p.chord, randomPlaces, offs, p.justTuning);
		// Each core's own spread (the dots on the ring)
		for (int i = 0; i < NUM_CORES; i++)
			offs[i] *= s.coreSpread[i];

		// TYPE (per core), STACK
		for (int i = 0; i < NUM_CORES; i++)
			typeWeights(s.coreType[i], weights[i]);
		stackNow = clamp01(s.stack + p.stackCv);

		// Slow thermal drift: advance each walk (target every 6-20 s, smoothstep between targets)
		if (s.analog > 0.f) {
			static const float RATE[NUM_CORES][2] = {{1.f / 11.f, 1.f / 17.f}, {1.f / 15.f, 1.f / 9.f},
				{1.f / 8.f, 1.f / 19.f}, {1.f / 13.f, 1.f / 7.f}};
			for (int i = 0; i < NUM_CORES; i++)
				for (int k = 0; k < 2; k++) {
					thermalPhase[i][k] += RATE[i][k] * (float) BLOCK / sampleRate;
					if (thermalPhase[i][k] >= 1.f) {
						thermalPhase[i][k] -= 1.f;
						thermalFrom[i][k] = thermalTo[i][k];
						thermalTo[i][k] = thermalRng[i].uniform();
					}
					float t = thermalPhase[i][k];
					t = t * t * (3.f - 2.f * t);
					thermalNow[i][k] = thermalFrom[i][k] + (thermalTo[i][k] - thermalFrom[i][k]) * t;
				}
		}
		// ANALOG: integrators saturate earlier, mismatch and pitch bloom grow
		float an = s.analog;
		limit = 8.f - 6.2f * an;
		// DRIVE before the cores also drives each core's own ring (the strike and the resonance bypass the input drive):
		// a saturating waveshaper on the core's output, tanh(g y) / sqrt(g): odd harmonics, a compressed, sustained,
		// fuzzy tail (quiet rings gain up to sqrt(g), loud ones are held). Faded in with the amount (a straight tanh at
		// DRIVE 1% already bent a 5 V sine to H3 -23 dB), off at DRIVE 0 and after the cores.
		float resDrive = posNow == DRIVE_PRE ? s.drive : posNow == DRIVE_BOTH ? BOTH_SHARE * s.drive : 0.f;
		ringDriveOn = resDrive > 0.f;
		ringG = std::exp(resDrive * 2.48490665f); // 1 .. 12
		ringMake = 1.f / std::sqrt(ringG);
		ringMix = resDrive;
		resSat = RES_SAT * an;
		static const float PAN[NUM_CORES] = {-1.f, 1.f / 3.f, -1.f / 3.f, 1.f};
		static const float WIDTH_SIGN[NUM_CORES] = {-1.f, 1.f, 1.f, -1.f};
		anyOwnPan = false;
		float maxRing = 0.f;
		for (int i = 0; i < NUM_CORES; i++) {
			// Pitch bloom: a loud ring sags the core's pitch a little; it comes back as the ring decays.
			float level = std::min(cores[0][i].ringLevel(), 2.f);
			maxRing = std::max(maxRing, std::max(level, cores[1][i].ringLevel()));
			smooth(bloom[i], level, 0.25f);
			// Where SPREAD puts the core, in CUTOFF knob units (an own cutoff starts here, so taking it never jumps)
			place[i] = s.cutoff + offs[i] / LOG2_1000;
			smooth(s.coreCutoff[i], p.ownCutoff[i] ? p.coreCutoff[i] : place[i], c);
			float o;
			if (p.ownCutoff[i]) {
				float own = LOG2_20 + s.coreCutoff[i] * LOG2_1000;
				if (p.quantizeCutoff) {
					float semis = (own - std::log2(261.6256f)) * 12.f;
					own = std::log2(261.6256f) + quantizeToScale(p.scale, semis) / 12.f;
				}
				o = own + p.pitchCv[i] + an * (mismatchOct[i] + thermalNow[i][0] * (4.f / 1200.f)) - an * 0.025f * bloom[i];
			}
			else {
				// The core's place: CUTOFF + its SPREAD offset. With "Quantize to the scale" the whole place snaps to
				// the scale (so SPREAD puts every core on a scale note), except SHAPE Chord / Scale, which already
				// build their notes from the root (snapping a chord in another key to C major would break it).
				float po = base + offs[i];
				if (p.quantizeCutoff && s.shape != SHAPE_CHORD && s.shape != SHAPE_SCALE) {
					float semis = (po - std::log2(261.6256f)) * 12.f;
					po = std::log2(261.6256f) + quantizeToScale(p.scale, semis) / 12.f;
				}
				o = po + p.pitchCv[i] + an * (mismatchOct[i] + thermalNow[i][0] * (4.f / 1200.f)) - an * 0.025f * bloom[i];
			}
			oct[i] = o;
			float res = clamp01(s.coreRes[i] + p.resCv[i]);
			float qScale = 1.f + an * (mismatchQ[i] - 1.f + 0.08f * thermalNow[i][1]);
			coreGain[i] = (1.f + an * (mismatchGain[i] - 1.f)) * s.coreLevel[i];
			if (p.ownPan[i]) {
				// Own pan: the whole core (both channels) moves; mid panned (same law as WIDTH), side narrowed
				float pan = std::min(std::max(s.corePan[i], -1.f), 1.f);
				panGain[0][i] = panGain[1][i] = 0.f;
				panMid[0][i] = std::sqrt(1.f - pan);
				panMid[1][i] = std::sqrt(1.f + pan);
				panSide[i] = 1.f - std::fabs(pan);
				anyOwnPan = true;
			}
			else {
				float pan = s.width * PAN[i];
				panGain[0][i] = std::sqrt(1.f - pan);
				panGain[1][i] = std::sqrt(1.f + pan);
				panMid[0][i] = panMid[1][i] = panSide[i] = 0.f;
			}
			for (int c = 0; c < 2; c++) {
				// WIDTH also pulls each core's two sides a little apart in pitch
				float side = (c == 0 ? -1.f : 1.f) * WIDTH_SIGN[i] * s.width * 0.03f;
				float fc = std::exp2(o + side);
				CoreCoefs cc = coreCoefs(fc, res, qScale, sampleRate);
				float g = cc.g;
				if (s.digital > 0.f)
					g = quantizeCoef(g, s.digital);
				if (first)
					gNow[c][i] = g;
				gStep[c][i] = (g - gNow[c][i]) / BLOCK;
				kNow[c][i] = (s.digital > 0.f) ? quantizeCoef(cc.k, s.digital) : cc.k;
				if (c == 0) {
					bpGain[i] = cc.bpGain;
					fbNow[i] = cc.fb;
					pingResp[i] = pingResponse(weights[i], cc.k, cc.bpGain);
				}
			}
		}
		// Low-pass gate: the vactrol of each core is lit by the ping pulse and / or the core's input level; it rises in
		// ~2 ms and falls slowly, slower the darker it gets (tau 0.3x .. 2.5x the DECAY time). Its light sets the open
		// amount o (depth s.lpg): gain o^2, one-pole cutoff 40 Hz .. 20 kHz (log).
		bool wasLpg = lpgOn;
		lpgOn = s.lpg > 0.f;
		if (lpgOn) {
			if (!wasLpg)
				lpgFirst = true;
			float tauRel = 0.04f * std::exp(3.624341f * p.lpgDecay); // 40 ms .. 1.5 s (ln 37.5)
			float attack = 1.f - std::exp(-(float) BLOCK / (0.002f * sampleRate));
			bool useEnv = p.lpgSource != 0, usePing = p.lpgSource != 1;
			for (int i = 0; i < NUM_CORES; i++) {
				float t = 0.f;
				if (usePing && lpgPulse[i] > 0)
					t = lpgVel[i];
				if (useEnv)
					t = std::max(t, std::min(1.f, std::sqrt(lpgEnv[i] * 10.f)));
				lpgPulse[i] = std::max(0, lpgPulse[i] - BLOCK);
				float v = lpgV[i];
				if (t > v)
					v += (t - v) * attack;
				else {
					float tau = tauRel * (0.3f + 2.2f * (1.f - v));
					v += (t - v) * (1.f - std::exp(-(float) BLOCK / (tau * sampleRate)));
				}
				lpgV[i] = v;
				float o = 1.f - s.lpg * (1.f - v);
				float gainT = o * o;
				float fc = std::min(40.f * std::pow(500.f, o), 0.45f * sampleRate);
				float coefT = 1.f - std::exp(-6.2831853f * fc / sampleRate);
				if (lpgFirst) {
					lpgGainNow[i] = gainT;
					lpgCoefNow[i] = coefT;
				}
				lpgGainStep[i] = (gainT - lpgGainNow[i]) / BLOCK;
				lpgCoefStep[i] = (coefT - lpgCoefNow[i]) / BLOCK;
			}
			lpgFirst = false;
		}
		else if (wasLpg) {
			for (int i = 0; i < NUM_CORES; i++) {
				lpgV[i] = lpgEnv[i] = 0.f;
				lpgLp[0][i] = lpgLp[1][i] = 0.f;
			}
		}
		// NOISE follows (amount f): the core's noise gain is 1 - f + f env, env = its ping envelope and / or the input
		// level, smoothed over ~1 ms. f = 0 is plain noise; f = 1 is noise only while a ping / the input sounds.
		// The ping envelope is set by the ping (velocity) and released between half the core's ring rate (low amounts:
		// a tail that blooms in the resonance) and 40 ms (full amount: tight, short). It is not read from the core's
		// state, which the noise itself excites (following the state latched the noise on at 10 V).
		float followMax = 0.f;
		// Reversed exponential: the slider's lower part barely changes the noise, the top gates it (x^3 of the travel)
		float fAmt = clamp01(p.noiseFollowAmt);
		fAmt = fAmt * fAmt * fAmt;
		for (int i = 0; i < NUM_CORES; i++) {
			float target = 1.f;
			if (fAmt > 0.f) {
				float gp = 0.f;
				if (p.noiseFollow & 1) {
					float ringRate = 0.5f * 3.14159265f * std::exp2(oct[i]) * std::max(kNow[0][i], 0.002f);
					float rate = (1.f - fAmt) * ringRate + fAmt * 25.f; // 1 / s (25 = 40 ms)
					pingEnv[i] *= std::exp(-rate * BLOCK / sampleRate);
					if (pingEnv[i] < 1e-4f)
						pingEnv[i] = 0.f;
					gp = pingEnv[i];
				}
				// Input level in dB (followIn is the peak at 0.2 x volts): -40 dBV = closed, -10 dBV = open, so the noise
				// follows the input's dynamics (a linear map was full open above ~0.6 V and never followed a signal)
				float gi = 0.f;
				if (p.noiseFollow & 2) {
					float db = 20.f * std::log10(followIn[i] * 5.f + 1e-6f);
					gi = clamp01((db + 40.f) / 30.f);
				}
				target = 1.f - fAmt + fAmt * std::max(gp, gi);
			}
			followGain[i] += (target - followGain[i]) * 0.3f;
			if (target == 0.f && followGain[i] < 1e-4f)
				followGain[i] = 0.f;
			followMax = std::max(followMax, followGain[i]);
		}
		noiseLive = followMax > 0.f;
		// 24 dB slope: its share is the core's LP weight; the ladder's bass loss grows with RES under ANALOG
		anySlope24 = false;
		for (int i = 0; i < NUM_CORES; i++) {
			bool on = p.slope24[i];
			anySlope24 = anySlope24 || on;
			float resN = std::min(clamp01(s.coreRes[i] + p.resCv[i]) / RES_FB_START, 1.f);
			// (about -3.5 dB at the most: "a little bass", measured against -12 dB for noise with the first, stronger setting)
			float loss = 1.f / (1.f + 0.5f * an * resN * resN);
			slopeShare[i] = on ? weights[i][0] : 0.f;
			slopeGain[i] = 1.f - slopeShare[i] * (1.f - loss);
			// Noise and the like pass the second stage and the bass loss too: give back what they take (a ping is
			// compensated through pingResp, input through the level compensation)
			slopeNoiseComp[i] = on ? std::min(3.f, 1.f / (slopeGain[i] * (1.f - slopeShare[i] + slopeShare[i] * 0.88f))) : 1.f;
			// ANALOG's level-dependent damping squashes loud rings and noise at high RES (measured -1..-2 dB): give it back
			slopeNoiseComp[i] *= 1.f + 0.3f * an * resN * resN;
			pingResp[i] /= 1.f + 0.2f * an * resN * resN;
			// A ring clipped by the integrator knee cannot be boosted at the strike: give the 24 dB + ANALOG loss back at the
			// core's output (ring, noise and input alike)
			coreGain[i] *= 1.f + 1.0f * slopeShare[i] * an * resN * resN;
			if (on) // a ping rings through the second stage (0.707 at the ring) and the bass loss
				pingResp[i] *= (1.f - slopeShare[i] + slopeShare[i] * 0.7071f) * slopeGain[i];
			else
				slopeStage[0][i].reset(), slopeStage[1][i].reset();
		}
		// In series a core's ring passes the cores after it: where they resonate at its pitch they multiply it (four
		// cores on one pitch: Q^4). Scale each ping by what the downstream cores add at its pitch (only a reduction,
		// and only as far as STACK goes), so a ping rings about as loud in series as in parallel.
		for (int i = 0; i < NUM_CORES; i++) {
			float down = 1.f;
			if (stackNow > 0.f)
				for (int j = i + 1; j < NUM_CORES; j++)
					down *= coreMagnitude(std::exp2(oct[i] - oct[j]), kNow[0][j], weights[j], bpGain[j]);
			seriesNorm[i] = 1.f / (1.f + stackNow * (std::max(down, 1.f) - 1.f));
			// Broadband noise gains a bit less than a ring's peak does (it only fills the resonance's bandwidth)
			noiseScale[i] = std::pow(seriesNorm[i], 1.0f) * slopeNoiseComp[i];
		}
		// BAND: the steep band after each core (fades in over the first 10% of the knob; asleep and cleared at 0)
		bool wasBand = bandOn;
		bandOn = false;
		for (int i = 0; i < NUM_CORES; i++)
			bandOn = bandOn || s.coreBand[i] > 0.f;
		if (bandOn) {
			for (int i = 0; i < NUM_CORES; i++) {
				// A core at BAND 0 runs the stage with a mix of 0 (its output stays exactly the core's)
				float oct = bandOctaves(s.coreBand[i]);
				bandK[i] = bandStageK(oct);
				bandGain[i] = bandMakeup(oct);
				bandMix[i] = std::min(1.f, s.coreBand[i] * 10.f);
				bandOut[i] = 1.f + (bandGain[i] - 1.f) * bandMix[i];
				// A ring sits at the band's centre and would get all of the make-up gain: pings stay as loud as
				// without BAND.
				pingResp[i] *= 1.f + (bandGain[i] - 1.f) * bandMix[i];
			}
		}
		else if (wasBand) {
			for (int ch = 0; ch < 2; ch++)
				for (int i = 0; i < NUM_CORES; i++)
					bands[ch][i].reset();
		}
		// Activity (input level + strongest ring) -> noise floor gate; self-oscillation keeps it open (it starts the
		// oscillation, as in a real filter). Idle check for skipping the cores.
		selfOsc = false;
		for (int i = 0; i < NUM_CORES; i++)
			selfOsc = selfOsc || fbNow[i] * FB_SELF_OSC >= 1.f;
		float activity = std::sqrt(energyIn) + maxRing;
		float gateTarget = selfOsc ? 1.f : std::min(1.f, activity / HISS_GATE_REF);
		hissGate += (gateTarget - hissGate) * hissGateCoef;
		if (!selfOsc && hissGate < 1e-4f && gateTarget == 0.f)
			hissGate = 0.f;
		coresQuiet = !selfOsc && maxRing < QUIET_LEVEL;
		// DIGITAL: coarse coefficients (above), sample-and-hold rate reduction, then coarse states at the top
		digitalOn = s.digital > 0.f;
		if (digitalOn) {
			float d = s.digital;
			holdLen = 1 + (int) std::lround(d * d * 31.f);
			stateStep = d > 0.7f ? std::exp2(-(16.f - 26.f * (d - 0.7f))) : 0.f;
			digitalGate = d > 0.75f ? (d - 0.75f) * 4.f * 0.06f : 0.f;
		}
		first = false;
	}

	/** Fixed-point coefficient: 14 bits at DIGITAL 0+, down to 6 bits at full (never 0). */
	static float quantizeCoef(float v, float d) {
		float bits = 14.f - 8.f * d;
		float q = std::exp2(bits);
		return std::max(std::round(v * q), 1.f) / q;
	}

	/** The four cores on both sides, stacked (parallel ... series), panned. BAND narrows each core's output to a steep
	band at its pitch (only instantiated with BAND on, so BAND 0 runs exactly the code it ran before). */
	template <bool DIGITAL, bool BAND, bool SAT, bool LPG, bool S24>
	inline void coresImpl(float x[2][NUM_CORES], const float exc[NUM_CORES], float hiss, float wet[2], EngineOutput& out) {
		float m = stackNow;
		float ycore[2][NUM_CORES];
		if (LPG) {
			// Each core's input level (for the gate's input source) and the gate's gain / cutoff, interpolated
			for (int i = 0; i < NUM_CORES; i++) {
				float a = 0.5f * (std::fabs(x[0][i]) + std::fabs(x[1][i]));
				lpgEnv[i] = std::max(a, lpgEnv[i] * lpgEnvDecay);
				lpgGainNow[i] += lpgGainStep[i];
				lpgCoefNow[i] += lpgCoefStep[i];
			}
		}
		for (int c = 0; c < 2; c++) {
			float prev = x[c][0];
			float sum = 0.f;
			float last = 0.f;
			for (int i = 0; i < NUM_CORES; i++) {
				Core& core = cores[c][i];
				if (exc[i] != 0.f)
					core.excite(exc[i] * seriesNorm[i], weights[i][1] + weights[i][3]);
				// In series a core hears the previous core: give it the noise too, so NOISE reaches every core
				float u = x[c][i] + (prev + noiseCore[c][i] - x[c][i]) * m + hiss;
				float y = core.process(u, gNow[c][i], SAT ? core.saturatedK(kNow[c][i], resSat) : kNow[c][i], weights[i],
					bpGain[i], fbNow[i], limit);
				if (SAT) {
					// DRIVE before the cores on the ring (ringDriveOn): see updateBlock
					if (ringDriveOn)
						y += ringMix * (fastTanh(ringG * y) * ringMake - y);
				}
				if (S24) {
					// 24 dB slope (ladder-like): the low-pass share of the core's sound goes through a second 12 dB
					// stage at the same cutoff (Q 0.7), and under ANALOG high resonance thins the bass (slopeGain)
					if (slopeShare[i] > 0.f) {
						float lp2 = slopeStage[c][i].process(y, gNow[c][i], SLOPE_K2);
						y = (y + (lp2 - y) * slopeShare[i]) * slopeGain[i];
					}
				}
				if (BAND) {
					// The band itself; its make-up gain goes to the output contribution (below), not into the
					// chain: in series it would multiply through all four stages (+47 dB at BAND 100%)
					float yb = bands[c][i].process(y, gNow[c][i], bandK[i]);
					y += (yb - y) * bandMix[i];
				}
				if (DIGITAL) {
					if (--holdCount[c][i] <= 0) {
						holdCount[c][i] = holdLen;
						holdVal[c][i] = y;
					}
					y = holdVal[c][i];
					// The top of the knob: a per-sample gate (no envelope): every sample below the threshold is
					// zeroed, a gritty, broken-digital texture
					if (std::fabs(y) < digitalGate)
						y = 0.f;
					if (stateStep > 0.f) {
						core.ic1 = std::round(core.ic1 / stateStep) * stateStep;
						core.ic2 = std::round(core.ic2 / stateStep) * stateStep;
					}
				}
				if (LPG) {
					// The vactrol's low-pass gate: a one-pole low-pass whose cutoff and the gain close together
					lpgLp[c][i] += lpgCoefNow[i] * (y - lpgLp[c][i]);
					y = lpgLp[c][i] * lpgGainNow[i];
				}
				if (!std::isfinite(y)) {
					core.reset();
					y = 0.f;
				}
				// LEVEL (and the analog gain mismatch) shapes what the core adds to the output; the chain in series gets
				// the core's own output, so four raised levels do not multiply through the cascade (+12 dB with
				// four cores at 120-160%)
				float yl = y * coreGain[i];
				if (BAND)
					yl *= bandOut[i];
				ycore[c][i] = yl;
				prev = y;
				last = yl;
				sum += panGain[c][i] * yl;
			}
			wet[c] = (1.f - m) * 0.5f * sum + m * last;
		}
		// Cores with an own pan (their pan gains above are 0): mid / side, so a stereo core moves as a whole
		if (anyOwnPan) {
			for (int i = 0; i < NUM_CORES; i++) {
				if (panMid[0][i] == 0.f && panMid[1][i] == 0.f && panSide[i] == 0.f)
					continue;
				float mid = 0.5f * (ycore[0][i] + ycore[1][i]);
				float side = 0.5f * (ycore[0][i] - ycore[1][i]) * panSide[i];
				wet[0] += (1.f - m) * 0.5f * (panMid[0][i] * mid + side);
				wet[1] += (1.f - m) * 0.5f * (panMid[1][i] * mid - side);
			}
		}
		for (int i = 0; i < NUM_CORES; i++)
			out.cores[i] = 0.5f * (ycore[0][i] + ycore[1][i]);
	}

	/** The cores, in the instantiation that matches the settings. Each optional stage (DIGITAL, BAND, ANALOG resonance
	saturation, low-pass gate, 24 dB slope) is a compile-time flag, so an option that is off costs nothing and leaves
	the code of the plain loop exactly as it was (cpu.md: an optional block inside the plain loop regroups its maths). */
	NP_NOINLINE void runCoresAny(float x[2][NUM_CORES], const float exc[NUM_CORES], float hiss, float wet[2],
			EngineOutput& out) {
		coresIdle = false;
		int flags = (digitalOn ? 1 : 0) | (bandOn ? 2 : 0) | (resSat > 0.f || ringDriveOn ? 4 : 0) | (lpgOn ? 8 : 0) | (anySlope24 ? 16 : 0);
		switch (flags) {
			case 0: coresImpl<false, false, false, false, false>(x, exc, hiss, wet, out); break;
			case 1: coresImpl<true, false, false, false, false>(x, exc, hiss, wet, out); break;
			case 2: coresImpl<false, true, false, false, false>(x, exc, hiss, wet, out); break;
			case 3: coresImpl<true, true, false, false, false>(x, exc, hiss, wet, out); break;
			case 4: coresImpl<false, false, true, false, false>(x, exc, hiss, wet, out); break;
			case 5: coresImpl<true, false, true, false, false>(x, exc, hiss, wet, out); break;
			case 6: coresImpl<false, true, true, false, false>(x, exc, hiss, wet, out); break;
			case 7: coresImpl<true, true, true, false, false>(x, exc, hiss, wet, out); break;
			case 8: coresImpl<false, false, false, true, false>(x, exc, hiss, wet, out); break;
			case 9: coresImpl<true, false, false, true, false>(x, exc, hiss, wet, out); break;
			case 10: coresImpl<false, true, false, true, false>(x, exc, hiss, wet, out); break;
			case 11: coresImpl<true, true, false, true, false>(x, exc, hiss, wet, out); break;
			case 12: coresImpl<false, false, true, true, false>(x, exc, hiss, wet, out); break;
			case 13: coresImpl<true, false, true, true, false>(x, exc, hiss, wet, out); break;
			case 14: coresImpl<false, true, true, true, false>(x, exc, hiss, wet, out); break;
			case 15: coresImpl<true, true, true, true, false>(x, exc, hiss, wet, out); break;
			case 16: coresImpl<false, false, false, false, true>(x, exc, hiss, wet, out); break;
			case 17: coresImpl<true, false, false, false, true>(x, exc, hiss, wet, out); break;
			case 18: coresImpl<false, true, false, false, true>(x, exc, hiss, wet, out); break;
			case 19: coresImpl<true, true, false, false, true>(x, exc, hiss, wet, out); break;
			case 20: coresImpl<false, false, true, false, true>(x, exc, hiss, wet, out); break;
			case 21: coresImpl<true, false, true, false, true>(x, exc, hiss, wet, out); break;
			case 22: coresImpl<false, true, true, false, true>(x, exc, hiss, wet, out); break;
			case 23: coresImpl<true, true, true, false, true>(x, exc, hiss, wet, out); break;
			case 24: coresImpl<false, false, false, true, true>(x, exc, hiss, wet, out); break;
			case 25: coresImpl<true, false, false, true, true>(x, exc, hiss, wet, out); break;
			case 26: coresImpl<false, true, false, true, true>(x, exc, hiss, wet, out); break;
			case 27: coresImpl<true, true, false, true, true>(x, exc, hiss, wet, out); break;
			case 28: coresImpl<false, false, true, true, true>(x, exc, hiss, wet, out); break;
			case 29: coresImpl<true, false, true, true, true>(x, exc, hiss, wet, out); break;
			case 30: coresImpl<false, true, true, true, true>(x, exc, hiss, wet, out); break;
			case 31: coresImpl<true, true, true, true, true>(x, exc, hiss, wet, out); break;
			default: break;
		}
	}
};

} // namespace np
