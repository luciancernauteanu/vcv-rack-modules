#pragma once
#include "../Layout.hpp"
#include "../Lfo.hpp"
#include "LfoControls.hpp"
#include "ModeButton.hpp"

/** Mini screen that draws the LFO's current shape. Left-click steps to the next shape (undoable),
right-click lists all six. Sits on a switch param (0..5, see wa::LfoShape). */
struct LfoShapeScreen : ModeButton {
	LfoShapeScreen() {
		color = layout::SCREEN_TEXT;
	}

	/** One representative cycle of a shape. The random ones are fixed example curves. */
	static float sample(int shape, float x) {
		static const float smoothPts[] = {-0.1f, 0.85f, -0.7f, 0.55f, -0.1f};
		static const float drunkPts[] = {0.f, 0.3f, 0.12f, 0.45f, 0.3f, 0.58f, 0.4f, 0.62f, 0.5f};
		const float* pts = nullptr;
		int n = 0;
		if (shape == wa::LFO_SMOOTH_RANDOM) {
			pts = smoothPts;
			n = 5;
		}
		else if (shape == wa::LFO_DRUNK) {
			pts = drunkPts;
			n = 9;
		}
		if (!pts)
			return wa::lfoShapeValue(shape, x);
		float p = x * (n - 1);
		int i = std::min((int) p, n - 2);
		float t = p - i;
		t = t * t * (3.f - 2.f * t);
		return pts[i] + (pts[i + 1] - pts[i]) * t;
	}

	void draw(const DrawArgs& args) override {
		engine::ParamQuantity* pq = getParamQuantity();
		int shape = pq ? clamp((int) std::round(pq->getValue()), 0, wa::NUM_LFO_SHAPES - 1) : 0;

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
		nvgFillColor(args.vg, nvgRGB(0x0b, 0x0d, 0x14));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgTransRGBAf(color, 0.45f));
		nvgStrokeWidth(args.vg, 0.7f);
		nvgStroke(args.vg);

		float padX = box.size.x * 0.1f, padY = box.size.y * 0.18f;
		float w = box.size.x - 2.f * padX, h = box.size.y - 2.f * padY;
		// Zero line
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, padX, padY + h / 2.f);
		nvgLineTo(args.vg, padX + w, padY + h / 2.f);
		nvgStrokeColor(args.vg, nvgTransRGBAf(color, 0.15f));
		nvgStrokeWidth(args.vg, 0.5f);
		nvgStroke(args.vg);

		nvgBeginPath(args.vg);
		const int N = 64;
		bool jumpShape = (shape == wa::LFO_SAW || shape == wa::LFO_SAW_REVERSE || shape == wa::LFO_SQUARE);
		for (int k = 0; k <= N; k++) {
			float x = (float) k / N;
			// Draw the edges of saw/square as vertical lines: sample just inside each end.
			float sx = x;
			if (jumpShape)
				sx = std::min(std::max(x, 0.0005f), 0.9995f);
			float v = sample(shape, sx);
			float px = padX + w * x;
			float py = padY + h * (0.5f - 0.5f * v);
			if (k == 0)
				nvgMoveTo(args.vg, px, py);
			else
				nvgLineTo(args.vg, px, py);
		}
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
		menu->addChild(createMenuLabel("LFO shape"));
		for (int s = 0; s < wa::NUM_LFO_SHAPES; s++) {
			menu->addChild(createCheckMenuItem(wa::LFO_SHAPE_NAMES[s], "",
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

/** Small lit button for an on/off param (e.g. SYNC, T, D, FREE). The label sits inside; the button
glows in `color` while on. Same param as the entry in the RATE knob's right-click menu. */
struct LitToggle : app::ParamWidget {
	std::string label;
	NVGcolor color = layout::SCREEN_TEXT;

	bool isOn() {
		engine::ParamQuantity* pq = getParamQuantity();
		return pq && pq->getValue() > 0.5f;
	}

	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && (e.mods & RACK_MOD_MASK) == 0) {
			engine::ParamQuantity* pq = getParamQuantity();
			if (pq) {
				float oldValue = pq->getValue();
				float newValue = oldValue > 0.5f ? 0.f : 1.f;
				pq->setValue(newValue);
				history::ParamChange* h = new history::ParamChange;
				h->name = "change " + pq->getLabel();
				h->moduleId = module->id;
				h->paramId = paramId;
				h->oldValue = oldValue;
				h->newValue = newValue;
				APP->history->push(h);
			}
			e.consume(this);
			return;
		}
		ParamWidget::onButton(e);
	}

	void draw(const DrawArgs& args) override {
		bool on = isOn();
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 1.6f);
		nvgFillColor(args.vg, on ? nvgTransRGBAf(color, 0.85f) : nvgRGB(0x14, 0x16, 0x1d));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgTransRGBAf(color, on ? 1.f : 0.4f));
		nvgStrokeWidth(args.vg, 0.7f);
		nvgStroke(args.vg);

		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, box.size.y * 0.62f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, on ? nvgRGB(0x10, 0x12, 0x1a) : nvgTransRGBAf(color, 0.75f));
		nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f + 0.4f, label.c_str(), NULL);
	}

	/** Lit buttons glow: drawn again on the light layer, so they stay bright in a dark room. */
	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1 && isOn()) {
			NVGpaint glow = nvgBoxGradient(args.vg, 0, 0, box.size.x, box.size.y, 2.f, 4.f, nvgTransRGBAf(color, 0.55f), nvgTransRGBAf(color, 0.f));
			nvgBeginPath(args.vg);
			nvgRect(args.vg, -4.f, -4.f, box.size.x + 8.f, box.size.y + 8.f);
			nvgFillPaint(args.vg, glow);
			nvgFill(args.vg);
		}
		ParamWidget::drawLayer(args, layer);
	}
};

inline LfoShapeScreen* createLfoShapeScreen(Vec mmCenter, Vec mmSize, Module* module, int paramId, NVGcolor color) {
	LfoShapeScreen* s = createParam<LfoShapeScreen>(Vec(0, 0), module, paramId);
	s->box.size = mm2px(mmSize);
	s->box.pos = mm2px(mmCenter).minus(s->box.size.div(2.f));
	s->color = color;
	return s;
}

inline LitToggle* createLitToggle(Vec mmCenter, Module* module, int paramId, const std::string& label, NVGcolor color, Vec mmSize = layout::LFO_TOGGLE_SIZE) {
	LitToggle* t = createParam<LitToggle>(Vec(0, 0), module, paramId);
	t->box.size = mm2px(mmSize);
	t->box.pos = mm2px(mmCenter).minus(t->box.size.div(2.f));
	t->label = label;
	t->color = color;
	return t;
}
