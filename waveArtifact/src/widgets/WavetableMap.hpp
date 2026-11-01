#pragma once
#include "../Layout.hpp"
#include "../WaveArtifact.hpp"
#include "WaveDraw.hpp"

/** The wavetable map: the whole sample at a glance, left to right. Every artifact's single cycle
is drawn one after another as one continuous grey waveform (soft gradient fill, glowing line,
glass sheen), so smooth, bright, spiky or thin regions are visible at once. Each active marker
sits on the waveform as its numbered colour bullet (as on the marker bar), at the artifact it is
playing now (moves with WAVE, its LFO and expander CV).

- Click: give that artifact to the selected marker. Keep dragging to sweep it through the sample.
- Drag a bullet: sweep that marker (it becomes the selected one).
- Double-click: preview that artifact (again to stop). Right-click: preview / assign menu.

The curve is computed once per bank; drawing it is cheap.
*/
struct WavetableMap : widget::Widget {
	WaveArtifact* module = nullptr;

	/** Points per artifact cycle in the cached curve */
	static const int PTS = 32;
	/** Cached curve: numTables * PTS values in -1..1 (scaled by the loudest artifact) */
	std::vector<float> curve;
	int curveSerial = -1;

	// Interaction
	int hovered = -1;
	int dragMarker = -1;

	const wa::WavetableBank* bank() {
		// Safe to read: only the UI thread (this thread) frees banks.
		return module ? module->activeBank.load() : nullptr;
	}

	int tableAt(float x, int n) {
		return clamp((int) (x / box.size.x * n), 0, n - 1);
	}
	float tableX(float index, int n) {
		return (index + 0.5f) / n * box.size.x;
	}
	float bulletR() {
		return std::min(mm2px(layout::BULLET_R), box.size.y * 0.3f);
	}

	/** Each artifact's cycle averaged down to PTS points. Artifacts are loudness-matched, so their
	peaks differ (a spiky cycle reaches higher than a sine): scaling by the loudest keeps that. */
	void buildCurve(const wa::WavetableBank& b) {
		int n = b.numTables;
		curve.assign((size_t) n * PTS, 0.f);
		const int per = wa::FRAME_SIZE / PTS;
		float peak = 1e-6f;
		for (int t = 0; t < n; t++) {
			const float* f = b.frame(t, 0);
			for (int j = 0; j < PTS; j++) {
				// Keep the chunk's extreme (sign kept) so sharp peaks survive the averaging.
				float lo = 0.f, hi = 0.f, sum = 0.f;
				for (int k = 0; k < per; k++) {
					float s = f[j * per + k];
					lo = std::min(lo, s);
					hi = std::max(hi, s);
					sum += s;
				}
				float v = (sum >= 0.f) ? hi : lo;
				curve[(size_t) t * PTS + j] = v;
				peak = std::max(peak, std::fabs(v));
			}
		}
		for (float& v : curve)
			v /= peak;
	}

	/** Module browser: a made-up curve that morphs from sine to saw-like to spiky. */
	void buildPreviewCurve() {
		const int n = 40;
		curve.assign((size_t) n * PTS, 0.f);
		for (int t = 0; t < n; t++) {
			float m = (float) t / (n - 1);
			for (int j = 0; j < PTS; j++) {
				float ph = 2.f * M_PI * j / PTS;
				float v = std::sin(ph) * (1.f - m) + (std::sin(ph) + 0.5f * std::sin(2 * ph) + 0.33f * std::sin(3 * ph)) * m;
				curve[(size_t) t * PTS + j] = v * (0.55f + 0.35f * std::sin(3.f * m + 1.f));
			}
		}
	}

	// --- Interaction -------------------------------------------------------------------------

	Vec mouseLocal() {
		Vec offset = getAbsoluteOffset(Vec(0, 0));
		return APP->scene->getMousePos().minus(offset).div(getAbsoluteZoom());
	}

