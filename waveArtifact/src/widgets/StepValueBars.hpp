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
	/** Whose controls these are: -1 the selected marker (the main panel), else that marker (an expander's own) */
	int marker = -1;
	/** Per control, when they belong to different markers (the SEND / RETURN module's rows): controlMarkers[i] is
	controls[i]'s marker. Empty: `marker` for all. */
	std::vector<int> controlMarkers;
	/** The main panel's knob strip (ADSR, KEEP) gets upright bars; other panels (false) always draw them under */
	bool stripVertical = true;

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1 || !module)
			return;
		NVGcontext* vg = args.vg;
		int shared = clamp(marker >= 0 ? marker : module->selectedMarker, 0, WaveArtifact::NUM_MARKERS - 1);
		for (size_t ci = 0; ci < controls.size(); ci++) {
			app::ParamWidget* w = controls[ci];
			if (!w->isVisible() || w->module != module)
				continue;
			int m = ci < controlMarkers.size() ? clamp(controlMarkers[ci], 0, WaveArtifact::NUM_MARKERS - 1) : shared;
			NVGcolor col = layout::MARKER_COLORS[m];
			// A dot: some step of the marker's row locks it (white: the selected step does). On a knob it sits in the
			// knob's middle; on a screen or button (shape, DEST, filter type, toggles) above it, clear of its text.
			int lockedSteps = module->stepsLockingInRow(w->paramId, m);
			float value;
			bool locked;
			bool hasStep = module->stepValueFor(w->paramId, &value, &locked);
			if (lockedSteps > 0) {
				bool knob = dynamic_cast<app::Knob*>(w) != nullptr;
				Vec at = knob ? w->box.getCenter() : Vec(w->box.pos.x + w->box.size.x / 2.f, w->box.pos.y - 4.6f);
				nvgBeginPath(vg);
				nvgCircle(vg, at.x, at.y, knob ? 2.6f : 1.7f);
				nvgFillColor(vg, (hasStep && locked) ? nvgRGB(0xff, 0xff, 0xff) : col);
				nvgFill(vg);
				// On the black knob cap: a dark ring keeps a pale color readable
				if (knob) {
					nvgStrokeColor(vg, nvgRGB(0x16, 0x18, 0x22));
					nvgStrokeWidth(vg, 0.8f);
					nvgStroke(vg);
				}
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
			bool vertical = stripVertical && cy > mm2px(layout::STRIP_Y0) && cy < mm2px(layout::STRIP_Y0 + layout::STRIP_H);
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
