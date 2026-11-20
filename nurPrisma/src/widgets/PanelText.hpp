#pragma once
#include <functional>

#include "../plugin.hpp"

namespace np {

/** Panel text drawn at runtime with the plugin's own fonts (Cinzel title, Jost labels).
Panel SVGs carry shapes only: text would need converting to paths. Positions and sizes in mm. */
struct PanelText : widget::TransparentWidget {
	enum Face { TITLE, LABEL, CAPTION, NUM_FACES };
	struct Item {
		Vec pos;
		std::string text;
		float size;
		NVGcolor color;
		Face face;
		/** Extra space between letters, as a fraction of the size */
		float spacing;
		/** While a core is in focus: this text instead (empty = same), tinted with the core's colour if `tint` */
		std::string focusText;
		bool tint;
		/** Shown only while *showIf is true (null = always) */
		const bool* showIf;
		/** Text worked out at draw time (empty = use `text`) */
		std::function<std::string()> dynamic;
	};
	std::vector<Item> items;
	/** The module's focus (-1 = ALL), or null (browser) */
	const int* focus = nullptr;
	/** Colour of core i, for tinted labels */
	NVGcolor (*coreColor)(int) = nullptr;

	static NVGcolor cream() { return nvgRGB(0xe9, 0xe3, 0xd5); }
	static NVGcolor grey() { return nvgRGB(0x8d, 0x8b, 0x86); }
	static NVGcolor gold() { return nvgRGB(0xd6, 0xa6, 0x4f); }

	void add(Vec mmPos, const std::string& text, float mmSize, NVGcolor color, Face face = LABEL, float spacing = 0.1f) {
		items.push_back({mmPos, text, mmSize, color, face, spacing, "", false, nullptr, nullptr});
	}

	/** A label whose text is worked out when drawn (e.g. DRIVE's voicing and position). */
	void addDynamic(Vec mmPos, std::function<std::string()> fn, float mmSize, NVGcolor color, Face face = LABEL,
			float spacing = 0.1f) {
		items.push_back({mmPos, "", mmSize, color, face, spacing, "", false, nullptr, fn});
	}

	/** A label shown only while `*flag` is true (e.g. SCALE while CUTOFF is quantized). */
	void addIf(Vec mmPos, const std::string& text, float mmSize, NVGcolor color, const bool* flag, Face face = CAPTION,
			float spacing = 0.2f) {
		items.push_back({mmPos, text, mmSize, color, face, spacing, "", false, flag, nullptr});
	}

	/** A label that changes while a core is in focus (e.g. MIX -> LEVEL), tinted with the core's colour. */
	void addFocus(Vec mmPos, const std::string& text, const std::string& focusText, float mmSize, NVGcolor color,
			Face face = LABEL, float spacing = 0.1f) {
		items.push_back({mmPos, text, mmSize, color, face, spacing, focusText, true, nullptr, nullptr});
	}

	void draw(const DrawArgs& args) override {
		static const char* files[NUM_FACES] = {"res/fonts/Cinzel-Regular.ttf", "res/fonts/Jost-SemiBold.ttf",
			"res/fonts/Jost-Medium.ttf"};
		std::shared_ptr<window::Font> fonts[NUM_FACES];
		for (int f = 0; f < NUM_FACES; f++)
			fonts[f] = APP->window->loadFont(asset::plugin(pluginInstance, files[f]));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		int f = focus ? *focus : -1;
		for (const Item& it : items) {
			if (it.showIf && !*it.showIf)
				continue;
			const std::shared_ptr<window::Font>& font = fonts[it.face];
			bool focused = f >= 0 && it.tint;
			std::string dyn = it.dynamic ? it.dynamic() : std::string();
			const std::string& str = it.dynamic ? dyn : (focused && !it.focusText.empty()) ? it.focusText : it.text;
			NVGcolor col = (focused && coreColor) ? coreColor(f) : it.color;
			if (!font)
				continue;
			float px = mm2px(it.size);
			float spacing = it.spacing * px;
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, px);
			nvgTextLetterSpacing(args.vg, spacing);
			nvgFillColor(args.vg, col);
			Vec p = mm2px(it.pos);
			// Letter spacing also follows the last letter: shift by half of it to stay centred.
			nvgText(args.vg, p.x + 0.5f * spacing, p.y, str.c_str(), nullptr);
		}
		nvgTextLetterSpacing(args.vg, 0.f);
	}
};

} // namespace np
