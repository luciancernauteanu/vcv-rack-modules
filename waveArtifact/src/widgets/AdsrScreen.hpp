#pragma once
#include "../Layout.hpp"
#include "../WaveArtifact.hpp"

/** HOLD: the note's length, or the top of the knob: until another marker starts. */
struct AdsrHoldQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float s = wa::adsrHoldSeconds(getValue());
		if (s < 0.f)
			return "until the next marker";
		return s < 1.f ? string::f("%.0f ms", s * 1000.f) : string::f("%.2f s", s);
	}
	std::string getUnit() override {
		return "";
	}
};

/** Shape of the selected marker's envelope (shown only in POLY). Knob travel sets each stage's
width, since the real times span 1 ms to 10 s. Takes no clicks. */
struct AdsrScreen : widget::TransparentWidget {
	WaveArtifact* module = nullptr;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			drawCurve(args.vg);
		Widget::drawLayer(args, layer);
	}

	void drawCurve(NVGcontext* vg) {
		float W = box.size.x, H = box.size.y;
		int m = module ? module->selectedMarker : 0;
		NVGcolor c = layout::MARKER_COLORS[m];
		float a = 0.2f, d = 0.5f, s = 1.f, r = 0.5f, hold = 0.67f;
		if (module) {
			a = module->params[WaveArtifact::MARKER_ATTACK_PARAM + m].getValue();
			d = module->params[WaveArtifact::MARKER_DECAY_PARAM + m].getValue();
			s = module->params[WaveArtifact::MARKER_SUSTAIN_PARAM + m].getValue();
			r = module->params[WaveArtifact::MARKER_RELEASE_PARAM + m].getValue();
			hold = module->params[WaveArtifact::MARKER_HOLD_PARAM + m].getValue();
		}
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, W, H, 2.f);
		nvgFillColor(vg, nvgRGB(0x0b, 0x0d, 0x14));
		nvgFill(vg);

		float padX = 2.f, padY = 2.f;
		float w = W - 2.f * padX, h = H - 2.f * padY;
		float total = a + d + hold + r + 1e-3f;
		float x1 = padX + w * a / total;
		float x2 = padX + w * (a + d) / total;
		float x3 = padX + w * (a + d + hold) / total;
		float x4 = padX + w;
		auto y = [&](float level) {
			return padY + h * (1.f - level);
		};
		const int N = 12;
		auto curve = [&](float x0, float xEnd, float from, float to) {
			for (int k = 1; k <= N; k++) {
				float t = (float) k / N;
				// The same shape as the engine's fall, drawn to -60 dB.
				float fall = (std::pow(0.001f, t) - 0.001f) / 0.999f;
				nvgLineTo(vg, x0 + (xEnd - x0) * t, y(to + (from - to) * fall));
			}
		};
		nvgBeginPath(vg);
		nvgMoveTo(vg, padX, y(0.f));
		nvgLineTo(vg, x1, y(1.f));
		curve(x1, x2, 1.f, s);
		nvgLineTo(vg, x3, y(s));
		curve(x3, x4, s, 0.f);
		nvgLineJoin(vg, NVG_ROUND);
		nvgStrokeColor(vg, c);
		nvgStrokeWidth(vg, 1.1f);
		nvgStroke(vg);
	}
};
