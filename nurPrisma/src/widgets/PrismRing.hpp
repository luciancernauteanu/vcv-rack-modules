#pragma once
#include <atomic>
#include <cmath>

#include "../plugin.hpp"

namespace np {

/** Colour on the prism gradient, t in [0, 1) around the ring (red, orange, yellow, green, cyan, blue, violet). */
inline NVGcolor prismColor(float t) {
	static const unsigned char C[8][3] = {{0xff, 0x4d, 0x6d}, {0xff, 0x9f, 0x43}, {0xff, 0xe6, 0x6d}, {0x5e, 0xf3, 0x8c},
		{0x4d, 0xd9, 0xff}, {0x6c, 0x7b, 0xff}, {0xc7, 0x7d, 0xff}, {0xff, 0x4d, 0x6d}};
	t = t - std::floor(t);
	float x = t * 7.f;
	int i = std::min((int) x, 6);
	float f = x - i;
	return nvgRGBf((C[i][0] + (C[i + 1][0] - C[i][0]) * f) / 255.f, (C[i][1] + (C[i + 1][1] - C[i][1]) * f) / 255.f,
		(C[i][2] + (C[i + 1][2] - C[i][2]) * f) / 255.f);
}

/** The colour of core i (0..3): four evenly spaced prism colours. */
inline NVGcolor coreColor(int i) {
	return prismColor(0.02f + 0.25f * i);
}

/** SPREAD: the prism ring around the CUTOFF knob. Drag it like a knob (the CUTOFF knob sits on top and
passes clicks outside its own circle through to here). The ring glows on the light layer, brighter with SPREAD,
and shows one dot per filter core at the core's live frequency (log scale over the knob's sweep). */
struct PrismRing : app::Knob {
	/** Ring radius in px (centre of the stroke); the box adds room for the glow */
	float radius = 0.f;
	/** Live core positions 0..1 (log frequency 20 Hz..20 kHz), written by the module; null in the browser */
	const std::atomic<float>* corePos = nullptr;
	static constexpr float SWEEP = 0.8333333f * (float) M_PI; // +-150 degrees, like the knobs

	void setDiameterMm(float mm) {
		radius = mm2px(mm) * 0.5f;
		float margin = mm2px(2.4f);
		box.size = Vec(2.f * (radius + margin), 2.f * (radius + margin));
		minAngle = -SWEEP;
		maxAngle = SWEEP;
	}

	float spread() {
		engine::ParamQuantity* pq = getParamQuantity();
		return pq ? pq->getScaledValue() : 0.3f;
	}

	void drawRing(NVGcontext* vg, float width, float alpha) {
		Vec c = box.size.div(2.f);
		const int SEGS = 120;
		nvgLineCap(vg, NVG_BUTT);
		for (int i = 0; i < SEGS; i++) {
			float t0 = (float) i / SEGS;
			float a0 = t0 * 2.f * (float) M_PI - 0.5f * (float) M_PI;
			float a1 = (i + 1.03f) / SEGS * 2.f * (float) M_PI - 0.5f * (float) M_PI;
			nvgBeginPath(vg);
			nvgArc(vg, c.x, c.y, radius, a0, a1, NVG_CW);
			NVGcolor col = prismColor(t0);
			col.a = alpha;
			nvgStrokeColor(vg, col);
			nvgStrokeWidth(vg, width);
			nvgStroke(vg);
		}
	}

	/** Where core i's dot sits, relative to the ring's box: on the ring at the core's frequency. Dots that land on an
	earlier dot step inward (2.2 mm each), so every dot stays visible and can be grabbed. */
	Vec dotPos(int i) {
		Vec c = box.size.div(2.f);
		float ang[4] = {};
		i = std::min(std::max(i, 0), 3);
		for (int k = 0; k <= i; k++) {
			float p = corePos ? corePos[k].load(std::memory_order_relaxed) : 0.42f + 0.08f * k;
			ang[k] = -SWEEP + 2.f * SWEEP * std::min(std::max(p, 0.f), 1.f) - 0.5f * (float) M_PI;
		}
		int stack = 0;
		float nearAngle = mm2px(2.4f) / radius; // angle of 2.4 mm along the ring
		for (int k = 0; k < i; k++)
			if (std::fabs(ang[k] - ang[i]) < nearAngle)
				stack++;
		float r = radius - stack * mm2px(2.2f);
		return c.plus(Vec(std::cos(ang[i]), std::sin(ang[i])).mult(r));
	}

