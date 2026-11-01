#pragma once
#include "../Layout.hpp"
#include "../WaveArtifact.hpp"
#include "WaveDraw.hpp"

/** Middle screen.
Top: one slot per marker (1..5, side by side) showing the waveform it plays right now, so it follows
WAVE, its LFO and dot assignments. The slot lights up with the marker's share of the output.
While a table is previewed, it covers the slots; click it to stop the preview.
Bottom: the marker bar. Click empty bar to add a marker (up to 5; when all 5 are in use it moves
the playhead instead), drag a bullet to move it (it can pass over other markers), click a bullet or slot to
edit that marker, right-click a bullet or slot to delete it. Drag the white playhead to set POSITION.
*/
struct MarkerBar : widget::Widget {
	WaveArtifact* module = nullptr;

	enum DragMode {
		DRAG_NONE,
		DRAG_MARKER,
		DRAG_PLAYHEAD,
	};
	DragMode dragMode = DRAG_NONE;
	bool loadOnRelease = false;
	bool hoverStatus = false;
	int dragMarker = -1;

	// Geometry in px
	float barX0() {
		return mm2px(layout::BAR_X0);
	}
	float barX1() {
		return mm2px(layout::BAR_X1);
	}
	float barY() {
		return mm2px(layout::BAR_Y);
	}
	float xToPos(float x) {
		return clamp((x - barX0()) / (barX1() - barX0()), 0.f, 1.f);
	}
	float posToX(float p) {
		return barX0() + p * (barX1() - barX0());
	}
	math::Rect slotRect(int i) {
		float x0 = mm2px(layout::SLOTS_MARGIN);
		float gap = mm2px(layout::SLOTS_GAP);
		float w = (box.size.x - 2.f * x0 - (wa::NUM_MARKERS - 1) * gap) / wa::NUM_MARKERS;
		float top = mm2px(layout::SLOTS_TOP);
		return math::Rect(x0 + i * (w + gap), top, w, mm2px(layout::SLOTS_BOTTOM) - top);
	}
	math::Rect slotsArea() {
		math::Rect a = slotRect(0);
		a.size.x = slotRect(wa::NUM_MARKERS - 1).getRight() - a.pos.x;
		return a;
	}

	/** Active marker whose bullet is under `p`, nearest first, or -1. */
	int bulletAt(Vec p) {
		int best = -1;
		float bestD = mm2px(layout::BULLET_R * 1.5f);
		for (int i = 0; i < wa::NUM_MARKERS; i++) {
			if (!module->isMarkerActive(i))
				continue;
			float x = posToX(module->params[WaveArtifact::MARKER_POS_PARAM + i].getValue());
			float d = p.minus(Vec(x, barY())).norm();
			// Prefer the selected marker when bullets overlap.
			if (d < bestD || (d <= bestD + 0.5f && i == module->selectedMarker)) {
				best = i;
				bestD = d;
			}
		}
		return best;
	}

	/** Mouse position in this widget's coordinates, valid during a drag. */
	Vec mouseLocal() {
		Vec offset = getAbsoluteOffset(Vec(0, 0));
		return APP->scene->getMousePos().minus(offset).div(getAbsoluteZoom());
	}

