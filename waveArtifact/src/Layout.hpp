#pragma once
#include "plugin.hpp"

// Panel geometry in mm. res/waveArtifact.svg draws the same shapes, so keep both in sync.
// 36 HP = 182.88 mm x 128.5 mm.

namespace layout {

static const float PANEL_W = 182.88f;
static const float PANEL_H = 128.5f;

// Top band: per-marker edit area (left) and file status + load (right)
static const Vec EDIT_POS = Vec(6.f, 11.f);
static const Vec EDIT_SIZE = Vec(104.f, 25.f);
static const float EDIT_KNOB_Y = 19.f;
static const Vec MARKER_BADGE = Vec(13.f, EDIT_KNOB_Y);
static const Vec WAVE_KNOB = Vec(28.f, EDIT_KNOB_Y);
static const Vec GAIN_KNOB = Vec(42.f, EDIT_KNOB_Y);
static const Vec BP_KNOB = Vec(56.f, EDIT_KNOB_Y);
static const Vec RES_KNOB = Vec(70.f, EDIT_KNOB_Y);
static const Vec PITCH_KNOB = Vec(84.f, EDIT_KNOB_Y);
static const Vec PAN_KNOB = Vec(98.f, EDIT_KNOB_Y);
static const float EDIT_LABEL_Y = 26.f;
// Second row (small controls, label to the right of each): SMOOTH under WAVE, then the WAVE LFO.
static const float EDIT_ROW2_Y = 31.5f;
static const Vec SMOOTH_KNOB = Vec(28.f, EDIT_ROW2_Y);
static const Vec LFO_RATE_KNOB = Vec(50.f, EDIT_ROW2_Y);
static const Vec LFO_AMT_KNOB = Vec(72.f, EDIT_ROW2_Y);
static const Vec LFO_SYNC_BUTTON = Vec(88.f, EDIT_ROW2_Y);

static const Vec STATUS_POS = Vec(114.f, 11.f);
static const Vec STATUS_SIZE = Vec(62.88f, 12.f);
static const Vec LOAD_BUTTON = Vec(120.f, 30.f);
static const Vec ARRANGE_BUTTON = Vec(148.f, 30.f);

// Middle: marker bar display (interactive widget arrives in M5)
static const Vec SCREEN_POS = Vec(6.f, 40.f);
static const Vec SCREEN_SIZE = Vec(170.88f, 40.f);

// Controls row
static const float CONTROL_Y = 91.f;
static const float CONTROL_LABEL_Y = 101.5f;
static const Vec POSITION_KNOB = Vec(22.f, CONTROL_Y);
static const Vec POS_ATTEN = Vec(42.f, CONTROL_Y);
static const Vec POS_INPUT = Vec(56.f, CONTROL_Y);
static const Vec FREQ_KNOB = Vec(84.f, CONTROL_Y);
static const Vec VOCT_INPUT = Vec(102.f, CONTROL_Y);
static const Vec CLOCK_INPUT = Vec(120.f, CONTROL_Y);

// Outputs on an inverted background
static const Vec OUT_BOX_POS = Vec(138.f, 83.f);
static const Vec OUT_BOX_SIZE = Vec(38.88f, 21.f);
static const Vec LEFT_OUTPUT = Vec(150.f, CONTROL_Y);
static const Vec RIGHT_OUTPUT = Vec(165.f, CONTROL_Y);

// Bottom: wavetable dots
static const Vec DOTS_POS = Vec(6.f, 106.f);
static const Vec DOTS_SIZE = Vec(170.88f, 14.f);

static const NVGcolor MARKER_COLORS[5] = {
	nvgRGB(0xff, 0x5a, 0x5f),
	nvgRGB(0xff, 0xb4, 0x00),
	nvgRGB(0x3d, 0xdc, 0x97),
	nvgRGB(0x4c, 0xc9, 0xf0),
	nvgRGB(0xb3, 0x88, 0xff),
};

} // namespace layout
