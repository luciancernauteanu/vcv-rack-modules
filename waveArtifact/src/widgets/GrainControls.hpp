#pragma once
#include "../GrainEngine.hpp"
#include "../plugin.hpp"
#include "LfoControls.hpp"
#include "LfoWidgets.hpp"

// Controls of the granular engine (the knobs that change meaning when the engine switch is on GRAIN).

/** Knob display that depends on the cloud's SYNC / T / D switches (other params of the same module). */
struct GrainSyncQuantity : ParamQuantity {
	int syncId = -1;
	int tripletId = -1;
	int dottedId = -1;

	bool flag(int paramId) {
		return module && paramId >= 0 && module->params[paramId].getValue() > 0.5f;
	}
	bool isSync() {
		return flag(syncId);
	}
	std::string getUnit() override {
		return "";
	}
};

/** DENSITY: grains per second, or the note value of the grain grid while synced. */
struct GrainDensityQuantity : GrainSyncQuantity {
	std::string getDisplayValueString() override {
		if (isSync())
			return wa::SYNC_DIVISIONS[wa::lfoDivision(getValue(), flag(tripletId), flag(dottedId))].name;
		float hz = wa::grainFreeHz(getValue());
		return string::f(hz < 10.f ? "%.2f grains/s" : "%.1f grains/s", hz);
	}
};

/** SIZE: milliseconds, or a multiple of the grain interval while synced. */
struct GrainSizeQuantity : GrainSyncQuantity {
	std::string getDisplayValueString() override {
		if (isSync())
			return string::f("%.2f x interval", wa::grainSizeSyncRatio(getValue()));
		float s = wa::grainSizeFreeSeconds(getValue());
		return s < 0.1f ? string::f("%.1f ms", s * 1000.f) : s < 1.f ? string::f("%.0f ms", s * 1000.f) : string::f("%.2f s", s);
	}
};

/** SCAN: speed through the slice, or the number of beats the slice lasts while synced. */
struct GrainScanQuantity : GrainSyncQuantity {
	std::string getDisplayValueString() override {
		float v = getValue();
		if (!wa::grainScanActive(v))
			return "off (stays at START)";
		if (isSync())
			return string::f(v < 0.f ? "%g beats, reverse" : "%g beats", wa::grainLoopBeats(v));
		return string::f("%+.2f x real time", wa::grainScanSpeed(v));
	}
};

struct GrainSprayQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float s = wa::grainSpraySeconds(getValue());
		return s < 1.f ? string::f("+-%.0f ms", s * 1000.f) : string::f("+-%.2f s", s);
	}
	std::string getUnit() override {
		return "";
	}
};

struct GrainPitchScatterQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		return string::f("+-%.1f st", wa::grainPitchScatterSemitones(getValue()));
	}
	std::string getUnit() override {
		return "";
	}
};

inline std::vector<std::string> grainWindowNames() {
	return std::vector<std::string>(wa::GRAIN_WINDOW_NAMES, wa::GRAIN_WINDOW_NAMES + wa::NUM_GRAIN_WINDOWS);
}

inline std::vector<std::string> grainDirectionNames() {
	return std::vector<std::string>(wa::GRAIN_DIRECTION_NAMES, wa::GRAIN_DIRECTION_NAMES + wa::NUM_GRAIN_DIRECTIONS);
}

/** Mini screen that draws the grain window. Left-click steps to the next window (undoable), right-click lists them. */
struct GrainWindowScreen : LfoShapeScreen {
	void draw(const DrawArgs& args) override {
		engine::ParamQuantity* pq = getParamQuantity();
		int shape = pq ? clamp((int) std::round(pq->getValue()), 0, wa::NUM_GRAIN_WINDOWS - 1) : 0;

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
		nvgFillColor(args.vg, nvgRGB(0x0b, 0x0d, 0x14));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgTransRGBAf(color, 0.45f));
		nvgStrokeWidth(args.vg, 0.7f);
		nvgStroke(args.vg);

		float padX = box.size.x * 0.1f, padY = box.size.y * 0.18f;
		float w = box.size.x - 2.f * padX, h = box.size.y - 2.f * padY;
		const int N = 48;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, padX, padY + h);
		for (int k = 0; k <= N; k++) {
			float x = (float) k / N;
			nvgLineTo(args.vg, padX + w * x, padY + h * (1.f - wa::grainWindowValue(shape, x)));
		}
		nvgLineTo(args.vg, padX + w, padY + h);
		nvgClosePath(args.vg);
		nvgFillColor(args.vg, nvgTransRGBAf(color, 0.18f));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, color);
		nvgStrokeWidth(args.vg, 1.1f);
		nvgLineJoin(args.vg, NVG_ROUND);
		nvgStroke(args.vg);
	}

	void appendContextMenu(Menu* menu) override {
		Module* m = module;
		if (!m)
			return;
		int id = paramId;
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Grain window"));
		for (int s = 0; s < wa::NUM_GRAIN_WINDOWS; s++) {
			menu->addChild(createCheckMenuItem(wa::GRAIN_WINDOW_NAMES[s], "",
				[=]() {
					return (int) std::round(m->params[id].getValue()) == s;
				},
				[=]() {
					m->params[id].setValue((float) s);
				}
			));
		}
	}
};

template <class T = GrainWindowScreen>
T* createGrainWindowScreen(Vec mmCenter, Vec mmSize, Module* module, int paramId, NVGcolor color) {
	T* s = createParam<T>(Vec(0, 0), module, paramId);
	s->box.size = mm2px(mmSize);
	s->box.pos = mm2px(mmCenter).minus(s->box.size.div(2.f));
	s->color = color;
	return s;
}
