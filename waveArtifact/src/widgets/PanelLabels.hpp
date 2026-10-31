#pragma once
#include "../plugin.hpp"

/** Draws panel text at runtime.
Panel SVGs must not contain <text>, and converting text to paths needs Inkscape. Until the
labels are drawn into the SVG as paths, this widget renders them with a font from Rack's
install, following the light/dark panel setting.
*/
struct PanelLabels : widget::Widget {
	struct Label {
		Vec pos;
		std::string text;
		float size;
		bool inverted;
		int align;
	};
	std::vector<Label> labels;

	void add(Vec mmPos, const std::string& text, float size = 7.f, bool inverted = false, int align = NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE) {
		labels.push_back(Label{mmPos, text, size, inverted, align});
	}

	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		bool dark = settings::preferDarkPanels;
		NVGcolor normal = dark ? nvgRGB(0xe6, 0xe6, 0xe6) : nvgRGB(0x26, 0x28, 0x2c);
		NVGcolor inverted = dark ? nvgRGB(0x26, 0x28, 0x2c) : nvgRGB(0xf2, 0xf2, 0xf2);

		nvgFontFaceId(args.vg, font->handle);
		for (const Label& l : labels) {
			Vec p = mm2px(l.pos);
			nvgFontSize(args.vg, l.size);
			nvgTextAlign(args.vg, l.align);
			nvgFillColor(args.vg, l.inverted ? inverted : normal);
			nvgText(args.vg, p.x, p.y, l.text.c_str(), NULL);
		}
	}
};
