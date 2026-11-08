#pragma once
#include "../WaveArtifact.hpp"
#include "../Layout.hpp"

/** While a step is selected on the sequencer (see WaveArtifact::lockTarget), a thin bar under each of the selected
marker's controls shows the value that step plays for it: in the marker's color where the step locks it, faint grey
(the knob's own value) where it does not. Bipolar settings (range centred on 0: PAN, PITCH, FINE...) fill from the
middle, the others from the left; a switch or list shows the step's choice as text instead. Draws nothing otherwise. */
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
			float x = w->box.pos.x + 1.5f, width = w->box.size.x - 3.f;
			float y = w->box.pos.y + w->box.size.y + 1.2f, h = 2.4f;
			// A switch or list: as many segments as choices, the step's choice lit (a toggle: one segment, lit when on).
			// No text: the choice names are longer than the controls are wide.
			if (dynamic_cast<engine::SwitchQuantity*>(q)) {
				int n = (int) std::round(hi - lo) + 1;
				int idx = clamp((int) std::round(value - lo), 0, n - 1);
				int segments = (n == 2) ? 1 : n;
				float gap = 0.8f, sw = (width - gap * (segments - 1)) / segments;
				for (int s = 0; s < segments; s++) {
					bool lit = (n == 2) ? idx == 1 : s == idx;
					nvgBeginPath(vg);
					nvgRoundedRect(vg, x + s * (sw + gap), y, sw, h, 1.f);
					nvgFillColor(vg, lit ? fill : nvgRGBAf(0.f, 0.f, 0.f, 0.45f));
					nvgFill(vg);
				}
				continue;
			}
			// Track
			nvgBeginPath(vg);
			nvgRoundedRect(vg, x, y, width, h, 1.f);
			nvgFillColor(vg, nvgRGBAf(0.f, 0.f, 0.f, 0.45f));
			nvgFill(vg);
			float t = clamp((value - lo) / (hi - lo), 0.f, 1.f);
			bool bipolar = lo < 0.f && std::fabs(lo + hi) <= 1e-4f * (hi - lo);
			float a = bipolar ? 0.5f : 0.f;
			float x0 = x + width * std::min(a, t), x1 = x + width * std::max(a, t);
			nvgBeginPath(vg);
			nvgRect(vg, x0, y, std::max(x1 - x0, 1.f), h);
			nvgFillColor(vg, fill);
			nvgFill(vg);
			if (bipolar) {
				nvgBeginPath(vg);
				nvgRect(vg, x + width * 0.5f - 0.4f, y - 0.6f, 0.8f, h + 1.2f);
				nvgFillColor(vg, nvgRGBAf(1.f, 1.f, 1.f, 0.5f));
				nvgFill(vg);
			}
		}
	}
};
