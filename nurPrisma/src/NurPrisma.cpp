#include "NurPrisma.hpp"

#include <cmath>

#include "Layout.hpp"
#include "widgets/PanelText.hpp"
#include "widgets/PrismRing.hpp"

using namespace np::layout;

static const char* const SHAPE_NAMES[NurPrisma::NUM_SHAPES] = {"Even", "Harmonic", "Scale", "Random", "Chord"};

NurPrisma::NurPrisma() {
	config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
	// CUTOFF 0..1 = 20 Hz..20 kHz (log). Default ~2 kHz.
	configParam(CUTOFF_PARAM, 0.f, 1.f, 0.6667f, "Cutoff", " Hz", 1000.f, 20.f);
	configParam(SPREAD_PARAM, 0.f, 1.f, 0.3f, "Spread", "%", 0.f, 100.f);
	configParam(RES_PARAM, 0.f, 1.f, 0.35f, "Resonance", "%", 0.f, 100.f);
	configParam(TYPE_PARAM, 0.f, 3.f, 0.f, "Type (LP > BP > HP > notch)");
	configSwitch(SHAPE_PARAM, 0.f, NUM_SHAPES - 1, 0.f, "Spread shape",
		{SHAPE_NAMES[0], SHAPE_NAMES[1], SHAPE_NAMES[2], SHAPE_NAMES[3], SHAPE_NAMES[4]});
	configParam(STACK_PARAM, 0.f, 1.f, 0.f, "Stack (parallel > series)", "%", 0.f, 100.f);
	configParam(DRIVE_PARAM, 0.f, 1.f, 0.f, "Drive", "%", 0.f, 100.f);
	configParam(WIDTH_PARAM, 0.f, 1.f, 0.5f, "Stereo width", "%", 0.f, 100.f);
	configParam(MIX_PARAM, 0.f, 1.f, 1.f, "Dry / wet", "%", 0.f, 100.f);
	configParam(NOISE_PARAM, 0.f, 1.f, 0.f, "Noise", "%", 0.f, 100.f);
	configParam(COLOR_PARAM, 0.f, 1.f, 0.3333f, "Noise colour (white > pink > brown > crackle)");
	configParam(ANALOG_PARAM, 0.f, 1.f, 0.5f, "Analog character", "%", 0.f, 100.f);
	configParam(DIGITAL_PARAM, 0.f, 1.f, 0.f, "Digital character", "%", 0.f, 100.f);
	configParam(STRUM_PARAM, -1.f, 1.f, 0.f, "Strum (negative = downward)", "%", 0.f, 100.f);
	configParam(STRIKE_PARAM, 0.f, 1.f, 0.35f, "Strike (click > mallet > breath)", "%", 0.f, 100.f);
	configParam(BAND_PARAM, 0.f, 1.f, 0.f, "Band (narrow each core to a steep band: 3 > 1/24 octave)", "%", 0.f, 100.f);
	static const char* const CORE_NAMES[NUM_CORES] = {"Red", "Yellow", "Green", "Violet"};
	for (int i = 0; i < NUM_CORES; i++)
		configParam(CORE_SPREAD_PARAM + i, -1.f, 2.f, 1.f, string::f("%s core spread (drag its dot on the ring)", CORE_NAMES[i]),
			"%", 0.f, 100.f);
	for (int i = 0; i < NUM_CORES; i++) {
		configParam(CORE_TYPE_PARAM + i, 0.f, 3.f, 0.f, string::f("%s core type (LP > BP > HP > notch)", CORE_NAMES[i]));
		configParam(CORE_RES_PARAM + i, 0.f, 1.f, 0.35f, string::f("%s core resonance", CORE_NAMES[i]), "%", 0.f, 100.f);
		configParam(CORE_BAND_PARAM + i, 0.f, 1.f, 0.f, string::f("%s core band", CORE_NAMES[i]), "%", 0.f, 100.f);
		configParam(CORE_LEVEL_PARAM + i, 0.f, 2.f, 1.f, string::f("%s core level", CORE_NAMES[i]), "%", 0.f, 100.f);
		configParam(CORE_PAN_PARAM + i, -1.f, 1.f, 0.f, string::f("%s core pan (replaces WIDTH's place)", CORE_NAMES[i]),
			"%", 0.f, 100.f);
		configParam(CORE_CUTOFF_PARAM + i, 0.f, 1.f, 0.6667f, string::f("%s core cutoff (own, out of SPREAD)", CORE_NAMES[i]),
			" Hz", 1000.f, 20.f);
		own[i].store(0);
		for (int k = 0; k < NUM_OWN; k++)
			lastSeen[i][k] = NAN;
	}

	configInput(IN_L_INPUT, "Left / poly (one voice per core)");
	configInput(IN_R_INPUT, "Right / poly");
	configInput(VOCT_INPUT, "V/oct (poly: one per core)");
	configInput(CUTOFF_INPUT, "Cutoff CV (poly: one per core)");
	configInput(SPREAD_INPUT, "Spread CV");
	configInput(RES_INPUT, "Resonance CV (poly: one per core)");
	configInput(STACK_INPUT, "Stack CV");
	configInput(PING_INPUT, "Ping trigger (poly: one per core; voltage = velocity)");
	configOutput(OUT_L_OUTPUT, "Left");
	configOutput(OUT_R_OUTPUT, "Right");
	configOutput(CORES_OUTPUT, "Cores (4 channels, one per core)");
	configBypass(IN_L_INPUT, OUT_L_OUTPUT);
	configBypass(IN_R_INPUT, OUT_R_OUTPUT);

	for (int i = 0; i < NUM_CORES; i++)
		corePos[i].store(0.5f);
	// Every instance gets its own analog character and its own Random places (both saved in the patch).
	engine.seedCharacter(random::u32());
	engine.seedRandomShape(random::u32());
}

