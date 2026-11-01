#pragma once
#include "../Layout.hpp"
#include "../plugin.hpp"

/** Small display button showing a switch param's current label (e.g. "LP"). Left-click steps to
the next value (wrapping), undoable like any param change. Right-click gives the usual param menu,
where the type can also be picked directly. */
struct ModeButton : app::ParamWidget {
	/** Short text per value, shown on the button */
	std::vector<std::string> labels;
	/** Accent for the text */
	NVGcolor color = layout::SCREEN_TEXT;
	/** Optional accent per value (overrides `color`) */
	std::vector<NVGcolor> colors;

	void onButton(const ButtonEvent& e) override {
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && (e.mods & RACK_MOD_MASK) == 0) {
			engine::ParamQuantity* pq = getParamQuantity();
			if (pq) {
				float oldValue = pq->getValue();
				int n = (int) std::round(pq->getMaxValue() - pq->getMinValue()) + 1;
				int v = (int) std::round(oldValue - pq->getMinValue());
				float newValue = pq->getMinValue() + (float) ((v + 1) % n);
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
		engine::ParamQuantity* pq = getParamQuantity();
		int v = pq ? (int) std::round(pq->getValue() - pq->getMinValue()) : 0;
		std::string text = (v >= 0 && v < (int) labels.size()) ? labels[v] : "";
		NVGcolor color = (v >= 0 && v < (int) colors.size()) ? colors[v] : this->color;

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
		nvgFillColor(args.vg, nvgRGB(0x12, 0x14, 0x1a));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgTransRGBAf(color, 0.5f));
		nvgStrokeWidth(args.vg, 0.8f);
		nvgStroke(args.vg);

		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		float fontSize = box.size.y * 0.7f;
		nvgFontSize(args.vg, fontSize);
		// Long labels (NOTCH) shrink so they keep a margin inside the button; short ones (LP) stay as they are.
		float bounds[4];
		nvgTextBounds(args.vg, 0.f, 0.f, text.c_str(), NULL, bounds);
		float width = bounds[2] - bounds[0];
		float maxWidth = box.size.x * 0.76f;
		if (width > maxWidth && width > 0.f)
			nvgFontSize(args.vg, fontSize * maxWidth / width);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, (text == "OFF") ? layout::SCREEN_DIM : color);
		nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f + 0.5f, text.c_str(), NULL);
	}
};
