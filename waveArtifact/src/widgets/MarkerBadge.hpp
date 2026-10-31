#pragma once
#include "../Layout.hpp"
#include "../WaveArtifact.hpp"
#include "WaveDraw.hpp"

/** The selected artifact: its whole section filled with the artifact's color, with a large
centered number. Covers the first top section (layout::TOP_SECTIONS[0]). */
struct MarkerBadge : widget::Widget {
	WaveArtifact* module = nullptr;

	void draw(const DrawArgs& args) override {
		int m = module ? module->selectedMarker : 0;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, mm2px(2.f));
		nvgFillColor(args.vg, layout::MARKER_COLORS[m]);
		nvgFill(args.vg);

		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		float size = box.size.x * 0.8f;
		wadraw::drawCenteredNumber(args.vg, font->handle, box.size.x / 2.f, box.size.y / 2.f, size, std::to_string(m + 1), nvgRGB(0x16, 0x18, 0x1c));
	}
};
