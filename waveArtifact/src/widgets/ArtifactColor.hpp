#pragma once
#include "../Layout.hpp"
#include "../WaveArtifact.hpp"
#include "AboveTooltip.hpp"

/** Value ring around a per-artifact knob, in the artifact's color: a faint track over the knob's
travel and a bright arc for the current value (from the left end, or from the middle for bipolar
knobs like PITCH and PAN). Added as a child of the knob, so it shows and hides with it. Drawn on
the light layer, so it stays readable in a dark room. */
struct ValueArc : widget::Widget {
	app::Knob* knob = nullptr;
	NVGcolor color = layout::SCREEN_TEXT;
	bool bipolar = false;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1 && knob) {
			engine::ParamQuantity* pq = knob->getParamQuantity();
			if (pq) {
				float a0 = knob->minAngle, a1 = knob->maxAngle;
				float v = clamp(pq->getScaledValue(), 0.f, 1.f);
				// The knob shows a sequencer step's value while it is being locked: the arc follows it in white, and
				// a dot in the marker's color marks the knob's own value.
				StepShownValue* shown = dynamic_cast<StepShownValue*>(knob);
				bool step = shown && shown->stepShowing && pq->getRange() > 0.f;
				NVGcolor arcColor = color;
				if (step) {
					float own = v;
					v = clamp((shown->stepShownValue - pq->getMinValue()) / pq->getRange(), 0.f, 1.f);
					arcColor = nvgRGB(0xff, 0xff, 0xff);
					float ownAt = a0 + (a1 - a0) * own - 0.5f * (float) M_PI;
					Vec c = box.size.div(2.f);
					float r = 0.5f * std::min(box.size.x, box.size.y) + 1.6f;
					nvgBeginPath(args.vg);
					nvgCircle(args.vg, c.x + r * std::cos(ownAt), c.y + r * std::sin(ownAt), 1.8f);
					nvgFillColor(args.vg, color);
					nvgFill(args.vg);
				}
				float at = a0 + (a1 - a0) * v;
				float from = bipolar ? 0.5f * (a0 + a1) : a0;
				Vec c = box.size.div(2.f);
				float r = 0.5f * std::min(box.size.x, box.size.y) + 1.6f;
				// Knob angles are measured from 12 o'clock, nanovg's from 3 o'clock.
				const float top = -0.5f * (float) M_PI;
				nvgBeginPath(args.vg);
				nvgArc(args.vg, c.x, c.y, r, a0 + top, a1 + top, NVG_CW);
				nvgStrokeColor(args.vg, nvgTransRGBAf(color, 0.18f));
				nvgStrokeWidth(args.vg, 1.2f);
				nvgStroke(args.vg);
				if (std::fabs(at - from) > 1e-3f) {
					nvgBeginPath(args.vg);
					nvgArc(args.vg, c.x, c.y, r, std::min(at, from) + top, std::max(at, from) + top, NVG_CW);
					nvgStrokeColor(args.vg, arcColor);
					nvgStrokeWidth(args.vg, 1.6f);
					nvgLineCap(args.vg, NVG_ROUND);
					nvgStroke(args.vg);
				}
			}
		}
		Widget::drawLayer(args, layer);
	}
};

/** Adds a ValueArc in `color` to a knob and returns the knob (for chaining in widget setup). */
template <class TKnob>
TKnob* withArc(TKnob* knob, NVGcolor color, bool bipolar = false) {
	ValueArc* arc = new ValueArc;
	arc->box.size = knob->box.size;
	arc->knob = knob;
	arc->color = color;
	arc->bipolar = bipolar;
	knob->addChild(arc);
	return knob;
}

/** A soft wash of the selected artifact's color under everything that follows the selection: the per-artifact
sections of the top band, the envelope strip (POLY) and KEEP (GRAIN), and a halo behind the ENGINE button. Global
controls (tempo, PLAY, POSITION, MASTER, outputs) stay plain, so the artifact's zone reads at a glance. Drawn on the
panel layer before the labels and the controls, so it sits under both. Strength 15 % (light) / 12 % (dark),
chosen from a rendered test of 10 / 16 / 22 %. */
struct ArtifactTint : widget::Widget {
	WaveArtifact* module = nullptr;