void NurPrisma::onSampleRateChange(const SampleRateChangeEvent& e) {
	engine.setSampleRate(e.sampleRate);
}

void NurPrisma::onReset(const ResetEvent& e) {
	Module::onReset(e);
	scale = 0;
	chord = 0;
	pingMode = np::PING_ALL;
	polySplit = true;
	quantizeCutoff = false;
	levelComp = np::LEVEL_FULL;
	focus = -1;
	for (int i = 0; i < NUM_CORES; i++) {
		own[i].store(0);
		for (int k = 0; k < NUM_OWN; k++)
			lastSeen[i][k] = NAN;
	}
	engine.reset();
}

/** The per-core twins that can be owned, in OWN_* bit order */
static const int OWN_CORE[NurPrisma::NUM_OWN] = {NurPrisma::CORE_TYPE_PARAM, NurPrisma::CORE_RES_PARAM,
	NurPrisma::CORE_BAND_PARAM, NurPrisma::CORE_CUTOFF_PARAM, NurPrisma::CORE_PAN_PARAM};

float NurPrisma::followValue(int core, int k) {
	switch (k) {
		case 0: return params[TYPE_PARAM].getValue();
		case 1: return params[RES_PARAM].getValue();
		case 2: return params[BAND_PARAM].getValue();
		// Where SPREAD puts the core now (engine, last block): taking an own cutoff starts there
		case 3: return clamp(engine.place[core], 0.f, 1.f);
		default: return params[WIDTH_PARAM].getValue() * np::widthPlace(core);
	}
}

void NurPrisma::setFocus(int core) {
	if (core >= 0 && core < NUM_CORES) {
		int mask = own[core].load();
		for (int k = 0; k < NUM_OWN; k++) {
			if (!(mask & (1 << k))) {
				float v = followValue(core, k);
				params[OWN_CORE[k] + core].setValue(v);
				lastSeen[core][k] = v;
			}
		}
	}
	focus = (core >= 0 && core < NUM_CORES) ? core : -1;
}

void NurPrisma::followGlobal(int core) {
	if (core < 0 || core >= NUM_CORES)
		return;
	own[core].store(0);
	for (int k = 0; k < NUM_OWN; k++) {
		float v = followValue(core, k);
		params[OWN_CORE[k] + core].setValue(v);
		lastSeen[core][k] = v;
	}
}

void NurPrisma::resetCore(int core) {
	if (core < 0 || core >= NUM_CORES)
		return;
	followGlobal(core);
	params[CORE_LEVEL_PARAM + core].setValue(1.f);
	params[CORE_SPREAD_PARAM + core].setValue(1.f);
}

void NurPrisma::checkOwnership() {
	for (int i = 0; i < NUM_CORES; i++)
		for (int k = 0; k < NUM_OWN; k++) {
			float v = params[OWN_CORE[k] + i].getValue();
			if (std::isnan(lastSeen[i][k]))
				lastSeen[i][k] = v;
			else if (v != lastSeen[i][k]) {
				lastSeen[i][k] = v;
				own[i].fetch_or(1 << k);
			}
		}
}

bool NurPrisma::coreIsCustom(int core) {
	return own[core].load() != 0 || params[CORE_LEVEL_PARAM + core].getValue() != 1.f ||
		params[CORE_SPREAD_PARAM + core].getValue() != 1.f;
}

void NurPrisma::rerollCharacter() {
	engine.seedCharacter(random::u32());
}

void NurPrisma::rerollRandomShape() {
	engine.seedRandomShape(random::u32());
}

