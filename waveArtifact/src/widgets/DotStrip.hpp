#pragma once
#include "../WaveArtifact.hpp"

/** One dot per extracted wavetable, in two rows.
Color follows the table's brightness (spectral centroid). Hollow dots are tables where no
fundamental was found. Hover, click-to-assign and marker rings arrive in M5.
*/
struct DotStrip : widget::Widget {
	WaveArtifact* module = nullptr;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			drawDots(args);
		Widget::drawLayer(args, layer);
	}

	void drawDots(const DrawArgs& args) {
		// Safe to read: only the UI thread (this thread) frees banks, in ModuleWidget::step().
		const wa::WavetableBank* bank = module ? module->activeBank.load() : nullptr;
		// In the module browser there is no module, so show a sample strip.
		int n = bank ? bank->numTables : (module ? 0 : 64);
		if (n == 0)
			return;

		int perRow = (n + 1) / 2;
		float dx = box.size.x / perRow;
		float r = std::min(dx * 0.32f, box.size.y * 0.16f);
		NVGcolor dim = nvgRGB(0x1f, 0x6f, 0x8b);
		NVGcolor bright = nvgRGB(0xd8, 0xf6, 0xff);

		for (int t = 0; t < n; t++) {
			int row = t / perRow;
			int col = t % perRow;
			float x = (col + 0.5f) * dx;
			float y = box.size.y * (row == 0 ? 0.27f : 0.73f);
			float b = bank ? bank->brightness[t] : (float) t / n;
			NVGcolor c = nvgLerpRGBA(dim, bright, b);

			nvgBeginPath(args.vg);
			nvgCircle(args.vg, x, y, r);
			if (!bank || bank->pitched[t]) {
				nvgFillColor(args.vg, c);
				nvgFill(args.vg);
			}
			else {
				nvgStrokeColor(args.vg, c);
				nvgStrokeWidth(args.vg, 1.f);
				nvgStroke(args.vg);
			}
		}
	}
};
