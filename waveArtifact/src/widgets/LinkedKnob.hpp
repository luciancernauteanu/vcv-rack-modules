#pragma once
#include "../WaveArtifact.hpp"

/** A per-marker knob for Shift moves across all markers (WaveArtifact::linkStep). Past its own limit the knob stays
put and the rest of the drag goes to the other markers, until each reaches its own limit. Moving back undoes that
extra first, so nothing jumps. Without Shift, or in Rack's rotary knob modes (which follow the mouse's angle, so
there is no "further" to count), it is a plain knob. */
template <class TKnob>
struct LinkedKnob : TKnob {
	/** Shift + double-click: back to the default on every active marker; otherwise the plain knob reset. */
	void onDoubleClick(const widget::Widget::DoubleClickEvent& e) override {
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(this->module);
		if (m && (APP->window->getMods() & RACK_MOD_MASK) == GLFW_MOD_SHIFT && m->resetSettingOnAllMarkers(this->paramId))
			return;
		TKnob::onDoubleClick(e);
	}

	void onDragMove(const widget::Widget::DragMoveEvent& e) override {
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(this->module);
		engine::ParamQuantity* pq = this->getParamQuantity();
		int mods = APP->window->getMods() & RACK_MOD_MASK;
		settings::KnobMode mode = settings::knobMode;
		bool linear = mode == settings::KNOB_MODE_LINEAR || mode == settings::KNOB_MODE_SCALED_LINEAR || this->forceLinear;
		if (!m || !pq || !pq->isBounded() || e.button != GLFW_MOUSE_BUTTON_LEFT || !(mods & GLFW_MOD_SHIFT) || !linear) {
			TKnob::onDragMove(e);
			return;
		}
		// The same movement Knob::onDragMove works out, in the param's units. (Its modifier speeds are
		// private: Shift 4x, Ctrl + Shift 1/100.)
		float modSpeed = (mods == GLFW_MOD_SHIFT) ? 4.f : 0.01f;
		float rangeRatio = pq->getRange() / ((this->maxAngle - this->minAngle) / float(2 * M_PI));
		float delta = (this->horizontal ? e.mouseDelta.x : -e.mouseDelta.y) * settings::knobLinearSensitivity * this->speed * modSpeed * rangeRatio;

		float value = pq->getValue();
		float beyond = m->linkBeyond;
		if (beyond != 0.f) {
			// Past the limit: more goes further, moving back comes back to the limit first.
			float next = beyond + delta;
			m->linkBeyond = ((next > 0.f) == (beyond > 0.f)) ? next : 0.f;
		}
		else if ((value >= pq->getMaxValue() && delta > 0.f) || (value <= pq->getMinValue() && delta < 0.f)) {
			m->linkBeyond = delta;
		}
		else {
			TKnob::onDragMove(e);
		}
	}
};