	void draw(const DrawArgs& args) override {
		// Unlit ring, visible with the room lights down too (the light layer adds the glow).
		drawRing(args.vg, mm2px(0.9f), 0.55f);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			float s = spread();
			float glow = 0.4f + 0.6f * s;
			drawRing(args.vg, mm2px(3.6f), 0.05f * glow);
			drawRing(args.vg, mm2px(2.4f), 0.09f * glow);
			drawRing(args.vg, mm2px(1.4f), 0.16f * glow);
			drawRing(args.vg, mm2px(0.9f), 0.55f + 0.4f * s);
		}
		app::Knob::drawLayer(args, layer);
	}
};

/** One core's dot on the prism ring, and the control for that core's own spread: drag it up / down (double-click
= 100%, right-click to type a value). It follows its core around the ring as the core moves. */
struct CoreDot : app::Knob {
	PrismRing* ring = nullptr;
	int index = 0;
	/** The module's focus (-1 = ALL), or null: the focused core's dot is ringed, the others dimmed */
	const int* focus = nullptr;
	/** The core's own-flags: with an own cutoff the dot drags the cutoff (spread no longer moves the core) */
	const std::atomic<int>* ownBits = nullptr;
	int spreadParam = -1, cutoffParam = -1, cutoffBit = 0;

	CoreDot() {
		box.size = mm2px(Vec(3.6f, 3.6f));
	}

	void step() override {
		if (ring)
			box.pos = ring->box.pos.plus(ring->dotPos(index)).minus(box.size.div(2.f));
		if (ownBits && spreadParam >= 0)
			paramId = (ownBits->load(std::memory_order_relaxed) & cutoffBit) ? cutoffParam : spreadParam;
		app::Knob::step();
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			bool active = APP->event->hoveredWidget == this || APP->event->draggedWidget == this;
			int f = focus ? *focus : -1;
			Vec d = box.size.div(2.f);
			NVGcolor col = coreColor(index);
			NVGcolor halo = col;
			halo.a = active ? 0.6f : (f >= 0 && f != index) ? 0.12f : 0.35f;
			if (f == index) {
				nvgBeginPath(args.vg);
				nvgCircle(args.vg, d.x, d.y, mm2px(1.75f));
				nvgStrokeColor(args.vg, nvgRGB(0xe9, 0xe3, 0xd5));
				nvgStrokeWidth(args.vg, mm2px(0.25f));
				nvgStroke(args.vg);
			}
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, d.x, d.y, mm2px(active ? 1.8f : 1.5f));
			nvgFillColor(args.vg, halo);
			nvgFill(args.vg);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, d.x, d.y, mm2px(0.9f));
			nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
			nvgFill(args.vg);
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, d.x, d.y, mm2px(0.65f));
			nvgFillColor(args.vg, col);
			nvgFill(args.vg);
		}
		app::Knob::drawLayer(args, layer);
	}
};

/** The CUTOFF knob in the middle of the ring. Its box is square, so its corners overlap the ring: points outside
its circle are left unhandled and fall through to the ring below. */
struct HeroKnob : RoundHugeBlackKnob {
	bool inside(Vec p) {
		Vec c = box.size.div(2.f);
		return p.minus(c).norm() <= c.x;
	}
	void onHover(const HoverEvent& e) override {
		if (inside(e.pos))
			RoundHugeBlackKnob::onHover(e);
	}
	void onButton(const ButtonEvent& e) override {
		if (inside(e.pos))
			RoundHugeBlackKnob::onButton(e);
	}
	void onHoverScroll(const HoverScrollEvent& e) override {
		if (inside(e.pos))
			RoundHugeBlackKnob::onHoverScroll(e);
	}
};

struct GoldJack : app::SvgPort {
	GoldJack() {
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/components/GoldJack.svg")));
	}
};

} // namespace np

namespace np {

/** A button that steps through a switch param's values (wraps at the end): SHAPE and SWEEP SHAPE. LEDs show which. */
struct CycleButton : app::SvgSwitch {
	CycleButton() {
		addFrame(Svg::load(asset::system("res/ComponentLibrary/VCVButton_0.svg")));
	}
};

} // namespace np