json_t* NurPrisma::dataToJson() {
	json_t* root = json_object();
	json_object_set_new(root, "scale", json_integer(scale));
	json_object_set_new(root, "chord", json_integer(chord));
	json_object_set_new(root, "pingMode", json_integer(pingMode));
	json_object_set_new(root, "polySplit", json_boolean(polySplit));
	json_object_set_new(root, "quantizeCutoff", json_boolean(quantizeCutoff));
	json_object_set_new(root, "levelComp", json_integer(levelComp));
	json_object_set_new(root, "focus", json_integer(focus));
	json_t* ownJ = json_array();
	for (int i = 0; i < NUM_CORES; i++)
		json_array_append_new(ownJ, json_integer(own[i].load()));
	json_object_set_new(root, "own", ownJ);
	json_t* offJ = json_array();
	for (int i = 0; i < NUM_CORES; i++)
		json_array_append_new(offJ, std::isnan(cutoffOffset[i]) ? json_null() : json_real(cutoffOffset[i]));
	json_object_set_new(root, "cutoffOffset", offJ);
	// Seeds as doubles: json integers are signed, the seeds use all 32 bits
	json_object_set_new(root, "characterSeed", json_real((double) engine.getCharacterSeed()));
	json_object_set_new(root, "randomSeed", json_real((double) engine.getRandomSeed()));
	return root;
}

void NurPrisma::dataFromJson(json_t* root) {
	auto getInt = [&](const char* key, int lo, int hi, int& out) {
		json_t* j = json_object_get(root, key);
		if (j)
			out = std::min(std::max((int) json_integer_value(j), lo), hi);
	};
	auto getBool = [&](const char* key, bool& out) {
		json_t* j = json_object_get(root, key);
		if (j)
			out = json_boolean_value(j);
	};
	getInt("scale", 0, np::NUM_SCALES - 1, scale);
	getInt("chord", 0, np::NUM_CHORDS - 1, chord);
	getInt("pingMode", 0, np::NUM_PING_MODES - 1, pingMode);
	getInt("levelComp", 0, np::NUM_LEVEL_COMPS - 1, levelComp);
	getInt("focus", -1, NUM_CORES - 1, focus);
	json_t* ownJ = json_object_get(root, "own");
	for (int i = 0; i < NUM_CORES; i++) {
		json_t* o = ownJ ? json_array_get(ownJ, i) : nullptr;
		own[i].store(o ? (int) json_integer_value(o) & 31 : 0);
		for (int k = 0; k < NUM_OWN; k++)
			lastSeen[i][k] = NAN;
	}
	// Offsets win over the (possibly pinned) saved CORE_CUTOFF values: the params are already loaded here, so mark
	// them as written by us.
	json_t* offJ = json_object_get(root, "cutoffOffset");
	for (int i = 0; i < NUM_CORES; i++) {
		json_t* o = offJ ? json_array_get(offJ, i) : nullptr;
		cutoffOffset[i] = (o && json_is_number(o)) ? (float) json_number_value(o) : NAN;
		cutoffWritten[i] = std::isnan(cutoffOffset[i]) ? NAN : params[CORE_CUTOFF_PARAM + i].getValue();
	}
	getBool("polySplit", polySplit);
	getBool("quantizeCutoff", quantizeCutoff);
	json_t* c = json_object_get(root, "characterSeed");
	if (c)
		engine.seedCharacter((uint32_t) json_number_value(c));
	json_t* r = json_object_get(root, "randomSeed");
	if (r)
		engine.seedRandomShape((uint32_t) json_number_value(r));
}

