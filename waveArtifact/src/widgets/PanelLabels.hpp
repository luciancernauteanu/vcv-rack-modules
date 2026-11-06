#pragma once
#include <atomic>

#include "../plugin.hpp"

/** Draws panel text at runtime.
Panel SVGs must not contain <text>, and converting text to paths needs Inkscape. Until the
labels are drawn into the SVG as paths, this widget renders them with a font from Rack's
install, following the light/dark panel setting.

A label can have a second text for the granular engine: when `modeParam` of `module` is on, the
alternative is drawn instead (the knobs keep their place but change meaning).
*/
struct PanelLabels : widget::Widget {
	struct Label {
		Vec pos;
		std::string text;
		float size;
		bool inverted;
		int align;
		/** Shown while the engine switch is on GRAIN; empty = same text */
		std::string grainText;
		float grainSize;
	};
	std::vector<Label> labels;
	/** Module and the param that switches the engine (>= 0.5 = granular) */
	Module* module = nullptr;
	int modeParam = -1;
	/** Or a flag set by the module (the expander learns the engine from the main module's message) */
	const std::atomic<bool>* grainFlag = nullptr;

	void add(Vec mmPos, const std::string& text, float size = 7.f, bool inverted = false, int align = NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE) {
		labels.push_back(Label{mmPos, text, size, inverted, align, "", size});
	}

	/** A label whose text changes with the engine (grainSize 0 = same size) */
	void addModal(Vec mmPos, const std::string& text, const std::string& grainText, float size = 7.f, float grainSize = 0.f) {
		labels.push_back(Label{mmPos, text, size, false, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, grainText, grainSize > 0.f ? grainSize : size});
	}

	bool grainMode() const {
		if (grainFlag)
			return grainFlag->load(std::memory_order_relaxed);
		return module && modeParam >= 0 && module->params[modeParam].getValue() > 0.5f;
	}

	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		bool dark = settings::preferDarkPanels;
		NVGcolor normal = dark ? nvgRGB(0xe6, 0xe6, 0xe6) : nvgRGB(0x26, 0x28, 0x2c);
		NVGcolor inverted = dark ? nvgRGB(0x26, 0x28, 0x2c) : nvgRGB(0xf2, 0xf2, 0xf2);
		bool grain = grainMode();

		nvgFontFaceId(args.vg, font->handle);
		for (const Label& l : labels) {
			Vec p = mm2px(l.pos);
			bool alt = grain && !l.grainText.empty();
			nvgFontSize(args.vg, alt ? l.grainSize : l.size);
			nvgTextAlign(args.vg, l.align);
			nvgFillColor(args.vg, l.inverted ? inverted : normal);
			nvgText(args.vg, p.x, p.y, (alt ? l.grainText : l.text).c_str(), NULL);
		}
	}
};
