#pragma once
#include "../Layout.hpp"
#include "../WaveArtifact.hpp"
#include "WaveDraw.hpp"

/** One dot per extracted wavetable, in two rows: the wavetable playground.
Color follows the table's brightness (spectral centroid). Hollow dots are tables where no
fundamental was found. Each active marker rings the dot it is playing, in its color.
Click a dot: give it to the marker being edited. Right-click: preview it (replaces the output until
stopped) or assign it to any of the 5 markers.
*/
struct DotStrip : widget::Widget {
	WaveArtifact* module = nullptr;
	int hovered = -1;
	ui::Tooltip* tooltip = nullptr;

	~DotStrip() {
		destroyTooltip();
	}

	const wa::WavetableBank* bank() {
		// Safe to read: only the UI thread (this thread) frees banks, in ModuleWidget::step().
		return module ? module->activeBank.load() : nullptr;
	}

	int perRow(int n) {
		return (n + 1) / 2;
	}

	Vec dotCenter(int t, int n) {
		int pr = perRow(n);
		float dx = box.size.x / pr;
		int row = t / pr;
		int col = t % pr;
		return Vec((col + 0.5f) * dx, box.size.y * (row == 0 ? 0.27f : 0.73f));
	}

	int dotAt(Vec p) {
		const wa::WavetableBank* b = bank();
		if (!b)
			return -1;
		int n = b->numTables;
		int pr = perRow(n);
		int col = (int) (p.x / (box.size.x / pr));
		int row = p.y < box.size.y / 2.f ? 0 : 1;
		int t = row * pr + col;
		return (col >= 0 && col < pr && t < n) ? t : -1;
	}

	std::string tooltipText() {
		const wa::WavetableBank* b = bank();
		if (!b || hovered < 0 || hovered >= b->numTables)
			return "";
		std::string s = string::f("Wavetable %d / %d", hovered + 1, b->numTables);
		if (!b->pitched[hovered])
			s += " (no pitch found)";
		s += string::f("\nClick: give to marker %d\nRight-click: preview, or assign to a marker", module->selectedMarker + 1);
		return s;
	}

	void createTooltip() {
		if (!settings::tooltips || tooltip)
			return;
		tooltip = new ui::Tooltip;
		tooltip->text = tooltipText();
		APP->scene->addChild(tooltip);
	}

	void destroyTooltip() {
		if (!tooltip)
			return;
		APP->scene->removeChild(tooltip);
		delete tooltip;
		tooltip = nullptr;
	}

	void step() override {
		if (tooltip) {
			if (hovered < 0)
				destroyTooltip();
			else
				tooltip->text = tooltipText();
		}
		Widget::step();
	}

	void onHover(const HoverEvent& e) override {
		hovered = dotAt(e.pos);
		if (hovered >= 0)
			createTooltip();
		e.consume(this);
	}

	void onLeave(const LeaveEvent& e) override {
		hovered = -1;
		destroyTooltip();
	}

	void onButton(const ButtonEvent& e) override {
		if (!module || e.action != GLFW_PRESS)
			return;
		int t = dotAt(e.pos);
		if (t < 0)
			return;
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			module->assignTable(module->selectedMarker, t);
			e.consume(this);
		}
		else if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
			destroyTooltip();
			openDotMenu(t);
			e.consume(this);
		}
	}

	void openDotMenu(int t) {
		WaveArtifact* m = module;
		const wa::WavetableBank* b = bank();
		ui::Menu* menu = createMenu();
		menu->addChild(createMenuLabel(string::f("Wavetable %d / %d", t + 1, b->numTables)));
		menu->addChild(createCheckMenuItem("Preview (replaces the output)", "",
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

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1)
			drawDots(args);
		Widget::drawLayer(args, layer);
	}

	void drawDots(const DrawArgs& args) {
		const wa::WavetableBank* b = bank();
		// In the module browser there is no module, so show a sample strip.
		int n = b ? b->numTables : (module ? 0 : 64);
		if (n == 0)
			return;

		float dx = box.size.x / perRow(n);
		float r = std::min(dx * 0.32f, box.size.y * 0.16f);
		NVGcolor dim = nvgRGB(0x3a, 0x42, 0x6a);
		NVGcolor bright = nvgRGB(0xe6, 0xe2, 0xff);

		for (int t = 0; t < n; t++) {
			Vec c = dotCenter(t, n);
			float br = b ? b->brightness[t] : (float) t / n;
			NVGcolor col = nvgLerpRGBA(dim, bright, br);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, c.x, c.y, r);
			if (!b || b->pitched[t]) {
				nvgFillColor(args.vg, col);
				nvgFill(args.vg);
			}
			else {
				nvgStrokeColor(args.vg, col);
				nvgStrokeWidth(args.vg, 1.f);
				nvgStroke(args.vg);
			}
		}
		if (!b)
			return;

		// Marker rings on the table each marker plays now (WAVE + LFO). Markers sharing a dot stack outward.
		int ringsOn[wa::MAX_TABLES] = {};
		for (int i = 0; i < wa::NUM_MARKERS; i++) {
			if (!module->isMarkerActive(i))
				continue;
			int t = clamp((int) std::round(module->shownIndex[i].load()), 0, n - 1);
			int k = ringsOn[t]++;
			Vec c = dotCenter(t, n);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, c.x, c.y, r + 1.6f + 1.6f * k);
			nvgStrokeColor(args.vg, layout::MARKER_COLORS[i]);
			nvgStrokeWidth(args.vg, i == module->selectedMarker ? 1.6f : 1.1f);
			nvgStroke(args.vg);
		}

		// Previewed dot: pulsing white halo.
		int p = module->previewTable.load();
		if (p >= 0 && p < n) {
			Vec c = dotCenter(p, n);
			float pulse = 0.5f + 0.5f * std::sin((float) system::getTime() * 6.f);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, c.x, c.y, r + 1.f + 1.5f * pulse);
			nvgFillColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, 0.25f + 0.35f * pulse));
			nvgFill(args.vg);
		}

		if (hovered >= 0 && hovered < n) {
			Vec c = dotCenter(hovered, n);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, c.x, c.y, r + 0.8f);
			nvgStrokeColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
			nvgStrokeWidth(args.vg, 1.f);
			nvgStroke(args.vg);
		}
	}
};
