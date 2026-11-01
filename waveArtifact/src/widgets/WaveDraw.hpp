#pragma once
#include "../WaveArtifact.hpp"

namespace wadraw {

/** Strokes one cycle of a (fractional) table index into `r`, using the full-band level. */
inline void drawTable(NVGcontext* vg, const wa::WavetableBank& bank, float index, math::Rect r, NVGcolor color, float width) {
	int maxT = bank.numTables - 1;
	index = clamp(index, 0.f, (float) maxT);
	int t0 = (int) index;
	int t1 = std::min(t0 + 1, maxT);
	float tf = index - t0;
	const float* f0 = bank.frame(t0, 0);
	const float* f1 = bank.frame(t1, 0);

	int points = clamp((int) r.size.x, 32, 256);
	float midY = r.pos.y + r.size.y / 2.f;
	// Artifacts are loudness-normalized, so peaks range from ~0.6 to 1.6: scale each drawing to fit
	// (this shows the shape; the level is the same for all).
	float vals[257];
	float peak = 1e-6f;
	for (int k = 0; k <= points; k++) {
		int i = std::min(k * wa::FRAME_SIZE / points, wa::FRAME_SIZE - 1);
		vals[k] = f0[i] + (f1[i] - f0[i]) * tf;
		peak = std::max(peak, std::fabs(vals[k]));
	}
	float amp = r.size.y * 0.45f / peak;
	nvgBeginPath(vg);
	for (int k = 0; k <= points; k++) {
		float v = vals[k];
		float x = r.pos.x + r.size.x * k / points;
		float y = midY - v * amp;
		if (k == 0)
			nvgMoveTo(vg, x, y);
		else
			nvgLineTo(vg, x, y);
	}
	nvgStrokeColor(vg, color);
	nvgStrokeWidth(vg, width);
	nvgLineJoin(vg, NVG_ROUND);
	nvgStroke(vg);
}

/** Stand-in shape for the module browser (no module, no bank). */
inline void drawSine(NVGcontext* vg, math::Rect r, float harmonic, NVGcolor color, float width) {
	int points = 64;
	float midY = r.pos.y + r.size.y / 2.f;
	float amp = r.size.y * 0.4f;
	nvgBeginPath(vg);
	for (int k = 0; k <= points; k++) {
		float ph = 2.f * M_PI * k / points;
		float v = std::sin(ph) + std::sin(harmonic * ph) / harmonic;
		float x = r.pos.x + r.size.x * k / points;
		float y = midY - v * amp * 0.7f;
		if (k == 0)
			nvgMoveTo(vg, x, y);
		else
			nvgLineTo(vg, x, y);
	}
	nvgStrokeColor(vg, color);
	nvgStrokeWidth(vg, width);
	nvgStroke(vg);
}

/** Draws a short number centered on (cx, cy) by its visible height. NVG_ALIGN_MIDDLE centers the
font's line box (ascender..descender), which leaves digits visibly off-center in a small circle;
digits sit on the baseline and are ~0.70 em tall in Nunito, so put the baseline 0.35 em below. */
inline void drawCenteredNumber(NVGcontext* vg, int font, float cx, float cy, float size, const std::string& text, NVGcolor color) {
	nvgFontFaceId(vg, font);
	nvgFontSize(vg, size);
	nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
	nvgFillColor(vg, color);
	// A single digit is centered by its ink, not its advance box: Nunito Bold's glyphs sit a little
	// off their advance center (measured from the font, em units; "1" is 0.048 em left, "5" 0.015
	// em right), which shows in a small circle.
	static const float INK_OFFSET_EM[10] = {-0.0002f, -0.0481f, -0.0098f, 0.0085f, 0.0085f, 0.0151f, 0.0024f, -0.0022f, 0.f, -0.0015f};
	float dx = 0.f;
	if (text.size() == 1 && text[0] >= '0' && text[0] <= '9')
		dx = -INK_OFFSET_EM[text[0] - '0'] * size;
	nvgText(vg, cx + dx, cy + 0.35f * size, text.c_str(), NULL);
}

inline std::shared_ptr<window::Font> monoFont() {
	return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
}

} // namespace wadraw
