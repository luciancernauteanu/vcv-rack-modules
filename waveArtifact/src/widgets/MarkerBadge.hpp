#pragma once
#include "../Layout.hpp"
#include "../WaveArtifact.hpp"

/** Colored circle with the number of the marker whose knobs are shown in the edit area. */
struct MarkerBadge : widget::Widget {
	WaveArtifact* module = nullptr;

	void draw(const DrawArgs& args) override {
		int m = module ? module->selectedMarker : 0;
		Vec c = box.size.div(2.f);
		float r = std::min(box.size.x, box.size.y) / 2.f;

		nvgBeginPath(args.vg);
		nvgCircle(args.vg, c.x, c.y, r);
		nvgFillColor(args.vg, layout::MARKER_COLORS[m]);
		nvgFill(args.vg);

		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, r * 1.3f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, nvgRGB(0x16, 0x18, 0x1c));
		std::string label = std::to_string(m + 1);
		nvgText(args.vg, c.x, c.y, label.c_str(), NULL);
	}
};
