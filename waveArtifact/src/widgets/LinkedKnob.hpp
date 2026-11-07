#pragma once
#include "../WaveArtifact.hpp"
#include "AboveTooltip.hpp"

/** A per-marker knob for Shift moves across all markers (WaveArtifact::linkStep). Past its own limit the knob stays
put and the rest of the drag goes to the other markers, until each reaches its own limit. Moving back undoes that
extra first, so nothing jumps. Without Shift, or in Rack's rotary knob modes (which follow the mouse's angle, so
there is no "further" to count), it is a plain knob. */
template <class TKnob>
struct LinkedKnob : AboveTooltip<TKnob>, StepShownValue {
	/** A drag that edits a sequencer step's lock: the lock's value before snapping (NAN = not started) */
	float lockDrag = NAN;

	/** Turns the knob's picture to `value` (as SvgKnob::onChange does for its own value), without touching the param */
	void rotateTo(float value) {
		engine::ParamQuantity* pq = this->getParamQuantity();
		if (!pq || !pq->isBounded() || pq->getRange() == 0.f)
			return;
		float angle = std::fmod(math::rescale(value, pq->getMinValue(), pq->getMaxValue(), this->minAngle, this->maxAngle), 2 * (float) M_PI);
		this->tw->identity();
		math::Vec center = this->sw->box.getCenter();
		this->tw->translate(center);
		this->tw->rotate(angle);
		this->tw->translate(center.neg());
		this->fb->setDirty();
	}

	/** While a step is selected and the knob is held: it shows (and moves) the step's value */
	void showStep(float value) {
		stepShowing = true;
		stepShownValue = value;
		rotateTo(value);
	}

	/** Released: back to the knob's own value */
	void endStepShow() {
		if (!stepShowing)
			return;
		stepShowing = false;
		widget::Widget::ChangeEvent eChange;
		this->onChange(eChange);
	}

	/** Shift + double-click: back to the default on every active marker; otherwise the plain knob reset. */
	void onDoubleClick(const widget::Widget::DoubleClickEvent& e) override {
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(this->module);
		// A step selected on the sequencer: double-click removes this setting's lock (the step plays the knob again).
		if (m && m->clearStepLock(this->paramId))
			return;
		if (m && (APP->window->getMods() & RACK_MOD_MASK) == GLFW_MOD_SHIFT && m->resetSettingOnAllMarkers(this->paramId))
			return;
		TKnob::onDoubleClick(e);
	}

	void onDragStart(const widget::Widget::DragStartEvent& e) override {
		lockDrag = NAN;
		// A step selected on the sequencer: on press the knob jumps to the step's value (its lock, or its own value
		// where the step has none) and follows the drag; on release it goes back to its own value.
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(this->module);
		float stepValue;
		bool stepLocked;
		if (e.button == GLFW_MOUSE_BUTTON_LEFT && m && m->stepValueFor(this->paramId, &stepValue, &stepLocked))
			showStep(stepValue);
		TKnob::onDragStart(e);
	}

	void onDragEnd(const widget::Widget::DragEndEvent& e) override {
		TKnob::onDragEnd(e);
		if (e.button == GLFW_MOUSE_BUTTON_LEFT)
			endStepShow();
	}

	void onChange(const widget::Widget::ChangeEvent& e) override {
		TKnob::onChange(e);
		// The param changed while the knob shows a step (e.g. its own value set elsewhere): keep showing the step.
		if (stepShowing)
			rotateTo(stepShownValue);
	}

	void onDragMove(const widget::Widget::DragMoveEvent& e) override {
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(this->module);
		engine::ParamQuantity* pq = this->getParamQuantity();
		int mods = APP->window->getMods() & RACK_MOD_MASK;
		settings::KnobMode mode = settings::knobMode;
		bool linear = mode == settings::KNOB_MODE_LINEAR || mode == settings::KNOB_MODE_SCALED_LINEAR || this->forceLinear;
		// A step selected on the sequencer: the drag moves the step's lock, the knob stays. (In the rotary modes the
		// knob moves and WaveArtifact::lockCaptureStep turns that into the lock.)
		float stepValue;
		bool stepLocked;
		if (m && pq && pq->isBounded() && linear && e.button == GLFW_MOUSE_BUTTON_LEFT && m->stepValueFor(this->paramId, &stepValue, &stepLocked)) {
			float modSpeed = (mods == RACK_MOD_CTRL) ? 0.1f : (mods == GLFW_MOD_SHIFT) ? 4.f : (mods == (RACK_MOD_CTRL | GLFW_MOD_SHIFT)) ? 0.01f : 1.f;
			float rangeRatio = pq->getRange() / ((this->maxAngle - this->minAngle) / float(2 * M_PI));
			float delta = (this->horizontal ? e.mouseDelta.x : -e.mouseDelta.y) * settings::knobLinearSensitivity * this->speed * modSpeed * rangeRatio;
			if (std::isnan(lockDrag))
				lockDrag = stepValue;
			lockDrag = clamp(lockDrag + delta, pq->getMinValue(), pq->getMaxValue());
			float lock = pq->snapEnabled ? std::round(lockDrag) : lockDrag;
			m->setStepLock(this->paramId, lock);
			showStep(lock);
			return;
		}
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
