#pragma once
#include "../WaveArtifact.hpp"

/** Paste as one undo step: it overwrites a whole marker at once. */
inline void pasteMarkerWithUndo(WaveArtifact* m, int i) {
	history::ComplexAction* action = new history::ComplexAction;
	action->name = "paste marker settings";
	for (int k = 0; k < WaveArtifact::NUM_MARKER_SETTINGS; k++) {
		int id = WaveArtifact::MARKER_SETTING_PARAMS[k] + i;
		float before = m->params[id].getValue();
		if (before == m->markerClipboard[k])
			continue;
		history::ParamChange* change = new history::ParamChange;
		change->moduleId = m->id;
		change->paramId = id;
		change->oldValue = before;
		change->newValue = m->markerClipboard[k];
		action->push(change);
	}
	m->pasteMarker(i);
	if (action->isEmpty())
		delete action;
	else
		APP->history->push(action);
}

/** A Shift move across the markers, as one undo step, taking in the knob's own step for the marker it was on. */
inline void pushLinkedMoveUndo(WaveArtifact* m) {
	int base = WaveArtifact::MARKER_SETTING_PARAMS[m->linkSetting];
	history::ComplexAction* action = new history::ComplexAction;
	action->name = "move all markers";
	bool hasOwn = false;
	history::State* state = APP->history;
	if (!state->actions.empty() && state->actionIndex == (int) state->actions.size()) {
		history::ParamChange* own = dynamic_cast<history::ParamChange*>(state->actions.back());
		if (own && own->moduleId == m->id && own->paramId == base + m->linkMarker) {
			state->actions.pop_back();
			state->actionIndex--;
			action->push(own);
			hasOwn = true;
		}
	}
	for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
		if (i == m->linkMarker && hasOwn)
			continue;
		float value = m->params[base + i].getValue();
		if (value == m->linkBase[i])
			continue;
		history::ParamChange* change = new history::ParamChange;
		change->moduleId = m->id;
		change->paramId = base + i;
		change->oldValue = m->linkBase[i];
		change->newValue = value;
		action->push(change);
	}
	if (action->isEmpty())
		delete action;
	else
		state->push(action);
}

/** A marker's envelope onto the other four, as one undo step. */
inline void copyEnvelopeToAll(WaveArtifact* m, int from) {
	history::ComplexAction* action = new history::ComplexAction;
	action->name = "copy envelope to all markers";
	for (int i = 0; i < WaveArtifact::NUM_MARKERS; i++) {
		if (i == from)
			continue;
		for (int k = 0; k < WaveArtifact::NUM_ENVELOPE_PARAMS; k++) {
			int id = WaveArtifact::MARKER_ENVELOPE_PARAMS[k];
			float value = m->params[id + from].getValue();
			float before = m->params[id + i].getValue();
			if (before == value)
				continue;
			history::ParamChange* change = new history::ParamChange;
			change->moduleId = m->id;
			change->paramId = id + i;
			change->oldValue = before;
			change->newValue = value;
			action->push(change);
			m->params[id + i].setValue(value);
		}
	}
	if (action->isEmpty())
		delete action;
	else
		APP->history->push(action);
}

/** Mute or unmute a marker, as one undo step. */
inline void toggleMuteWithUndo(WaveArtifact* m, int i) {
	int id = WaveArtifact::MARKER_MUTE_PARAM + i;
	float before = m->params[id].getValue();
	float after = (before > 0.5f) ? 0.f : 1.f;
	m->params[id].setValue(after);
	rack::Context* context = contextGet();
	if (!context || !context->history)
		return;
	history::ParamChange* change = new history::ParamChange;
	change->name = (after > 0.5f) ? "mute marker" : "unmute marker";
	change->moduleId = m->id;
	change->paramId = id;
	change->oldValue = before;
	change->newValue = after;
	context->history->push(change);
}

/** Copy / paste items of a marker's right-click menus. */
inline void addMarkerCopyPasteItems(ui::Menu* menu, WaveArtifact* m, int i) {
	menu->addChild(createMenuItem("Copy settings", "", [=]() {
		m->copyMarker(i);
	}));
	int from = m->markerClipboardFrom;
	std::string paste = (from >= 0) ? string::f("Paste settings of marker %d", from + 1) : "Paste settings";
	menu->addChild(createMenuItem(paste, "", [=]() {
		pasteMarkerWithUndo(m, i);
	}, from < 0));
	menu->addChild(createMenuItem("Copy ADSR to all markers", "", [=]() {
		copyEnvelopeToAll(m, i);
	}));
}
