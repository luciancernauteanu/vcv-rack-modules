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

	/** Felt corner in Hz for STRIKE 0..1 */
	static float feltHz(float strike) {
		float m = std::min(clamp01(strike), 0.5f) * 2.f;
		return 20000.f * std::pow(0.015f, m);
	}

	void start(float amplitude, float strike, float sampleRate) {
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
	}

	float next(Rng& rng) {
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
		}
		hissRng.seed(seed ^ 0xa5a5a5a5u);
	}
	uint32_t getCharacterSeed() const {
		return characterSeed;
	}

	/** The frozen places of SHAPE Random (saved in the patch). */
	void seedRandomShape(uint32_t seed) {
		randomSeed = seed;
		Rng r;
		r.seed(seed);
		for (int i = 0; i < NUM_CORES; i++)
			randomPlaces[i] = r.uniform();
	}
	uint32_t getRandomSeed() const {
		return randomSeed;
	}

	void reset() {
		for (int c = 0; c < 2; c++)
			for (int i = 0; i < NUM_CORES; i++) {
				cores[c][i].reset();
				drives[c][i].reset();
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
				burst[i].start(pendingVel[i] * PING_LEVEL / pingResp[i], s.strike, sampleRate);
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
			float a = noise[0].process(s.color);
			float b = noise[1].process(s.color);
			float amt = s.noise * s.noise;
			noiseL = a * amt;
			noiseR = (a + (b - a) * s.width) * amt;
		}
		float hiss = 0.f;
		if (s.analog > 0.f)
			hiss = hissRng.uniform() * s.analog * HISS;

		// Inputs per core, through DRIVE
		float x[2][NUM_CORES];
		const float* in[2] = {inL, inR};
		float noiseLR[2] = {noiseL, noiseR};
		noiseNow[0] = noiseL;
		noiseNow[1] = noiseR;
		for (int c = 0; c < 2; c++) {
			if (perCore) {
				for (int i = 0; i < NUM_CORES; i++)
					x[c][i] = drives[c][i].process(in[c][i], s.drive, grace) + noiseLR[c];
			}
			else {
				float v = drives[c][0].process(in[c][0], s.drive, grace) + noiseLR[c];
				for (int i = 0; i < NUM_CORES; i++)
					x[c][i] = v;
			}
		}

		// Interpolate the integrator gains across the block (no zipper on fast moves)
		for (int c = 0; c < 2; c++)
			for (int i = 0; i < NUM_CORES; i++)
				gNow[c][i] += gStep[c][i];

		float wet[2];
		if (bandOn) {
			if (digitalOn)
				runCoresDigitalBand(x, exc, hiss, wet, out);
			else
				runCoresBand(x, exc, hiss, wet, out);
		}
		else if (digitalOn)
			runCoresDigital(x, exc, hiss, wet, out);
		else
			runCores(x, exc, hiss, wet, out);

		// Level compensation: the wet signal is brought back to the input's loudness (BAND, narrow BP, notch, a low LP...).
		// Energies are followed per sample (300 ms); the gain is worked out per block (see updateLevelComp) and
		// stepped smoothly across it.
		float ein = 0.5f * (dryL * dryL + dryR * dryR);
		float eout = 0.5f * (wet[0] * wet[0] + wet[1] * wet[1]);
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
	ColoredNoise noise[2];
	Rng hissRng;
	Rng pingRng;
	SafetyLimiter limiter;

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
	/** This sample's noise per side (already in every core's input; series cores get it again on top of the chain) */
	float noiseNow[2] = {};
	float coreGain[NUM_CORES] = {};
	float weights[NUM_CORES][4] = {{1.f, 0.f, 0.f, 0.f}, {1.f, 0.f, 0.f, 0.f}, {1.f, 0.f, 0.f, 0.f}, {1.f, 0.f, 0.f, 0.f}};
	float stackNow = 0.f;
	float limit = 0.f;
	float pingResp[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};

	// Character
	uint32_t characterSeed = 0, randomSeed = 0;
	float mismatchOct[NUM_CORES] = {};
	float mismatchQ[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	float mismatchGain[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	float randomPlaces[NUM_CORES] = {};
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

	// BAND
	BandStage bands[2][NUM_CORES];
	bool bandOn = false;
	float bandK[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	float bandGain[NUM_CORES] = {1.f, 1.f, 1.f, 1.f};
	float bandMix[NUM_CORES] = {};

	// Output
	float compGain = 1.f;
	float compStep = 0.f;
	float compDb = 0.f;
	int compCount = 0;
	/** Blocks without input (or right after a ping) */
	int silentBlocks = 0;
	float compSmooth = 0.1f, compRelax = 0.02f;
	float energyIn = 0.f, energyOut = 0.f;

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
		if (p.levelComp == LEVEL_OFF) {
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
	float dryBuf[2][64] = {};
	int dryPos[2] = {};

	/** The dry signal, delayed like the wet path (DRIVE latency), so MIX never combs. */
	float dryDelay(int c, float x) {
		dryBuf[c][dryPos[c]] = x;
		float y = dryBuf[c][(dryPos[c] - DriveStage::DRIVE_LATENCY) & 63];
		dryPos[c] = (dryPos[c] + 1) & 63;
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
		smooth(s.noise, p.noise, c);
		smooth(s.color, p.color, c);
		smooth(s.analog, p.analog, c);
		smooth(s.digital, p.digital, c);
		smooth(s.band, p.band, c);
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
		spreadOffsets(s.shape, spreadEff, p.scale, p.chord, randomPlaces, offs);
		// Each core's own spread (the dots on the ring)
		for (int i = 0; i < NUM_CORES; i++)
			offs[i] *= s.coreSpread[i];

		// TYPE (per core), STACK
		for (int i = 0; i < NUM_CORES; i++)
			typeWeights(s.coreType[i], weights[i]);
		stackNow = clamp01(s.stack + p.stackCv);

		// ANALOG: integrators saturate earlier, mismatch and pitch bloom grow
		float an = s.analog;
		limit = 8.f - 6.2f * an;
		static const float PAN[NUM_CORES] = {-1.f, 1.f / 3.f, -1.f / 3.f, 1.f};
		static const float WIDTH_SIGN[NUM_CORES] = {-1.f, 1.f, 1.f, -1.f};
		anyOwnPan = false;
		for (int i = 0; i < NUM_CORES; i++) {
			// Pitch bloom: a loud ring sags the core's pitch a little; it comes back as the ring decays.
			float level = std::min(cores[0][i].ringLevel(), 2.f);
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
				o = own + p.pitchCv[i] + an * mismatchOct[i] - an * 0.025f * bloom[i];
			}
			else {
				o = base + p.pitchCv[i] + offs[i] + an * mismatchOct[i] - an * 0.025f * bloom[i];
			}
			oct[i] = o;
			float res = clamp01(s.coreRes[i] + p.resCv[i]);
			float qScale = 1.f + an * (mismatchQ[i] - 1.f);
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
		// DIGITAL: coarse coefficients (above), sample-and-hold rate reduction, then coarse states at the top
		digitalOn = s.digital > 0.f;
		if (digitalOn) {
			float d = s.digital;
			holdLen = 1 + (int) std::lround(d * d * 31.f);
			stateStep = d > 0.7f ? std::exp2(-(16.f - 26.f * (d - 0.7f))) : 0.f;
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
	template <bool DIGITAL, bool BAND>
	inline void coresImpl(float x[2][NUM_CORES], const float exc[NUM_CORES], float hiss, float wet[2], EngineOutput& out) {
		float m = stackNow;
		float ycore[2][NUM_CORES];
		for (int c = 0; c < 2; c++) {
			float prev = x[c][0];
			float sum = 0.f;
			float last = 0.f;
			for (int i = 0; i < NUM_CORES; i++) {
				Core& core = cores[c][i];
				if (exc[i] != 0.f)
					core.excite(exc[i], weights[i][1] + weights[i][3]);
				// In series a core hears the previous core: give it the noise too, so NOISE reaches every core
				float u = x[c][i] + (prev + noiseNow[c] - x[c][i]) * m + hiss;
				float y = core.process(u, gNow[c][i], kNow[c][i], weights[i], bpGain[i], fbNow[i], limit) * coreGain[i];
				if (BAND) {
					float yb = bands[c][i].process(y, gNow[c][i], bandK[i]) * bandGain[i];
					y += (yb - y) * bandMix[i];
				}
				if (DIGITAL) {
					if (--holdCount[c][i] <= 0) {
						holdCount[c][i] = holdLen;
						holdVal[c][i] = y;
					}
					y = holdVal[c][i];
					if (stateStep > 0.f) {
						core.ic1 = std::round(core.ic1 / stateStep) * stateStep;
						core.ic2 = std::round(core.ic2 / stateStep) * stateStep;
					}
				}
				if (!std::isfinite(y)) {
					core.reset();
					y = 0.f;
				}
				ycore[c][i] = y;
				prev = y;
				last = y;
				sum += panGain[c][i] * y;
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

	void runCores(float x[2][NUM_CORES], const float exc[NUM_CORES], float hiss, float wet[2], EngineOutput& out) {
		coresImpl<false, false>(x, exc, hiss, wet, out);
	}
	NP_NOINLINE void runCoresDigital(float x[2][NUM_CORES], const float exc[NUM_CORES], float hiss, float wet[2],
			EngineOutput& out) {
		coresImpl<true, false>(x, exc, hiss, wet, out);
	}
	NP_NOINLINE void runCoresBand(float x[2][NUM_CORES], const float exc[NUM_CORES], float hiss, float wet[2],
			EngineOutput& out) {
		coresImpl<false, true>(x, exc, hiss, wet, out);
	}
	NP_NOINLINE void runCoresDigitalBand(float x[2][NUM_CORES], const float exc[NUM_CORES], float hiss, float wet[2],
			EngineOutput& out) {
		coresImpl<true, true>(x, exc, hiss, wet, out);
	}
};

} // namespace np