void NurPrisma::readParams() {
	np::EngineParams& p = engine.p;
	// CUTOFF moves every core. A core with its own CUTOFF keeps its *offset* from CUTOFF (remembered apart from the
	// value it plays at): it may pin at the end of the range while CUTOFF goes far, and comes back exactly to where
	// it was set when CUTOFF returns. A change of the core's own value that we did not write (its knob in focus, a
	// MIDI map, a loaded patch without offsets) sets a new offset.
	float cutoff = params[CUTOFF_PARAM].getValue();
	for (int i = 0; i < NUM_CORES; i++) {
		if (!(own[i].load(std::memory_order_relaxed) & OWN_CUTOFF)) {
			cutoffWritten[i] = NAN;
			continue;
		}
		Param& c = params[CORE_CUTOFF_PARAM + i];
		float v = c.getValue();
		if (std::isnan(cutoffOffset[i]) || std::isnan(cutoffWritten[i]) || v != cutoffWritten[i])
			cutoffOffset[i] = v - cutoff;
		float target = clamp(cutoff + cutoffOffset[i], 0.f, 1.f);
		if (target != v)
			c.setValue(target);
		cutoffWritten[i] = target;
	}
	p.cutoff = cutoff;
	p.spread = params[SPREAD_PARAM].getValue();
	p.res = params[RES_PARAM].getValue();
	p.type = params[TYPE_PARAM].getValue();
	p.stack = params[STACK_PARAM].getValue();
	p.drive = params[DRIVE_PARAM].getValue();
	p.width = params[WIDTH_PARAM].getValue();
	p.mix = params[MIX_PARAM].getValue();
	p.noise = params[NOISE_PARAM].getValue();
	p.color = params[COLOR_PARAM].getValue();
	p.analog = params[ANALOG_PARAM].getValue();
	p.digital = params[DIGITAL_PARAM].getValue();
	p.strum = params[STRUM_PARAM].getValue();
	p.strike = params[STRIKE_PARAM].getValue();
	p.band = params[BAND_PARAM].getValue();
	for (int i = 0; i < NUM_CORES; i++) {
		p.coreSpread[i] = params[CORE_SPREAD_PARAM + i].getValue();
		int mask = own[i].load(std::memory_order_relaxed);
		p.ownType[i] = mask & OWN_TYPE;
		p.ownRes[i] = mask & OWN_RES;
		p.ownBand[i] = mask & OWN_BAND;
		p.coreType[i] = params[CORE_TYPE_PARAM + i].getValue();
		p.coreRes[i] = params[CORE_RES_PARAM + i].getValue();
		p.coreBand[i] = params[CORE_BAND_PARAM + i].getValue();
		p.coreLevel[i] = params[CORE_LEVEL_PARAM + i].getValue();
		p.corePan[i] = params[CORE_PAN_PARAM + i].getValue();
		p.ownPan[i] = mask & OWN_PAN;
		p.coreCutoff[i] = params[CORE_CUTOFF_PARAM + i].getValue();
		p.ownCutoff[i] = mask & OWN_CUTOFF;
	}
	p.shape = (int) params[SHAPE_PARAM].getValue();
	p.scale = scale;
	p.chord = chord;
	p.pingMode = pingMode;
	p.quantizeCutoff = quantizeCutoff;
	p.levelComp = levelComp;

	// CV: V/OCT and CUTOFF are 1 V/oct, RES / SPREAD / STACK 10 V = the whole knob. Poly = one channel per core.
	Input& voct = inputs[VOCT_INPUT];
	Input& cut = inputs[CUTOFF_INPUT];
	Input& res = inputs[RES_INPUT];
	for (int i = 0; i < NUM_CORES; i++) {
		p.pitchCv[i] = voct.getPolyVoltage(i) + cut.getPolyVoltage(i);
		p.resCv[i] = 0.1f * res.getPolyVoltage(i);
	}
	p.spreadCv = 0.1f * inputs[SPREAD_INPUT].getVoltage();
	p.stackCv = 0.1f * inputs[STACK_INPUT].getVoltage();
}

void NurPrisma::process(const ProcessArgs& args) {
	if (paramCounter-- <= 0) {
		paramCounter = np::PrismaEngine::BLOCK - 1;
		readParams();
		int shape = (int) params[SHAPE_PARAM].getValue();
		for (int i = 0; i < NUM_SHAPES; i++)
			lights[SHAPE_LIGHT + i].setBrightness(i == shape ? 1.f : 0.f);
		for (int i = 0; i < NUM_CORES; i++)
			corePos[i].store(engineOut.corePos[i], std::memory_order_relaxed);
	}

	// PING: poly = one channel per core; mono = per the ping mode (strummed / cycle / random). Voltage = velocity.
	Input& pingIn = inputs[PING_INPUT];
	int pingChannels = pingIn.getChannels();
	for (int c = 0; c < pingChannels; c++) {
		float v = pingIn.getVoltage(c);
		if (pingTrigger[c].process(v, 0.1f, 1.f)) {
			float vel = std::min(std::max(v * 0.1f, 0.1f), 1.f);
			if (pingChannels > 1)
				engine.ping(c % NUM_CORES, vel);
			else
				engine.pingAll(vel);
		}
	}

	// Inputs: 5 V = 1.0. Poly (2+ channels) = one voice per core; R unpatched = L.
	Input& inL = inputs[IN_L_INPUT];
	Input& inR = inputs[IN_R_INPUT];
	bool rConnected = inR.isConnected();
	int nL = inL.getChannels();
	int nR = rConnected ? inR.getChannels() : nL;
	bool perCore = polySplit && (nL > 1 || nR > 1);
	float xl[NUM_CORES], xr[NUM_CORES];
	float dryL = 0.2f * inL.getVoltageSum();
	float dryR = rConnected ? 0.2f * inR.getVoltageSum() : dryL;
	if (perCore) {
		// A mono side (poly on the other) feeds all four cores.
		if (nL > 1)
			np::routeVoices(inL.getVoltages(), nL, xl);
		else
			std::fill(xl, xl + NUM_CORES, inL.getVoltage());
		if (rConnected && nR > 1)
			np::routeVoices(inR.getVoltages(), nR, xr);
		else if (rConnected)
			std::fill(xr, xr + NUM_CORES, inR.getVoltage());
		for (int i = 0; i < NUM_CORES; i++) {
			xl[i] *= 0.2f;
			xr[i] = rConnected ? 0.2f * xr[i] : xl[i];
		}
	}
	else {
		xl[0] = dryL;
		xr[0] = dryR;
	}

	engine.process(xl, xr, perCore, dryL, dryR, engineOut);

	outputs[OUT_L_OUTPUT].setVoltage(5.f * engineOut.l);
	outputs[OUT_R_OUTPUT].setVoltage(5.f * engineOut.r);
	if (outputs[CORES_OUTPUT].isConnected()) {
		outputs[CORES_OUTPUT].setChannels(NUM_CORES);
		for (int i = 0; i < NUM_CORES; i++)
			outputs[CORES_OUTPUT].setVoltage(clamp(5.f * engineOut.cores[i], -10.f, 10.f), i);
	}
}

