#pragma once
#include "../Lfo.hpp"
#include "../plugin.hpp"

/** LFO rate: Hz in free mode, note value (e.g. "1/8T") in sync mode, from the enabled kinds.
The sync/triplet/dotted switches are other params of the same module. */
struct LfoRateQuantity : ParamQuantity {
	int syncId = -1;
	int tripletId = -1;
	int dottedId = -1;

	bool flag(int paramId) {
		return module && paramId >= 0 && module->params[paramId].getValue() > 0.5f;
	}
	bool isSync() {
		return flag(syncId);
	}
	std::string getDisplayValueString() override {
		if (isSync()) {
			int div = wa::lfoDivision(getValue(), flag(tripletId), flag(dottedId));
			return wa::SYNC_DIVISIONS[div].name;
		}
		float hz = wa::lfoFreeHz(getValue());
		return string::f(hz < 1.f ? "%.3f" : "%.2f", hz);
	}
	std::string getUnit() override {
		return isSync() ? "" : " Hz";
	}
};

/** Rate knob whose right-click menu holds the LFO's other settings: an optional amount slider,
then Sync to CLOCK (default off) and the note-value kinds the synced knob steps through. */
template <class TBase>
struct LfoRateKnobT : TBase {
	int amountId = -1;
	int syncId = -1;
	int tripletId = -1;
	int dottedId = -1;

	void appendContextMenu(Menu* menu) override {
		Module* m = this->module;
		if (!m)
			return;
		if (amountId >= 0) {
			menu->addChild(new MenuSeparator);
			menu->addChild(createMenuLabel("LFO amount"));
			ui::Slider* amount = new ui::Slider;
			amount->quantity = m->getParamQuantity(amountId);
			amount->box.size.x = 200.f;
			menu->addChild(amount);
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Sync"));
		auto toggle = [=](int paramId, const std::string& text) {
			return createCheckMenuItem(text, "",
				[=]() {
					return m->params[paramId].getValue() > 0.5f;
				},
				[=]() {
					Param& p = m->params[paramId];
					p.setValue(p.getValue() > 0.5f ? 0.f : 1.f);
				}
			);
		};
		menu->addChild(toggle(syncId, "Sync to tempo (BPM / CLOCK)"));
		menu->addChild(toggle(tripletId, "Triplets"));
		menu->addChild(toggle(dottedId, "Dotted"));
	}
};

template <class TKnob>
TKnob* createLfoRateKnob(Vec mmCenter, Module* module, int rateId, int syncId, int tripletId, int dottedId, int amountId = -1) {
	TKnob* k = createParamCentered<TKnob>(mm2px(mmCenter), module, rateId);
	k->syncId = syncId;
	k->tripletId = tripletId;
	k->dottedId = dottedId;
	k->amountId = amountId;
	return k;
}

/** Configures an LFO rate param with its display; default ~0.2 Hz. */
inline LfoRateQuantity* configLfoRate(Module* m, int rateId, int syncId, int tripletId, int dottedId, const std::string& name, float def = 0.394f) {
	LfoRateQuantity* q = m->configParam<LfoRateQuantity>(rateId, 0.f, 1.f, def, name);
	q->syncId = syncId;
	q->tripletId = tripletId;
	q->dottedId = dottedId;
	return q;
}
