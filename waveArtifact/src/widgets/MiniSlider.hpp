#pragma once
#include "../plugin.hpp"
#include "../Layout.hpp"

/** Small horizontal slider: click or drag sets the value where the mouse is, double-click resets, right-click is the
usual param menu. The fill and the percentage are drawn inside. Used for the sequencer's RND / COND amounts and the
SAMPLE engine's HITS. One undo entry per drag. */
struct MiniSlider : app::ParamWidget {
	NVGcolor color = layout::SCREEN_TEXT;
	float dragX = 0.f, oldValue = 0.f;

	void setFromX(float x) {
		engine::ParamQuantity* pq = getParamQuantity();
		if (pq && box.size.x > 0.f)
			pq->setScaledValue(clamp(x / box.size.x, 0.f, 1.f));
	}
	void onButton(const ButtonEvent& e) override {
		engine::ParamQuantity* pq = getParamQuantity();
		if (pq && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT && (e.mods & RACK_MOD_MASK) == 0) {
			oldValue = pq->getValue();
			dragX = e.pos.x;
			setFromX(dragX);
			e.consume(this);
			return;
		}
		ParamWidget::onButton(e);
	}
	void onDragMove(const DragMoveEvent& e) override {
		if (e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		dragX += e.mouseDelta.x / getAbsoluteZoom();
		setFromX(dragX);
	}
	void onDragEnd(const DragEndEvent& e) override {
		engine::ParamQuantity* pq = getParamQuantity();
		if (pq && e.button == GLFW_MOUSE_BUTTON_LEFT && pq->getValue() != oldValue) {
			history::ParamChange* h = new history::ParamChange;
			h->name = "change " + pq->getLabel();
			h->moduleId = module->id;
			h->paramId = paramId;
			h->oldValue = oldValue;
			h->newValue = pq->getValue();
			APP->history->push(h);
		}
		ParamWidget::onDragEnd(e);
	}
	void draw(const DrawArgs& args) override {
		engine::ParamQuantity* pq = getParamQuantity();
		float f = pq ? clamp(pq->getScaledValue(), 0.f, 1.f) : 0.5f;
		NVGcontext* vg = args.vg;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 1.6f);
		nvgFillColor(vg, nvgRGB(0x14, 0x16, 0x1d));
		nvgFill(vg);
		if (f > 0.f) {
			nvgBeginPath(vg);
			nvgRoundedRect(vg, 0, 0, box.size.x * f, box.size.y, 1.6f);
			nvgFillColor(vg, nvgTransRGBAf(color, 0.55f));
			nvgFill(vg);
		}
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, box.size.x, box.size.y, 1.6f);
		nvgStrokeColor(vg, nvgTransRGBAf(color, 0.6f));
		nvgStrokeWidth(vg, 0.7f);
		nvgStroke(vg);
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		nvgFontFaceId(vg, font->handle);
		nvgFontSize(vg, box.size.y * 0.72f);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(vg, nvgRGB(0xf0, 0xf2, 0xff));
		nvgText(vg, box.size.x / 2.f, box.size.y / 2.f + 0.4f, string::f("%d%%", (int) std::round(f * 100.f)).c_str(), NULL);
	}
};
