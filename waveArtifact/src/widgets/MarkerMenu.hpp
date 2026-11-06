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
}
