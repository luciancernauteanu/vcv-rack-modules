#pragma once
#include "../WaveArtifact.hpp"
#include "../Layout.hpp"

/** While a step is selected on the sequencer (see WaveArtifact::lockTarget), a thin bar under each of the selected
marker's controls shows the value that step plays for it: in the marker's color where the step locks it, faint grey
(the knob's own value) where it does not. Bipolar settings (range centred on 0: PAN, PITCH, FINE...) fill from the
middle, the others from the left; a switch or list shows the step's choice as lit segments. Controls of the strip
under the knobs (ADSR, KEEP) have no room under them: their bar stands on their right, filling upwards. Draws
nothing otherwise. */
struct StepValueBars : widget::TransparentWidget {
	WaveArtifact* module = nullptr;
	/** Every per-marker control (all markers): only the visible ones of the target marker draw a bar */
	std::vector<app::ParamWidget*> controls;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1 || !module)
			return;
		NVGcontext* vg = args.vg;
		NVGcolor col = layout::MARKER_COLORS[clamp(module->selectedMarker, 0, WaveArtifact::NUM_MARKERS - 1)];
		for (app::ParamWidget* w : controls) {
			if (!w->isVisible())
				continue;
			// A dot above the control: some step of this marker's row locks it (white: the selected step does).
			int lockedSteps = module->stepsLockingInRow(w->paramId);
			float value;
			bool locked;
			bool hasStep = module->stepValueFor(w->paramId, &value, &locked);
			if (lockedSteps > 0) {
				nvgBeginPath(vg);
				nvgCircle(vg, w->box.pos.x + w->box.size.x / 2.f, w->box.pos.y - 4.6f, 1.7f);
				nvgFillColor(vg, (hasStep && locked) ? nvgRGB(0xff, 0xff, 0xff) : col);
				nvgFill(vg);
			}
			if (!hasStep)
				continue;
			engine::ParamQuantity* q = w->getParamQuantity();
			if (!q)
				continue;
			float lo = q->getMinValue(), hi = q->getMaxValue();
			if (hi <= lo)
				continue;
			NVGcolor fill = locked ? col : nvgRGBAf(0.62f, 0.64f, 0.68f, 0.55f);
			// Under the control, along its width. In the strip under the knobs (ADSR, KEEP) there is no room
			// under it before the screen: there the bar stands on its right, along its height, filling upwards.
			const float thick = 2.4f;
			float cy = w->box.getCenter().y;
			bool vertical = cy > mm2px(layout::STRIP_Y0) && cy < mm2px(layout::STRIP_Y0 + layout::STRIP_H);
			// The track: `along` is its length, from `start` (left, or bottom when vertical)
			float along, start, across;
			if (vertical) {
				along = w->box.size.y - 3.f;
				start = w->box.pos.y + w->box.size.y - 1.5f;
				across = w->box.pos.x + w->box.size.x + 1.2f;
			}
			else {
				along = w->box.size.x - 3.f;
				start = w->box.pos.x + 1.5f;
				across = w->box.pos.y + w->box.size.y + 1.2f;
			}
			// A piece of the track from fraction a to b (0 = its start)
			auto piece = [&](float a, float b, float grow, float radius) {
				nvgBeginPath(vg);
				if (vertical)
					nvgRoundedRect(vg, across - grow, start - along * b, thick + 2.f * grow, along * (b - a), radius);
				else
					nvgRoundedRect(vg, start + along * a, across - grow, along * (b - a), thick + 2.f * grow, radius);
			};
			// A switch or list: as many segments as choices, the step's choice lit (a toggle: one segment, lit when on).
			// No text: the choice names are longer than the controls are wide.
			if (dynamic_cast<engine::SwitchQuantity*>(q)) {
				int n = (int) std::round(hi - lo) + 1;
				int idx = clamp((int) std::round(value - lo), 0, n - 1);
				int segments = (n == 2) ? 1 : n;
				float gap = 0.8f / along, sw = (1.f - gap * (segments - 1)) / segments;
				for (int s = 0; s < segments; s++) {
					bool lit = (n == 2) ? idx == 1 : s == idx;
					piece(s * (sw + gap), s * (sw + gap) + sw, 0.f, 1.f);
					nvgFillColor(vg, lit ? fill : nvgRGBAf(0.f, 0.f, 0.f, 0.45f));
					nvgFill(vg);
				}
				continue;
			}
			// Track
			piece(0.f, 1.f, 0.f, 1.f);
			nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.45f));
			nvgFill(vg);
			float t = clamp((value - lo) / (hi - lo), 0.f, 1.f);
			bool bipolar = lo < 0.f && std::fabs(lo + hi) <= 1e-4f * (hi - lo);
			float a = bipolar ? 0.5f : 0.f;
			float minLen = 1.f / along;
			piece(std::min(a, t), std::max(std::max(a, t), std::min(a, t) + minLen), 0.f, 0.f);
			nvgFillColor(vg, fill);
			nvgFill(vg);
			if (bipolar) {
				float half = 0.4f / along;
				piece(0.5f - half, 0.5f + half, 0.6f, 0.f);
				nvgFillColor(vg, nvgRGBAf(1.f, 1.f, 1.f, 0.5f));
				nvgFill(vg);
			}
		}
	}
};