	void draw(const DrawArgs& args) override {
		int m = module ? clamp(module->selectedMarker, 0, WaveArtifact::NUM_MARKERS - 1) : 1;
		NVGcolor c = nvgTransRGBAf(layout::MARKER_COLORS[m], settings::preferDarkPanels ? 0.12f : 0.15f);
		auto wash = [&](Vec mmPos, Vec mmSize) {
			Vec p = mm2px(mmPos), size = mm2px(mmSize);
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, p.x, p.y, size.x, size.y, mm2px(2.f));
			nvgFillColor(args.vg, c);
			nvgFill(args.vg);
		};
		// The top band's per-artifact sections (not the badge, which is solid, nor the last: tempo)
		const int count = (int) (sizeof(layout::TOP_SECTIONS) / sizeof(layout::TOP_SECTIONS[0])) - 1;
		for (int i = 1; i < count; i++) {
			const layout::Section& s = layout::TOP_SECTIONS[i];
			wash(Vec(s.x0, layout::TOP_Y0), Vec(s.x1 - s.x0, layout::TOP_Y1 - layout::TOP_Y0));
		}
		if (!module || module->params[WaveArtifact::PLAY_MODE_PARAM].getValue() > 0.5f)
			wash(layout::ADSR_BOX_POS, layout::ADSR_BOX_SIZE);
		if (module && (int) std::round(module->params[WaveArtifact::ENGINE_MODE_PARAM].getValue()) == 1)
			wash(layout::SPREAD_BOX_POS, layout::SPREAD_BOX_SIZE);
		// The ENGINE button is the selected artifact's too: a halo in its color behind it
		{
			Vec p = mm2px(layout::ENGINE_BUTTON.minus(layout::ENGINE_BUTTON_SIZE.div(2.f)).minus(Vec(0.8f, 0.8f)));
			Vec size = mm2px(layout::ENGINE_BUTTON_SIZE.plus(Vec(1.6f, 1.6f)));
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, p.x, p.y, size.x, size.y, mm2px(1.6f));
			nvgFillColor(args.vg, nvgTransRGBAf(layout::MARKER_COLORS[m], 0.3f));
			nvgFill(args.vg);
			nvgStrokeColor(args.vg, nvgTransRGBAf(layout::MARKER_COLORS[m], 0.7f));
			nvgStrokeWidth(args.vg, 1.f);
			nvgStroke(args.vg);
		}
		Widget::draw(args);
	}
};

/** Outlines the per-artifact sections of the top band in the selected artifact's color, so it is
clear which controls follow the selection. Global sections (tempo, clock, reset) stay uncolored. */
struct ArtifactSections : widget::Widget {
	WaveArtifact* module = nullptr;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			int m = module ? module->selectedMarker : 0;
			NVGcolor c = layout::MARKER_COLORS[m];
			// Every top section except the last one (BPM / CLOCK / RESET).
			const int count = (int) (sizeof(layout::TOP_SECTIONS) / sizeof(layout::TOP_SECTIONS[0])) - 1;
			for (int i = 0; i < count; i++) {
				const layout::Section& s = layout::TOP_SECTIONS[i];
				Vec p = mm2px(Vec(s.x0, layout::TOP_Y0));
				Vec q = mm2px(Vec(s.x1, layout::TOP_Y1));
				nvgBeginPath(args.vg);
				nvgRoundedRect(args.vg, p.x + 0.5f, p.y + 0.5f, q.x - p.x - 1.f, q.y - p.y - 1.f, mm2px(2.f));
				nvgStrokeColor(args.vg, nvgTransRGBAf(c, 0.7f));
				nvgStrokeWidth(args.vg, 1.f);
				nvgStroke(args.vg);
			}
			// The strip under the knobs follows the selection too: the envelope only exists in POLY, KEEP in GRAIN.
			auto outline = [&](Vec mmPos, Vec mmSize) {
				Vec p = mm2px(mmPos);
				Vec size = mm2px(mmSize);
				nvgBeginPath(args.vg);
				nvgRoundedRect(args.vg, p.x + 0.5f, p.y + 0.5f, size.x - 1.f, size.y - 1.f, mm2px(2.f));
				nvgStrokeColor(args.vg, nvgTransRGBAf(c, 0.7f));
				nvgStrokeWidth(args.vg, 1.f);
				nvgStroke(args.vg);
			};
			if (module && module->params[WaveArtifact::PLAY_MODE_PARAM].getValue() > 0.5f)
				outline(layout::ADSR_BOX_POS, layout::ADSR_BOX_SIZE);
			if (module && (int) std::round(module->params[WaveArtifact::ENGINE_MODE_PARAM].getValue()) == 1)
				outline(layout::SPREAD_BOX_POS, layout::SPREAD_BOX_SIZE);
		}
		Widget::drawLayer(args, layer);
	}
};