/** The core selector under the hero: an ALL pill and four bullets in the cores' colours. They never move, so a core
can be picked while its dot on the ring is hard to catch. Click a bullet: TYPE, RES, BAND, the SPREAD
ring, WIDTH (-> PAN) and MIX (-> LEVEL) edit that core; click it again or ALL to go back. A ring around a bullet
means that core has settings of its own. Right-click a bullet: follow the global knobs again, or reset the core. */
struct CoreSelector : widget::OpaqueWidget {
	NurPrisma* module = nullptr;

	Vec bulletPos(int i) {
		return mm2px(Vec(BULLET_X0 + i * BULLET_PITCH, BULLET_Y)).minus(box.pos);
	}
	Vec allPos() {
		return mm2px(Vec(BULLET_ALL_X, BULLET_Y)).minus(box.pos);
	}
	/** -1 = ALL, 0..3 = a bullet, -2 = nothing */
	int hit(Vec p) {
		for (int i = 0; i < NurPrisma::NUM_CORES; i++)
			if (p.minus(bulletPos(i)).norm() < mm2px(2.4f))
				return i;
		Vec a = allPos();
		if (std::fabs(p.x - a.x) < mm2px(3.4f) && std::fabs(p.y - a.y) < mm2px(2.f))
			return -1;
		return -2;
	}

	void onButton(const ButtonEvent& e) override {
		if (e.action != GLFW_PRESS || !module)
			return;
		int h = hit(e.pos);
		if (h == -2)
			return;
		e.consume(this);
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			module->setFocus((h >= 0 && h != module->focus) ? h : -1);
		}
		else if (e.button == GLFW_MOUSE_BUTTON_RIGHT && h >= 0) {
			static const char* const NAMES[NurPrisma::NUM_CORES] = {"Red", "Yellow", "Green", "Violet"};
			NurPrisma* m = module;
			ui::Menu* menu = createMenu();
			menu->addChild(createMenuLabel(string::f("%s core", NAMES[h])));
			menu->addChild(createMenuItem("Edit this core", "", [=]() { m->setFocus(h); }));
			menu->addChild(createMenuItem("Follow the global knobs again (TYPE, RES, BAND, CUTOFF, WIDTH)", "",
				[=]() { m->followGlobal(h); }));
			menu->addChild(createMenuItem("Reset (follow everything, level 100%, spread 100%)", "", [=]() { m->resetCore(h); }));
		}
	}

	void drawBullets(const DrawArgs& args, bool lit) {
		NVGcontext* vg = args.vg;
		int f = module ? module->focus : -1;
		// ALL pill
		Vec a = allPos();
		float w = mm2px(6.4f), hgt = mm2px(3.4f);
		if (!lit) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, a.x - w / 2, a.y - hgt / 2, w, hgt, hgt / 2);
			nvgFillColor(vg, f < 0 ? nvgRGB(0xe9, 0xe3, 0xd5) : nvgRGB(0x2a, 0x2c, 0x31));
			nvgFill(vg);
			nvgStrokeColor(vg, nvgRGB(0x3a, 0x3c, 0x42));
			nvgStrokeWidth(vg, mm2px(0.2f));
			nvgStroke(vg);
			std::shared_ptr<window::Font> font =
				APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/Jost-SemiBold.ttf"));
			if (font) {
				nvgFontFaceId(vg, font->handle);
				nvgFontSize(vg, mm2px(1.7f));
				nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgFillColor(vg, f < 0 ? nvgRGB(0x14, 0x15, 0x19) : nvgRGB(0x8d, 0x8b, 0x86));
				nvgText(vg, a.x, a.y + 0.2f, "ALL", nullptr);
			}
		}
		for (int i = 0; i < NurPrisma::NUM_CORES; i++) {
			Vec b = bulletPos(i);
			NVGcolor col = np::coreColor(i);
			bool sel = f == i;
			if (lit && sel) {
				NVGcolor glow = col;
				glow.a = 0.3f;
				nvgBeginPath(vg);
				nvgCircle(vg, b.x, b.y, mm2px(2.3f));
				nvgFillColor(vg, glow);
				nvgFill(vg);
			}
			if (!lit && module && module->coreIsCustom(i)) {
				nvgBeginPath(vg);
				nvgCircle(vg, b.x, b.y, mm2px(1.85f));
				nvgStrokeColor(vg, nvgRGB(0xe9, 0xe3, 0xd5));
				nvgStrokeWidth(vg, mm2px(0.25f));
				nvgStroke(vg);
			}
			if (lit == sel) {
				col.a = sel ? 1.f : 0.55f;
				nvgBeginPath(vg);
				nvgCircle(vg, b.x, b.y, mm2px(1.3f));
				nvgFillColor(vg, col);
				nvgFill(vg);
			}
		}
	}

	void draw(const DrawArgs& args) override {
		drawBullets(args, false);
	}
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			drawBullets(args, true);
		Widget::drawLayer(args, layer);
	}
};