	/** Active marker whose bullet is under p, selected one preferred, or -1. */
	int bulletAt(Vec p, int n) {
		int best = -1;
		float r = bulletR() + 2.f;
		float bestD = r * r;
		for (int i = 0; i < wa::NUM_MARKERS; i++) {
			if (!module->isMarkerActive(i))
				continue;
			float dx = tableX(module->shownIndex[i].load(), n) - p.x;
			float dy = box.size.y / 2.f - p.y;
			float d = dx * dx + dy * dy;
			if (d < bestD || (d <= bestD + 1.f && i == module->selectedMarker)) {
				best = i;
				bestD = d;
			}
		}
		return best;
	}

	void setWave(int marker, float x) {
		module->params[WaveArtifact::MARKER_WAVE_PARAM + marker].setValue(clamp(x / box.size.x, 0.f, 1.f));
	}

	void onHover(const HoverEvent& e) override {
		const wa::WavetableBank* b = bank();
		hovered = b ? tableAt(e.pos.x, b->numTables) : -1;
		Widget::onHover(e);
		e.consume(this);
	}

	void onLeave(const LeaveEvent& e) override {
		hovered = -1;
	}

	void onButton(const ButtonEvent& e) override {
		const wa::WavetableBank* b = bank();
		if (!module || !b || e.action != GLFW_PRESS)
			return;
		int n = b->numTables;
		int t = tableAt(e.pos.x, n);
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			int hit = bulletAt(e.pos, n);
			if (hit >= 0) {
				// Grab that marker's bullet.
				module->selectedMarker = hit;
				dragMarker = hit;
			}
			else {
				// Give this artifact to the selected marker; dragging on sweeps it.
				module->assignTable(module->selectedMarker, t);
				dragMarker = module->selectedMarker;
			}
			e.consume(this);
		}
		else if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			openMenu(t);
			e.consume(this);
		}
	}

	void onDragMove(const DragMoveEvent& e) override {
		if (module && dragMarker >= 0 && e.button == GLFW_MOUSE_BUTTON_LEFT)
			setWave(dragMarker, mouseLocal().x);
	}

	void onDragEnd(const DragEndEvent& e) override {
		dragMarker = -1;
	}

	void onDoubleClick(const DoubleClickEvent& e) override {
		const wa::WavetableBank* b = bank();
		if (!module || !b || hovered < 0)
			return;
		int t = hovered;
		module->previewTable = (module->previewTable.load() == t) ? -1 : t;
	}

	void openMenu(int t) {
		WaveArtifact* m = module;
		const wa::WavetableBank* b = bank();
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("Artifact %d / %d", t + 1, b->numTables)));
		menu->addChild(createCheckMenuItem("Preview (replaces the output)", "double-click",
			[=]() {
				return m->previewTable.load() == t;
			},
			[=]() {
				m->previewTable = (m->previewTable.load() == t) ? -1 : t;
			}
		));
		if (m->previewTable.load() >= 0 && m->previewTable.load() != t) {
			menu->addChild(createMenuItem(string::f("Stop preview of %d", m->previewTable.load() + 1), "", [=]() {
				m->previewTable = -1;
			}));
		}
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Assign to"));
		for (int i = 0; i < wa::NUM_MARKERS; i++) {
			std::string right;
			if (!m->isMarkerActive(i))
				right = "off, turns on";
			else if (m->markerTable(i) == t)
				right = "has it";
			else if (m->markerTable(i) >= 0)
				right = string::f("now %d", m->markerTable(i) + 1);
			menu->addChild(createMenuItem("Marker " + std::to_string(i + 1), right, [=]() {
				m->assignTable(i, t);
			}));
		}
	}

	// --- Drawing -------------------------------------------------------------------------------

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			nvgSave(args.vg);
			nvgScissor(args.vg, 0, 0, box.size.x, box.size.y);
			drawMap(args);
			nvgRestore(args.vg);
		}
		Widget::drawLayer(args, layer);
	}

	/** Path along the curve, closed back along the centre line (for the fill) or open (for the line). */
	void curvePath(NVGcontext* vg, int n, float mid, float amp, bool closed) {
		float W = box.size.x;
		int total = n * PTS;
		// About two points per pixel is enough.
		int step = std::max(1, (int) (total / (2.f * W)));
		nvgBeginPath(vg);
		if (closed)
			nvgMoveTo(vg, 0.f, mid);
		bool first = !closed;
		for (int k = 0; k < total; k += step) {
			float x = (k + 0.5f) / total * W;
			float y = mid - curve[k] * amp;
			if (first) {
				nvgMoveTo(vg, x, y);
				first = false;
			}
			else
				nvgLineTo(vg, x, y);
		}
		if (closed) {
			nvgLineTo(vg, W, mid);
			nvgClosePath(vg);
		}
	}

	void drawMap(const DrawArgs& args) {
		NVGcontext* vg = args.vg;
		const wa::WavetableBank* b = bank();
		float W = box.size.x, H = box.size.y;
		float r = mm2px(2.f);
		float mid = H / 2.f, amp = H * 0.42f;

		// Background: dark glass, a little lighter through the middle.
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, W, H, r);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, H, nvgRGB(0x0b, 0x0c, 0x0e), nvgRGB(0x19, 0x1a, 0x1e)));
		nvgFill(vg);

		int n = 0;
		if (b) {
			n = b->numTables;
			int serial = module->bankSerial.load();
			if (curveSerial != serial || (int) curve.size() != n * PTS) {
				buildCurve(*b);
				curveSerial = serial;
			}
		}
		else if (!module) {
			if (curve.empty())
				buildPreviewCurve();
			n = (int) curve.size() / PTS;
		}
		if (n <= 0)
			return;

		// Centre line and faint artifact ticks (only when they are far enough apart).
		nvgBeginPath(vg);
		nvgMoveTo(vg, 0, mid);
		nvgLineTo(vg, W, mid);
		nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x14));
		nvgStrokeWidth(vg, 0.6f);
		nvgStroke(vg);
		if (W / n >= 4.f) {
			nvgBeginPath(vg);
			for (int t = 1; t < n; t++) {
				float x = (float) t / n * W;
				nvgMoveTo(vg, x, H - 2.5f);
				nvgLineTo(vg, x, H - 1.f);
			}
			nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x22));
			nvgStrokeWidth(vg, 0.6f);
			nvgStroke(vg);
		}

		// Marker halos: a soft colour glow behind the artifact each marker plays.
		if (module) {
			for (int i = 0; i < wa::NUM_MARKERS; i++) {
				if (!module->isMarkerActive(i))
					continue;
				float x = tableX(module->shownIndex[i].load(), n);
				float hw = std::max(W / n, mm2px(3.f));
				NVGcolor c = layout::MARKER_COLORS[i];
				nvgBeginPath(vg);
				nvgRect(vg, x - hw, 0, 2.f * hw, H);
				nvgFillPaint(vg, nvgBoxGradient(vg, x - hw * 0.25f, 0, hw * 0.5f, H, hw * 0.4f, hw * 0.9f, nvgTransRGBA(c, 70), nvgTransRGBA(c, 0)));
				nvgFill(vg);
			}
		}

		// Hovered artifact: a light band.
		if (hovered >= 0 && hovered < n) {
			nvgBeginPath(vg);
			nvgRect(vg, (float) hovered / n * W, 0, std::max(W / n, 1.5f), H);
			nvgFillColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x16));
			nvgFill(vg);
		}

		// Waveform fill: silver at the peaks fading to smoke at the centre, mirrored for both halves.
		NVGcolor peakC = nvgRGBA(0xd4, 0xd7, 0xde, 0xd0), centreC = nvgRGBA(0x5c, 0x60, 0x68, 0x40);
		curvePath(vg, n, mid, amp, true);
		nvgSave(vg);
		nvgIntersectScissor(vg, 0, 0, W, mid);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0, mid - amp, 0, mid, peakC, centreC));
		nvgFill(vg);
		nvgRestore(vg);
		nvgSave(vg);
		nvgIntersectScissor(vg, 0, mid, W, H - mid);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0, mid + amp, 0, mid, peakC, centreC));
		nvgFill(vg);
		nvgRestore(vg);

		// The line: a wide soft glow under a thin bright stroke.
		curvePath(vg, n, mid, amp, false);
		nvgLineJoin(vg, NVG_ROUND);
		nvgStrokeColor(vg, nvgRGBA(0xe8, 0xea, 0xf0, 0x1c));
		nvgStrokeWidth(vg, 3.f);
		nvgStroke(vg);
		nvgStrokeColor(vg, nvgRGBA(0xf2, 0xf3, 0xf7, 0xd8));
		nvgStrokeWidth(vg, 0.8f);
		nvgStroke(vg);

		// Glass: a sheen over the top half and a soft dark edge all round.
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, W, mid, r);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, mid, nvgRGBA(0xff, 0xff, 0xff, 0x12), nvgRGBA(0xff, 0xff, 0xff, 0x00)));
		nvgFill(vg);
		nvgBeginPath(vg);
		nvgRoundedRect(vg, 0, 0, W, H, r);
		nvgFillPaint(vg, nvgBoxGradient(vg, 0, 0, W, H, r, 4.f, nvgRGBA(0, 0, 0, 0), nvgRGBA(0, 0, 0, 0x90)));
		nvgFill(vg);
		nvgStrokeColor(vg, nvgRGBA(0xff, 0xff, 0xff, 0x18));
		nvgStrokeWidth(vg, 0.7f);
		nvgStroke(vg);

		if (!module)
			return;
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/Nunito-Bold.ttf"));

		// Preview: a pulsing white band on the previewed artifact.
		int p = module->previewTable.load();
		if (p >= 0 && p < n) {
			float pulse = 0.5f + 0.5f * std::sin((float) system::getTime() * 6.f);
			nvgBeginPath(vg);
			nvgRect(vg, (float) p / n * W, 0, std::max(W / n, 2.f), H);
			nvgFillColor(vg, nvgRGBAf(1.f, 1.f, 1.f, 0.2f + 0.3f * pulse));
			nvgFill(vg);
		}

		// Marker bullets on the centre line, like the marker bar's (selected one on top, ringed).
		float br = bulletR();
		int sel = module->selectedMarker;
		for (int k = 0; k <= wa::NUM_MARKERS; k++) {
			int i = (k < wa::NUM_MARKERS) ? k : sel;
			if ((k < wa::NUM_MARKERS && i == sel) || !module->isMarkerActive(i))
				continue;
			float x = clamp(tableX(module->shownIndex[i].load(), n), br + 1.f, W - br - 1.f);
			// A dark shadow ring lifts the bullet off the waveform.
			nvgBeginPath(vg);
			nvgCircle(vg, x, mid, br + 3.f);
			nvgFillPaint(vg, nvgRadialGradient(vg, x, mid + 0.6f, br, br + 3.f, nvgRGBA(0, 0, 0, 0xa0), nvgRGBA(0, 0, 0, 0)));
			nvgFill(vg);
			nvgBeginPath(vg);
			nvgCircle(vg, x, mid, br);
			nvgFillColor(vg, layout::MARKER_COLORS[i]);
			nvgFill(vg);
			if (i == sel) {
				nvgBeginPath(vg);
				nvgCircle(vg, x, mid, br + 1.6f);
				nvgStrokeColor(vg, nvgRGB(0xff, 0xff, 0xff));
				nvgStrokeWidth(vg, 1.2f);
				nvgStroke(vg);
			}
			if (font)
				wadraw::drawCenteredNumber(vg, font->handle, x, mid, br * 1.4f, std::to_string(i + 1), nvgRGB(0x16, 0x18, 0x22));
		}
	}
};
