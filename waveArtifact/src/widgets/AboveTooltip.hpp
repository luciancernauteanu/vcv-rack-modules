#pragma once
#include "../WaveArtifact.hpp"

/** The param tooltip (value and description, as Rack's own), but ABOVE the control: Rack puts it at the bottom-right
corner, over the step value bar drawn under each per-marker control (see StepValueBars). */
struct AboveParamTooltip : ui::Tooltip {
	app::ParamWidget* paramWidget = nullptr;

	void step() override {
		engine::ParamQuantity* pq = paramWidget->getParamQuantity();
		if (pq) {
			text = pq->getString();
			std::string description = pq->getDescription();
			if (description != "")
				text += "\n" + description;
		}
		Tooltip::step();
		// Its bottom-left corner at the control's top-right corner, kept inside the window.
		math::Vec topRight = paramWidget->getAbsoluteOffset(math::Vec(paramWidget->box.size.x, 0.f));
		box.pos = topRight.minus(math::Vec(0.f, box.size.y)).round();
		box = box.nudge(parent->box.zeroPos());
	}
};

/** Any ParamWidget, with its tooltip shown above it instead of below */
template <class T>
struct AboveTooltip : T {
	AboveParamTooltip* above = nullptr;

	void showAbove() {
		if (above || !settings::tooltips || !this->module)
			return;
		above = new AboveParamTooltip;
		above->paramWidget = this;
		APP->scene->addChild(above);
	}
	void hideAbove() {
		if (!above)
			return;
		APP->scene->removeChild(above);
		delete above;
		above = nullptr;
	}

	// Not T::onEnter / onLeave: those make Rack's own tooltip.
	void onEnter(const widget::Widget::EnterEvent& e) override {
		showAbove();
	}
	void onLeave(const widget::Widget::LeaveEvent& e) override {
		hideAbove();
	}
	void onButton(const widget::Widget::ButtonEvent& e) override {
		// The right-click menu replaces the tooltip, as in Rack.
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT && (e.mods & RACK_MOD_MASK) == 0)
			hideAbove();
		// A press on a per-marker control: the sequencer's lane shows that setting for this marker's row.
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			WaveArtifact* m = dynamic_cast<WaveArtifact*>(this->module);
			if (m)
				m->showInLane(this->paramId);
		}
		T::onButton(e);
	}
	/** The control's own menu, then (with a sequencer on the left) clearing this setting's trig locks in its row */
	void appendContextMenu(ui::Menu* menu) override {
		T::appendContextMenu(menu);
		WaveArtifact* m = dynamic_cast<WaveArtifact*>(this->module);
		if (m)
			m->appendLockMenu(menu, this->paramId);
	}
	~AboveTooltip() {
		hideAbove();
	}
};

/** A knob that can show another value than its own for a while: a sequencer step's locked value while it is being
locked (see LinkedKnob). Its ValueArc follows. */
struct StepShownValue {
	bool stepShowing = false;
	float stepShownValue = 0.f;
};