struct NurPrismaWidget : app::ModuleWidget {
	/** Controls that edit one core while it is in focus: [which][0 = global, 1..4 = core] */
	enum { F_TYPE, F_RES, F_BAND, F_WIDTH, F_MIX, F_RING, F_CUTOFF, NUM_FOCUS };
	app::ParamWidget* focusWidgets[NUM_FOCUS][1 + NurPrisma::NUM_CORES] = {};

	/** One global knob and four per-core twins at the same place; step() shows the one in focus. */
	template <class TKnob>
	void addFocusKnob(int which, Vec mmPos, int globalParam, int coreParamBase) {
		NurPrisma* m = dynamic_cast<NurPrisma*>(module);
		for (int k = 0; k <= NurPrisma::NUM_CORES; k++) {
			app::ParamWidget* w = createParamCentered<TKnob>(mm2px(mmPos), m, k == 0 ? globalParam : coreParamBase + k - 1);
			w->visible = k == 0;
			focusWidgets[which][k] = w;
			addParam(w);
		}
	}

	void step() override {
		NurPrisma* m = dynamic_cast<NurPrisma*>(module);
		if (m) {
			m->checkOwnership();
			int f = m->focus;
			for (int which = 0; which < NUM_FOCUS; which++)
				for (int k = 0; k <= NurPrisma::NUM_CORES; k++)
					if (focusWidgets[which][k])
						focusWidgets[which][k]->visible = (k == f + 1);
		}
		ModuleWidget::step();
	}

