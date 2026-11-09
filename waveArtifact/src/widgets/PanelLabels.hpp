#pragma once
#include <atomic>

#include "../plugin.hpp"

/** Draws panel text at runtime.
Panel SVGs must not contain <text>, and converting text to paths needs Inkscape. Until the
labels are drawn into the SVG as paths, this widget renders them with a font from Rack's
install, following the light/dark panel setting.

A label can have other texts for the granular and the sample engines: while `modeParam` of `module` is on 1 (GRAIN)
or 2 (SAMPLE), the alternative is drawn instead (the knobs keep their place but change meaning).
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
		/** Shown on SAMPLE; `sameAsGrain` = the GRAIN text */
		std::string sampleText;
		bool sameAsGrain;
	};
	std::vector<Label> labels;
	/** Module and the param that switches the engine (0 wavetable, 1 granular, 2 sample) */
	Module* module = nullptr;
	int modeParam = -1;
	/** Or a flag set by the module (the expander learns the engine from the main module's message): granular or
	sample, both shown with the GRAIN texts */
	const std::atomic<bool>* grainFlag = nullptr;

	void add(Vec mmPos, const std::string& text, float size = 7.f, bool inverted = false, int align = NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE) {
		labels.push_back(Label{mmPos, text, size, inverted, align, "", size, "", true});
	}

	/** A label whose text changes with the engine (grainSize 0 = same size). SAMPLE shows the GRAIN text. */
	void addModal(Vec mmPos, const std::string& text, const std::string& grainText, float size = 7.f, float grainSize = 0.f) {
		labels.push_back(Label{mmPos, text, size, false, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, grainText, grainSize > 0.f ? grainSize : size, "", true});
	}

	/** Its own text on SAMPLE too (" " = none) */
	void addModal3(Vec mmPos, const std::string& text, const std::string& grainText, const std::string& sampleText, float size = 7.f, float grainSize = 0.f) {
		labels.push_back(Label{mmPos, text, size, false, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, grainText, grainSize > 0.f ? grainSize : size, sampleText, false});
	}

	/** 0 wavetable, 1 granular, 2 sample */
	int engineMode() const {
		if (grainFlag)
			return grainFlag->load(std::memory_order_relaxed) ? 1 : 0;
		if (!module || modeParam < 0)
			return 0;
		return clamp((int) std::round(module->params[modeParam].getValue()), 0, 2);
	}

	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		if (!font)
			return;
		bool dark = settings::preferDarkPanels;
		NVGcolor normal = dark ? nvgRGB(0xe6, 0xe6, 0xe6) : nvgRGB(0x26, 0x28, 0x2c);
		NVGcolor inverted = dark ? nvgRGB(0x26, 0x28, 0x2c) : nvgRGB(0xf2, 0xf2, 0xf2);
		int engine = engineMode();

		nvgFontFaceId(args.vg, font->handle);
		for (const Label& l : labels) {
			Vec p = mm2px(l.pos);
			const std::string* text = &l.text;
			float size = l.size;
			const std::string& grain = (engine == 2 && !l.sameAsGrain) ? l.sampleText : l.grainText;
			if (engine >= 1 && !grain.empty()) {
				text = &grain;
				size = l.grainSize;
			}
			nvgFontSize(args.vg, size);
			nvgTextAlign(args.vg, l.align);
			nvgFillColor(args.vg, l.inverted ? inverted : normal);
			nvgText(args.vg, p.x, p.y, text->c_str(), NULL);
		}
	}
};
