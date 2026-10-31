#pragma once
#include "plugin.hpp"

// Panel geometry in mm. res/waveArtifact.svg draws the same section boxes, so keep both in sync.
// 36 HP = 182.88 mm x 128.5 mm.
//
//  y  11..43.5  per-marker sections: MARKER | WAVE | GAIN | FILTER | PITCH | PAN | ARRANGE + CLOCK
//  y  46..76    screen: marker waveforms, marker bar, file status (click to load)
//  y  79..104   POSITION | FREQ + V/OCT | MASTER FILTER | outputs
//  y 106..120   wavetable dots

namespace layout {

static const float PANEL_W = 182.88f;
static const float PANEL_H = 128.5f;

struct Section {
	float x0, x1;
};

// Top band: per-marker sections. Two rows: knobs at ROW1_Y, small controls at ROW2_Y.
static const float TOP_Y0 = 11.f;
static const float TOP_Y1 = 43.5f;
static const float ROW1_Y = 19.f;
static const float ROW1_LABEL_Y = 26.f;
static const float ROW2_Y = 34.f;
static const float ROW2_LABEL_Y = 40.5f;
static const Section TOP_SECTIONS[] = {
	{6.f, 17.f},      // marker badge
	{19.f, 53.f},     // WAVE, LFO rate, SMOOTH
	{55.f, 69.f},     // GAIN
	{71.f, 113.f},    // FILTER: cutoff, drive, res, type
	{115.f, 131.f},   // PITCH, FINE
	{133.f, 147.f},   // PAN
	{149.f, 176.88f}, // ARRANGE, CLOCK
};
static const Vec MARKER_BADGE = Vec(11.5f, ROW1_Y);
static const Vec WAVE_KNOB = Vec(36.f, ROW1_Y);
static const Vec LFO_RATE_KNOB = Vec(28.f, ROW2_Y);
static const Vec SMOOTH_KNOB = Vec(44.f, ROW2_Y);
static const Vec GAIN_KNOB = Vec(62.f, ROW1_Y);
static const Vec CUTOFF_KNOB = Vec(81.f, ROW1_Y);
static const Vec DRIVE_KNOB = Vec(103.f, ROW1_Y);
static const Vec RES_KNOB = Vec(81.f, ROW2_Y);
static const Vec FILTER_MODE_BUTTON = Vec(103.f, ROW2_Y);
static const Vec PITCH_KNOB = Vec(123.f, ROW1_Y);
static const Vec FINE_KNOB = Vec(123.f, ROW2_Y);
static const Vec PAN_KNOB = Vec(140.f, ROW1_Y);
static const Vec ARRANGE_BUTTON = Vec(162.94f, ROW1_Y);
static const Vec CLOCK_INPUT = Vec(162.94f, ROW2_Y);

/** Size of the click-to-cycle filter type buttons */
static const Vec MODE_BUTTON_SIZE = Vec(10.f, 5.f);

// Middle screen: the 5 markers' waveforms side by side on top, the marker bar, the file line.
static const Vec SCREEN_POS = Vec(6.f, 46.f);
static const Vec SCREEN_SIZE = Vec(170.88f, 30.f);
// The rest is relative to SCREEN_POS. The bar matches the SVG's bar-track (y 64.9..66.1, x 14..168.88).
static const float SLOTS_TOP = 1.2f;
static const float SLOTS_BOTTOM = 14.2f;
static const float SLOTS_MARGIN = 2.f;
static const float SLOTS_GAP = 1.5f;
static const float BAR_Y = 19.5f;
static const float BAR_X0 = 8.f;
static const float BAR_X1 = 162.88f;
static const float BULLET_R = 2.1f;
/** File/status line (click it to load a WAV) */
static const float STATUS_Y = 26.6f;

// Controls band
static const float CTRL_Y0 = 79.f;
static const float CTRL_Y1 = 104.f;
static const float CONTROL_Y = 90.f;
static const float CONTROL_LABEL_Y = 99.5f;
static const Section CTRL_SECTIONS[] = {
	{6.f, 50.f},  // POSITION, CV AMT, POS CV
	{52.f, 76.f}, // FREQ, V/OCT
	{78.f, 136.f}, // master filter
};
static const Vec POSITION_KNOB = Vec(16.f, CONTROL_Y);
static const Vec POS_ATTEN = Vec(31.f, CONTROL_Y);
static const Vec POS_INPUT = Vec(43.f, CONTROL_Y);
static const Vec FREQ_KNOB = Vec(58.f, CONTROL_Y);
static const Vec VOCT_INPUT = Vec(70.f, CONTROL_Y);
static const Vec MASTER_MODE_BUTTON = Vec(85.f, CONTROL_Y);
static const Vec MASTER_CUTOFF_KNOB = Vec(96.5f, CONTROL_Y);
static const Vec MASTER_RES_KNOB = Vec(107.5f, CONTROL_Y);
static const Vec MASTER_SPREAD_KNOB = Vec(118.5f, CONTROL_Y);
static const Vec MASTER_FDBK_KNOB = Vec(129.5f, CONTROL_Y);

// Outputs on an inverted background
static const Vec OUT_BOX_POS = Vec(138.f, CTRL_Y0);
static const Vec OUT_BOX_SIZE = Vec(38.88f, CTRL_Y1 - CTRL_Y0);
static const Vec LEFT_OUTPUT = Vec(150.f, CONTROL_Y);
static const Vec RIGHT_OUTPUT = Vec(165.f, CONTROL_Y);

// Bottom: wavetable dots
static const Vec DOTS_POS = Vec(6.f, 106.f);
static const Vec DOTS_SIZE = Vec(170.88f, 14.f);

// Pastel, blue through purple to red.
static const NVGcolor MARKER_COLORS[5] = {
	nvgRGB(0x8f, 0xb8, 0xff), // blue
	nvgRGB(0xa9, 0xa4, 0xff), // periwinkle
	nvgRGB(0xc7, 0x9b, 0xf2), // lavender
	nvgRGB(0xe6, 0x9a, 0xc8), // orchid
	nvgRGB(0xff, 0x9a, 0x9e), // rose
};
/** Screen text and highlights */
static const NVGcolor SCREEN_TEXT = nvgRGB(0xb8, 0xc4, 0xff);
static const NVGcolor SCREEN_DIM = nvgRGB(0x5a, 0x60, 0x80);

} // namespace layout