	NurPrismaWidget(NurPrisma* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/NurPrisma.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		np::PanelText* text = createWidget<np::PanelText>(Vec(0, 0));
		text->box.size = box.size;
		const NVGcolor cream = np::PanelText::cream();
		const NVGcolor grey = np::PanelText::grey();
		const NVGcolor gold = np::PanelText::gold();
		text->add(Vec(FILTER_CX, TITLE_Y), "NUR PRISMA", TITLE_SIZE, cream, np::PanelText::TITLE, 0.06f);
		text->add(Vec(FILTER_CX, SUBTITLE_Y), "SPREAD FILTER", SUBTITLE_SIZE, grey, np::PanelText::CAPTION, 0.45f);
		if (module)
			text->focus = &module->focus;
		text->coreColor = np::coreColor;
		// Shown inside the bottom of the ring while CUTOFF snaps to the scale (right-click), so a stepped CUTOFF is
		// never a mystery
		if (module)
			text->addIf(Vec(FILTER_CX, HERO_SCALE_Y), "SCALE", 1.8f, gold, &module->quantizeCutoff);
		text->addFocus(Vec(FILTER_CX, HERO_LABEL_Y), "CUTOFF  \xc2\xb7  SPREAD", "CUTOFF  \xc2\xb7  CORE SPREAD", 2.3f, cream,
			np::PanelText::LABEL, 0.3f);

		auto knob = [&](app::ParamWidget* w, float x, float y, float d, const char* label, float size) {
			addParam(w);
			text->add(Vec(x, y + d / 2.f + LABEL_GAP), label, size, cream, np::PanelText::LABEL, 0.1f);
		};
		addFocusKnob<RoundLargeBlackKnob>(F_TYPE, Vec(TYPE_X, TOP_Y), NurPrisma::TYPE_PARAM, NurPrisma::CORE_TYPE_PARAM);
		text->addFocus(Vec(TYPE_X, TOP_Y + KNOB_LARGE / 2.f + LABEL_GAP), "TYPE", "", 2.6f, cream);
		addFocusKnob<RoundLargeBlackKnob>(F_RES, Vec(RES_X, TOP_Y), NurPrisma::RES_PARAM, NurPrisma::CORE_RES_PARAM);
		text->addFocus(Vec(RES_X, TOP_Y + KNOB_LARGE / 2.f + LABEL_GAP), "RES", "", 2.6f, cream);

		knob(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(STRUM_X, TOP_Y)), module, NurPrisma::STRUM_PARAM),
			STRUM_X, TOP_Y, KNOB_SMALL, "STRUM", 2.1f);
		knob(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(STRIKE_X, TOP_Y)), module, NurPrisma::STRIKE_PARAM),
			STRIKE_X, TOP_Y, KNOB_SMALL, "STRIKE", 2.1f);

		// SHAPE: a button that steps through the five spacings, with one LED per spacing.
		addParam(createParamCentered<np::CycleButton>(mm2px(Vec(FILTER_CX, SHAPE_Y)), module, NurPrisma::SHAPE_PARAM));
		for (int i = 0; i < NurPrisma::NUM_SHAPES; i++)
			addChild(createLightCentered<SmallLight<WhiteLight>>(
				mm2px(Vec(FILTER_CX + (i - 2.f) * SHAPE_LED_PITCH, SHAPE_Y + SHAPE_LED_DY)), module,
				NurPrisma::SHAPE_LIGHT + i));
		text->add(Vec(FILTER_CX, SHAPE_Y + SHAPE_LED_DY + 3.f), "SHAPE", 2.2f, cream, np::PanelText::LABEL, 0.1f);

		// Hero: the ring first, the CUTOFF knob on top.
		np::PrismRing* ring = nullptr;
		for (int k = 0; k <= NurPrisma::NUM_CORES; k++) {
			np::PrismRing* r = createParam<np::PrismRing>(Vec(0, 0), module,
				k == 0 ? (int) NurPrisma::SPREAD_PARAM : NurPrisma::CORE_SPREAD_PARAM + k - 1);
			r->setDiameterMm(HERO_RING_D);
			r->box.pos = mm2px(Vec(FILTER_CX, HERO_Y)).minus(r->box.size.div(2.f));
			if (module)
				r->corePos = module->corePos;
			r->visible = k == 0;
			focusWidgets[F_RING][k] = r;
			addParam(r);
			if (k == 0)
				ring = r;
		}
		addFocusKnob<np::HeroKnob>(F_CUTOFF, Vec(FILTER_CX, HERO_Y), NurPrisma::CUTOFF_PARAM, NurPrisma::CORE_CUTOFF_PARAM);
		// The four core dots, on top of everything in the hero: each is its core's own spread control.
		for (int i = 0; i < NurPrisma::NUM_CORES; i++) {
			np::CoreDot* dot = createParam<np::CoreDot>(Vec(0, 0), module, NurPrisma::CORE_SPREAD_PARAM + i);
			dot->ring = ring;
			dot->index = i;
			if (module) {
				dot->focus = &module->focus;
				dot->ownBits = &module->own[i];
			}
			dot->spreadParam = NurPrisma::CORE_SPREAD_PARAM + i;
			dot->cutoffParam = NurPrisma::CORE_CUTOFF_PARAM + i;
			dot->cutoffBit = NurPrisma::OWN_CUTOFF;
			addParam(dot);
		}

		addFocusKnob<RoundSmallBlackKnob>(F_BAND, Vec(SIDE_L_X, SIDE_Y1), NurPrisma::BAND_PARAM, NurPrisma::CORE_BAND_PARAM);
		text->addFocus(Vec(SIDE_L_X, SIDE_Y1 + KNOB_SMALL / 2.f + LABEL_GAP), "BAND", "", 2.1f, cream);
		knob(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(SIDE_L_X, SIDE_Y2)), module, NurPrisma::STACK_PARAM),
			SIDE_L_X, SIDE_Y2, KNOB_SMALL, "STACK", 2.1f);
		addFocusKnob<RoundSmallBlackKnob>(F_WIDTH, Vec(SIDE_R_X, SIDE_Y1), NurPrisma::WIDTH_PARAM, NurPrisma::CORE_PAN_PARAM);
		text->addFocus(Vec(SIDE_R_X, SIDE_Y1 + KNOB_SMALL / 2.f + LABEL_GAP), "WIDTH", "PAN", 2.1f, cream);
		addFocusKnob<RoundSmallBlackKnob>(F_MIX, Vec(SIDE_R_X, SIDE_Y2), NurPrisma::MIX_PARAM, NurPrisma::CORE_LEVEL_PARAM);
		text->addFocus(Vec(SIDE_R_X, SIDE_Y2 + KNOB_SMALL / 2.f + LABEL_GAP), "MIX", "LEVEL", 2.1f, cream);

		CoreSelector* selector = createWidget<CoreSelector>(mm2px(Vec(BULLET_ALL_X - 4.f, BULLET_Y - 3.f)));
		selector->box.size = mm2px(Vec(BULLET_X0 + 3 * BULLET_PITCH + 3.f - (BULLET_ALL_X - 4.f), 6.f));
		selector->module = module;
		addChild(selector);

		static const int CHAR_PARAMS[5] = {NurPrisma::DRIVE_PARAM, NurPrisma::NOISE_PARAM, NurPrisma::COLOR_PARAM,
			NurPrisma::ANALOG_PARAM, NurPrisma::DIGITAL_PARAM};
		static const char* const CHAR_LABELS[5] = {"DRIVE", "NOISE", "COLOR", "ANALOG", "DIGITAL"};
		for (int i = 0; i < 5; i++) {
			addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(CHAR_X[i], CHAR_Y)), module, CHAR_PARAMS[i]));
			text->add(Vec(CHAR_X[i], CHAR_Y + KNOB_MEDIUM / 2.f + CHAR_LABEL_GAP), CHAR_LABELS[i], 2.2f, cream,
				np::PanelText::LABEL, 0.1f);
		}


		// Jacks
		auto jackLabel = [&](float x, float y, const char* label, bool out) {
			text->add(Vec(x, y + JACK_LABEL_DY), label, 2.f, out ? gold : grey, np::PanelText::CAPTION, 0.1f);
		};
		static const int CV_INPUTS[6] = {NurPrisma::PING_INPUT, NurPrisma::VOCT_INPUT, NurPrisma::CUTOFF_INPUT, NurPrisma::SPREAD_INPUT,
			NurPrisma::RES_INPUT, NurPrisma::STACK_INPUT};
		static const char* const CV_LABELS[6] = {"PING", "V/OCT", "CUTOFF", "SPREAD", "RES", "STACK"};
		for (int i = 0; i < 6; i++) {
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(CV_X[i], CV_Y)), module, CV_INPUTS[i]));
			jackLabel(CV_X[i], CV_Y, CV_LABELS[i], false);
		}
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(IO_X[0], IO_Y)), module, NurPrisma::IN_L_INPUT));
		jackLabel(IO_X[0], IO_Y, "IN L", false);
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(IO_X[1], IO_Y)), module, NurPrisma::IN_R_INPUT));
		jackLabel(IO_X[1], IO_Y, "IN R", false);
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(IO_X[2], IO_Y)), module, NurPrisma::CORES_OUTPUT));
		jackLabel(IO_X[2], IO_Y, "CORES", false);
		addOutput(createOutputCentered<np::GoldJack>(mm2px(Vec(IO_X[3], IO_Y)), module, NurPrisma::OUT_L_OUTPUT));
		jackLabel(IO_X[3], IO_Y, "OUT L", true);
		addOutput(createOutputCentered<np::GoldJack>(mm2px(Vec(IO_X[4], IO_Y)), module, NurPrisma::OUT_R_OUTPUT));
		jackLabel(IO_X[4], IO_Y, "OUT R", true);


		// Text last: on top of the panel, below nothing interactive (it is transparent to the mouse).
		addChild(text);
	}

	void appendContextMenu(ui::Menu* menu) override {
		NurPrisma* m = getModule<NurPrisma>();
		if (!m)
			return;
		menu->addChild(new ui::MenuSeparator);
		menu->addChild(createMenuLabel("Ping"));
		menu->addChild(createIndexSubmenuItem("Ping mode (mono PING)", {"All (strummed chord)", "Cycle (next core)", "Random core"},
			[=]() { return m->pingMode; }, [=](int i) { m->pingMode = i; }));
		menu->addChild(new ui::MenuSeparator);
		menu->addChild(createMenuLabel("Pitch"));
		std::vector<std::string> scales, chords;
		for (int i = 0; i < np::NUM_SCALES; i++)
			scales.push_back(np::SCALES[i].name);
		for (int i = 0; i < np::NUM_CHORDS; i++)
			chords.push_back(np::CHORDS[i].name);
		menu->addChild(createIndexSubmenuItem("Scale (SHAPE Scale, quantize)", scales, [=]() { return m->scale; },
			[=](int i) { m->scale = i; }));
		menu->addChild(createIndexSubmenuItem("Chord (SHAPE Chord)", chords, [=]() { return m->chord; },
			[=](int i) { m->chord = i; }));
		menu->addChild(createBoolPtrMenuItem("Quantize CUTOFF to the scale (SCALE shows in the ring)", "", &m->quantizeCutoff));
		menu->addChild(createMenuItem("Re-roll Random shape", "", [=]() { m->rerollRandomShape(); }));
		menu->addChild(new ui::MenuSeparator);
		menu->addChild(createMenuLabel("Input and output"));
		menu->addChild(createIndexSubmenuItem("Level compensation",
			{"Off (filters change the level)", "Gentle (+-6 dB)", "Full (keep the input's loudness)"},
			[=]() { return m->levelComp; }, [=](int i) { m->levelComp = i; }));
		menu->addChild(createBoolPtrMenuItem("Poly input: one voice per core", "", &m->polySplit));
		menu->addChild(createMenuItem("Re-roll analog character", "", [=]() { m->rerollCharacter(); }));
	}
};

Model* modelNurPrisma = createModel<NurPrisma, NurPrismaWidget>("NurPrisma");