	void openMarkerMenu(int i) {
		WaveArtifact* m = module;
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel("Marker " + std::to_string(i + 1)));
		int t = m->markerTable(i);
		if (t >= 0)
			menu->addChild(createMenuLabel(string::f("Artifact %d / %d", t + 1, m->activeBank.load()->numTables)));
		menu->addChild(createMenuItem("Edit", "", [=]() {
			m->selectedMarker = i;
		}));
		menu->addChild(createMenuItem("Delete", "", [=]() {
			m->deleteMarker(i);
		}, m->activeMarkerCount() <= 1));
	}

	/** The file line at the bottom of the screen; clicking it loads a WAV. */
	bool inStatusLine(Vec p) {
		return p.y >= mm2px(layout::STATUS_Y - 3.f);
	}

	/** Previous (0) / next (1) WAV-in-folder arrow at the right end of the file line */
	math::Rect arrowRect(int k) {
		Vec size = mm2px(layout::FILE_ARROW_SIZE);
		// Fixed at the far right of the file line: previous, then next. Does not move with the file name.
		float cx = mm2px(layout::BAR_X1) - size.x / 2.f - (1 - k) * (size.x + mm2px(1.2f));
		return math::Rect(Vec(cx - size.x / 2.f, mm2px(layout::STATUS_Y) - size.y / 2.f), size);
	}
	bool showArrows() {
		return module && module->hasSiblings();
	}
	/** Arrow under `p` (0 previous, 1 next), or -1 */
	int arrowAt(Vec p) {
		if (!showArrows())
			return -1;
		for (int k = 0; k < 2; k++) {
			if (arrowRect(k).grow(Vec(1.f, 1.f)).contains(p))
				return k;
		}
		return -1;
	}
	int hoverArrow = -1;

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS)
			return;
		bool left = e.button == GLFW_MOUSE_BUTTON_LEFT;
		bool right = e.button == GLFW_MOUSE_BUTTON_RIGHT;
		if (!left && !right)
			return;

		// Previous / next WAV in the folder (no dialog, so it can act on press)
		int arrow = arrowAt(e.pos);
		if (arrow >= 0) {
			if (left)
				module->loadSibling(arrow == 0 ? -1 : 1);
			e.consume(this);
			return;
		}

		// Load: the file line, or the empty screen before anything is loaded. The dialog opens on
		// release (onDragEnd), so the press/release pair is not split by the modal dialog.
		bool noBank = !module->activeBank.load();
		if (left && (inStatusLine(e.pos) || (noBank && e.pos.y < mm2px(layout::SLOTS_BOTTOM + 1.f)))) {
			loadOnRelease = true;
			e.consume(this);
			return;
		}

		// Slots
		if (e.pos.y < mm2px(layout::SLOTS_BOTTOM + 1.f)) {
			if (module->previewTable >= 0) {
				if (left) {
					module->previewTable = -1;
					e.consume(this);
				}
				return;
			}
			for (int i = 0; i < wa::NUM_MARKERS; i++) {
				if (!slotRect(i).contains(e.pos) || !module->isMarkerActive(i))
					continue;
				if (left)
					module->selectedMarker = i;
				else
					openMarkerMenu(i);
				e.consume(this);
				return;
			}
			return;
		}

		// Bar
		int hit = bulletAt(e.pos);
		if (hit >= 0) {
			if (right) {
				openMarkerMenu(hit);
			}
			else {
				module->selectedMarker = hit;
				dragMode = DRAG_MARKER;
				dragMarker = hit;
			}
			e.consume(this);
			return;
		}
		if (!left)
			return; // right-click on empty bar: the module's menu
		float playX = posToX(module->shownPosition.load());
		if (std::fabs(e.pos.x - playX) < mm2px(1.5f) || module->activeMarkerCount() >= wa::NUM_MARKERS) {
			dragMode = DRAG_PLAYHEAD;
			module->params[WaveArtifact::POSITION_PARAM].setValue(xToPos(e.pos.x));
		}
		else {
			int added = module->addMarker(xToPos(e.pos.x));
			if (added >= 0) {
				dragMode = DRAG_MARKER;
				dragMarker = added;
			}
		}
		e.consume(this);
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT)
			return;
		float p = xToPos(mouseLocal().x);
		if (dragMode == DRAG_MARKER && dragMarker >= 0)
			module->params[WaveArtifact::MARKER_POS_PARAM + dragMarker].setValue(p);
		else if (dragMode == DRAG_PLAYHEAD)
			module->params[WaveArtifact::POSITION_PARAM].setValue(p);
	}

	void onDragEnd(const DragEndEvent& e) override {
		dragMode = DRAG_NONE;
		dragMarker = -1;
		if (loadOnRelease) {
			loadOnRelease = false;
			openLoadDialog(module);
		}
	}

	void onHover(const HoverEvent& e) override {
		hoverArrow = arrowAt(e.pos);
		hoverStatus = module && inStatusLine(e.pos) && hoverArrow < 0;
		Widget::onHover(e);
		e.consume(this); // needed to receive onLeave
	}

	void onLeave(const LeaveEvent& e) override {
		hoverStatus = false;
		hoverArrow = -1;
	}

	void drawArrows(const DrawArgs& args) {
		if (!showArrows())
			return;
		for (int k = 0; k < 2; k++) {
			math::Rect r = arrowRect(k);
			bool hot = hoverArrow == k;
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, r.pos.x, r.pos.y, r.size.x, r.size.y, 2.f);
			nvgFillColor(args.vg, hot ? nvgRGBA(0xb8, 0xc4, 0xff, 0x40) : nvgRGBA(0xb8, 0xc4, 0xff, 0x18));
			nvgFill(args.vg);
			nvgStrokeColor(args.vg, hot ? layout::SCREEN_TEXT : layout::SCREEN_DIM);
			nvgStrokeWidth(args.vg, 0.8f);
			nvgStroke(args.vg);
			// Triangle pointing left (previous) or right (next)
			Vec c = r.getCenter();
			float h = r.size.y * 0.28f;
			float w = h * 1.1f;
			float dir = (k == 0) ? -1.f : 1.f;
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, c.x + dir * w * 0.6f, c.y);
			nvgLineTo(args.vg, c.x - dir * w * 0.4f, c.y - h);
			nvgLineTo(args.vg, c.x - dir * w * 0.4f, c.y + h);
			nvgClosePath(args.vg);
			nvgFillColor(args.vg, hot ? nvgRGB(0xff, 0xff, 0xff) : layout::SCREEN_TEXT);
			nvgFill(args.vg);
		}
	}

	void drawStatus(const DrawArgs& args) {
		std::shared_ptr<window::Font> font = wadraw::monoFont();
		if (!font)
			return;
		std::string text = module ? module->getStatus() : "Load a WAV (max 10 MB)";
		float y = mm2px(layout::STATUS_Y);
		float x = mm2px(layout::BAR_X0);
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 9.5f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
		nvgFillColor(args.vg, hoverStatus ? nvgRGB(0xff, 0xff, 0xff) : layout::SCREEN_TEXT);
		float right = nvgText(args.vg, x, y, text.c_str(), NULL);
		if (hoverStatus) {
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, x, y + 5.f);
			nvgLineTo(args.vg, right, y + 5.f);
			nvgStrokeColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, 0.6f));
			nvgStrokeWidth(args.vg, 0.7f);
			nvgStroke(args.vg);
		}
		if (module && module->activeBank.load()) {
			nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
			nvgFillColor(args.vg, layout::SCREEN_DIM);
			// Hint at the right end of the bar, as before the arrows.
			float hintX = showArrows() ? arrowRect(0).pos.x - mm2px(1.5f) : mm2px(layout::BAR_X1);
			const char* hint = "click the file name to load another WAV";
			nvgText(args.vg, hintX, y, hint, NULL);
		}
		drawArrows(args);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			nvgSave(args.vg);
			nvgScissor(args.vg, 0, 0, box.size.x, box.size.y);
			drawSlots(args);
			drawPreview(args);
			drawBar(args);
			drawStatus(args);
			nvgRestore(args.vg);
		}
		Widget::drawLayer(args, layer);
	}

	void drawSlots(const DrawArgs& args) {
		// Safe to read: only the UI thread (this thread) frees banks, in ModuleWidget::step().
		const wa::WavetableBank* bank = module ? module->activeBank.load() : nullptr;
		std::shared_ptr<window::Font> font = wadraw::monoFont();
		float pad = mm2px(0.8f);

		for (int i = 0; i < wa::NUM_MARKERS; i++) {
			math::Rect r = slotRect(i);
			NVGcolor c = layout::MARKER_COLORS[i];
			bool active = module ? module->isMarkerActive(i) : (i < 3);
			bool selected = module && module->selectedMarker == i;
			float heard = module ? module->shownHeard[i].load() : (i == 0 ? 1.f : 0.f);

			// Fill grows with this marker's share of the output.
			nvgBeginPath(args.vg);
			nvgRoundedRect(args.vg, r.pos.x, r.pos.y, r.size.x, r.size.y, 3.f);
			nvgFillColor(args.vg, nvgTransRGBAf(c, active ? 0.05f + 0.18f * heard : 0.02f));
			nvgFill(args.vg);
			nvgStrokeColor(args.vg, nvgTransRGBAf(c, selected ? 1.f : active ? 0.45f : 0.15f));
			nvgStrokeWidth(args.vg, selected ? 1.5f : 0.75f);
			nvgStroke(args.vg);

			math::Rect wr = r.grow(Vec(-pad, -pad));
			wr.pos.y += mm2px(2.2f);
			wr.size.y -= mm2px(2.2f);
			// Center line
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, wr.pos.x, wr.getCenter().y);
			nvgLineTo(args.vg, wr.getRight(), wr.getCenter().y);
			nvgStrokeColor(args.vg, nvgTransRGBAf(c, 0.12f));
			nvgStrokeWidth(args.vg, 0.5f);
			nvgStroke(args.vg);

			float index = 0.f;
			if (active) {
				if (bank) {
					index = module->shownIndex[i].load();
					wadraw::drawTable(args.vg, *bank, index, wr, c, 1.3f);
				}
				else if (!module) {
					wadraw::drawSine(args.vg, wr, 2.f + i, c, 1.3f);
				}
			}

			if (!font)
				continue;
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 9.f);
			nvgFillColor(args.vg, nvgTransRGBAf(c, active ? 1.f : 0.35f));
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			std::string num = std::to_string(i + 1);
			nvgText(args.vg, r.pos.x + pad + 1.f, r.pos.y + pad, num.c_str(), NULL);
			// CV tag: an expander is modulating this artifact.
			if (module && active && module->shownCv[i].load(std::memory_order_relaxed)) {
				float tx = r.pos.x + pad + 9.f, ty = r.pos.y + pad, tw = 13.f, th = 8.5f;
				nvgBeginPath(args.vg);
				nvgRoundedRect(args.vg, tx, ty, tw, th, 2.f);
				nvgFillColor(args.vg, c);
				nvgFill(args.vg);
				nvgFontSize(args.vg, 7.f);
				nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
				nvgFillColor(args.vg, nvgRGB(0x12, 0x14, 0x1c));
				nvgText(args.vg, tx + tw / 2.f, ty + th / 2.f + 0.35f * 7.f, "CV", NULL);
				nvgFontSize(args.vg, 9.f);
				nvgFillColor(args.vg, nvgTransRGBAf(c, 1.f));
			}
			// OUT tag: this artifact's own output is patched, so it plays there and not in L/R.
			if (module && (module->outputs[WaveArtifact::ARTIFACT_L_OUTPUT + i].isConnected() || module->outputs[WaveArtifact::ARTIFACT_R_OUTPUT + i].isConnected())) {
				bool cv = active && module->shownCv[i].load(std::memory_order_relaxed);
				float tx = r.pos.x + pad + (cv ? 24.f : 9.f), ty = r.pos.y + pad, tw = 17.f, th = 8.5f;
				nvgBeginPath(args.vg);
				nvgRoundedRect(args.vg, tx + 0.4f, ty + 0.4f, tw - 0.8f, th - 0.8f, 2.f);
				nvgStrokeColor(args.vg, c);
				nvgStrokeWidth(args.vg, 0.8f);
				nvgStroke(args.vg);
				nvgFontSize(args.vg, 7.f);
				nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
				nvgText(args.vg, tx + tw / 2.f, ty + th / 2.f + 0.35f * 7.f, "OUT", NULL);
				nvgFontSize(args.vg, 9.f);
			}
			nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP);
			std::string info;
			if (!active)
				info = "off";
			else if (bank)
				info = string::f("%d/%d", (int) std::round(index) + 1, bank->numTables);
			nvgText(args.vg, r.getRight() - pad - 1.f, r.pos.y + pad, info.c_str(), NULL);
		}

		// (No hint text over the empty slots: the file line at the bottom already says "Load a WAV".)
	}

	void drawPreview(const DrawArgs& args) {
		if (!module)
			return;
		int t = module->previewTable.load();
		const wa::WavetableBank* bank = module->activeBank.load();
		if (t < 0 || !bank || t >= bank->numTables)
			return;
		math::Rect a = slotsArea();
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, a.pos.x, a.pos.y, a.size.x, a.size.y, 3.f);
		nvgFillColor(args.vg, nvgRGBAf(0.03f, 0.035f, 0.05f, 0.94f));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, layout::SCREEN_TEXT);
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);

		math::Rect wr = a.grow(Vec(-mm2px(30.f), -mm2px(1.5f)));
		wadraw::drawTable(args.vg, *bank, (float) t, wr, nvgRGB(0xf0, 0xf0, 0xff), 1.6f);

		std::shared_ptr<window::Font> font = wadraw::monoFont();
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 10.f);
		nvgFillColor(args.vg, layout::SCREEN_TEXT);
		float x = a.pos.x + mm2px(2.f);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
		nvgText(args.vg, x, a.pos.y + mm2px(1.5f), "PREVIEW", NULL);
		std::string tt = string::f("table %d/%d", t + 1, bank->numTables);
		nvgText(args.vg, x, a.pos.y + mm2px(5.5f), tt.c_str(), NULL);
		nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM);
		nvgFillColor(args.vg, layout::SCREEN_DIM);
		nvgText(args.vg, x, a.getBottom() - mm2px(1.2f), "click to stop", NULL);
	}

	void drawBar(const DrawArgs& args) {
		float y = barY();
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, barX0(), y - mm2px(0.6f), barX1() - barX0(), mm2px(1.2f), mm2px(0.6f));
		nvgFillColor(args.vg, nvgRGB(0x3a, 0x3f, 0x52));
		nvgFill(args.vg);

		// Playhead: where the morph is now (knob + CV, smoothed).
		float play = module ? module->shownPosition.load() : 0.35f;
		float px = posToX(play);
		float top = mm2px(layout::SLOTS_BOTTOM + 1.2f);
		float bottom = mm2px(layout::BAR_Y + 3.2f);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, px, top);
		nvgLineTo(args.vg, px, bottom);
		nvgStrokeColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, 0.85f));
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
		float tri = mm2px(1.3f);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, px - tri, top);
		nvgLineTo(args.vg, px + tri, top);
		nvgLineTo(args.vg, px, top + tri * 1.2f);
		nvgClosePath(args.vg);
		nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
		nvgFill(args.vg);

		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));
		float r = mm2px(layout::BULLET_R);
		// Selected marker last, so it draws on top.
		int sel = module ? module->selectedMarker : -1;
		for (int k = 0; k <= wa::NUM_MARKERS; k++) {
			int i = (k < wa::NUM_MARKERS) ? k : sel;
			if (k < wa::NUM_MARKERS && i == sel)
				continue;
			if (k == wa::NUM_MARKERS && !module)
				break;
			bool active = module ? module->isMarkerActive(i) : (i < 3);
			if (!active)
				continue;
			float pos = module ? module->params[WaveArtifact::MARKER_POS_PARAM + i].getValue() : i * 0.5f;
			float x = posToX(pos);
			NVGcolor c = layout::MARKER_COLORS[i];
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, x, y, r);
			nvgFillColor(args.vg, c);
			nvgFill(args.vg);
			if (module && i == sel) {
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, x, y, r + 1.6f);
				nvgStrokeColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
				nvgStrokeWidth(args.vg, 1.2f);
				nvgStroke(args.vg);
			}
			if (font)
				wadraw::drawCenteredNumber(args.vg, font->handle, x, y, r * 1.4f, std::to_string(i + 1), nvgRGB(0x16, 0x18, 0x22));
		}
	}
};
