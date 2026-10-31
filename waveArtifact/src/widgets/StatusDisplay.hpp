#pragma once
#include "../WaveArtifact.hpp"

/** Shows the loaded file, table count, analysis progress or the last error. */
struct StatusDisplay : LedDisplay {
	WaveArtifact* module = nullptr;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
			if (font) {
				std::string text = module ? module->getStatus() : "Load a WAV (max 2 MB)";
				nvgSave(args.vg);
				nvgScissor(args.vg, 0, 0, box.size.x, box.size.y);
				nvgFontFaceId(args.vg, font->handle);
				nvgFontSize(args.vg, 11.f);
				nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
				nvgFillColor(args.vg, nvgRGB(0x4c, 0xc9, 0xf0));
				nvgText(args.vg, 6.f, box.size.y / 2.f, text.c_str(), NULL);
				nvgRestore(args.vg);
			}
		}
		LedDisplay::drawLayer(args, layer);
	}
};
