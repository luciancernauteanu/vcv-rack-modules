#pragma once
#include "plugin.hpp"

// Panel geometry in mm. res/waveArtifact.svg draws the same section boxes, so keep both in sync.
// 42 HP = 213.36 mm x 128.5 mm (the right 6 HP column holds the five artifact outputs).
//
//  y  11..43.5  per-artifact sections: ARTIFACT | WAVE | GAIN | FILTER | PITCH | PAN | tempo: BPM, CLOCK, RESET
//  y  46..75.5  screen: marker waveforms, marker bar, file status (click to load)
//  y  77.5..106 POSITION | FREQ + V/OCT | MASTER (type, cutoff, res, drive, spread, fdbk, each with CV) | outputs
//  y 108..120   wavetable dots

namespace layout {

static const float PANEL_W = 213.36f;
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
	{6.f, 16.f},      // ARTIFACT badge
	{18.f, 66.f},     // WAVE, SMOOTH | LFO: shape screen, RATE, AMT, SYNC / T / D
	{68.f, 80.f},     // GAIN, PAN
	{82.f, 118.f},    // FILTER: cutoff, drive, res, type
	{120.f, 138.f},   // PITCH, FINE, FREE
	{140.f, 176.88f}, // tempo: BPM, CLK OUT, CLOCK, RESET (Auto arrange is in the right-click menu)
};
static const Vec MARKER_BADGE = Vec(11.f, ROW1_Y);
static const Vec WAVE_KNOB = Vec(27.f, ROW1_Y);
static const Vec SMOOTH_KNOB = Vec(27.f, ROW2_Y);
// The marker's WAVE LFO: shape screen and RATE on top; SYNC / T / D and AMT below.
static const Vec LFO_SHAPE_SCREEN = Vec(44.5f, 18.5f);
static const Vec LFO_SHAPE_SIZE = Vec(16.f, 9.f);
static const Vec LFO_RATE_KNOB = Vec(59.f, ROW1_Y);
static const Vec LFO_SYNC_BUTTON = Vec(39.8f, ROW2_Y);
static const Vec LFO_TRIP_BUTTON = Vec(45.7f, ROW2_Y);
static const Vec LFO_DOT_BUTTON = Vec(50.5f, ROW2_Y);  // SYNC / T / D centered under the shape screen
static const Vec LFO_AMT_KNOB = Vec(60.5f, ROW2_Y);
static const Vec GAIN_KNOB = Vec(74.f, ROW1_Y);
static const Vec CUTOFF_KNOB = Vec(92.f, ROW1_Y);
static const Vec DRIVE_KNOB = Vec(108.f, ROW1_Y);
static const Vec RES_KNOB = Vec(92.f, ROW2_Y);
static const Vec FILTER_MODE_BUTTON = Vec(108.f, ROW2_Y);
// PITCH steps in semitones; the FREE button next to FINE switches it to continuous.
static const Vec PITCH_KNOB = Vec(129.f, ROW1_Y);
static const Vec FINE_KNOB = Vec(124.2f, ROW2_Y);
static const Vec PITCH_FREE_BUTTON = Vec(132.9f, ROW2_Y);
// PAN sits under GAIN (one "level and place" section).
static const Vec PAN_KNOB = Vec(74.f, ROW2_Y);
// Internal tempo on top; CLOCK and RESET below. The RESET jack's label is a
// button that restarts the beat grid and the LFOs (same as a gate).
static const Vec BPM_KNOB = Vec(149.5f, ROW1_Y);
/** Beat clock out (internal or external tempo), on an inverted box with its label */
static const Vec CLOCK_OUTPUT = Vec(167.5f, 18.4f);
static const Vec CLOCK_OUT_BOX_POS = Vec(162.3f, 12.2f);
static const Vec CLOCK_OUT_BOX_SIZE = Vec(10.4f, 15.6f);
static const Vec CLOCK_INPUT = Vec(149.5f, ROW2_Y);
static const Vec RESET_INPUT = Vec(167.5f, ROW2_Y);
static const Vec RESTART_BUTTON = Vec(167.5f, 40.6f);
static const Vec RESTART_BUTTON_SIZE = Vec(8.6f, 4.2f);

/** Size of the click-to-cycle filter type buttons */
static const Vec MODE_BUTTON_SIZE = Vec(10.f, 5.f);
/** Size of the small lit buttons: SYNC / FREE, and the narrower T and D */
static const Vec LFO_TOGGLE_SIZE = Vec(6.8f, 4.4f);
static const Vec LFO_TOGGLE_SMALL = Vec(4.2f, 4.4f);

// Middle screen: the 5 markers' waveforms side by side on top, the marker bar, the file line.
static const Vec SCREEN_POS = Vec(6.f, 46.f);
static const Vec SCREEN_SIZE = Vec(170.88f, 29.5f);
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
static const float STATUS_Y = 26.f;
/** Previous / next WAV-in-folder arrows at the right end of the file line */
static const Vec FILE_ARROW_SIZE = Vec(4.2f, 3.6f);

// Controls band
static const float CTRL_Y0 = 77.5f;
static const float CTRL_Y1 = 106.f;
static const float CONTROL_Y = 90.5f;
static const float CONTROL_LABEL_Y = 101.2f;
static const Section CTRL_SECTIONS[] = {
	{6.f, 57.f},    // POSITION, CV AMT, POS CV, position LFO
	{59.f, 81.f},   // FREQ, V/OCT
	{83.f, 148.f},  // master: TYPE, then CUTOFF RES DRIVE SPREAD FDBK
};
static const Vec POSITION_KNOB = Vec(14.f, CONTROL_Y);
// CV column next to the knob: amount trimpot, its label, then the jack and its label (aligned
// with the POSITION label).
static const float POS_CV_LABEL_Y = 88.6f;
/** Title above the position LFO's shape screen */
static const float POS_LFO_LABEL_Y = 79.9f;
static const Vec POS_ATTEN = Vec(27.f, 83.4f);
static const Vec POS_INPUT = Vec(27.f, 94.6f);
// The position LFO (same controls as a marker's): shape screen, SYNC / T / D, RATE and AMT. Its
// amount is also in RATE's right-click menu.
static const Vec POS_LFO_SHAPE_SCREEN = Vec(44.5f, 85.2f);
static const Vec POS_LFO_SHAPE_SIZE = Vec(17.f, 7.f);
static const Vec POS_LFO_SYNC_BUTTON = Vec(39.5f, 92.4f);
static const Vec POS_LFO_TRIP_BUTTON = Vec(45.9f, 92.4f);
static const Vec POS_LFO_DOT_BUTTON = Vec(50.8f, 92.4f);  // SYNC / T / D centered under the shape screen
static const Vec POS_LFO_RATE_KNOB = Vec(36.4f, 98.8f);
static const Vec POS_LFO_SMOOTH_KNOB = Vec(45.f, 98.8f);
static const Vec POS_LFO_AMT_KNOB = Vec(53.6f, 98.8f);
static const float POS_LFO_KNOB_LABEL_Y = 104.6f;
static const Vec FREQ_KNOB = Vec(64.5f, CONTROL_Y);
static const Vec VOCT_INPUT = Vec(75.5f, CONTROL_Y);

// Master section. Each of the five knobs has its CV amount trimpot and CV jack stacked under it.
static const float MASTER_LABEL_Y = 79.6f;
static const float MASTER_KNOB_Y = 84.8f;
// ~1.5 mm between each knob (7.7 mm) and its amount trimpot (6 mm), then the jack (8 mm)
static const float MASTER_AMT_Y = 93.2f;
static const float MASTER_CV_Y = 101.f;
static const Vec MASTER_MODE_BUTTON = Vec(89.f, MASTER_KNOB_Y);
static const float MASTER_COL_X[5] = {101.5f, 111.9f, 122.3f, 132.7f, 143.1f}; // cutoff, res, drive, spread, fdbk

// Outputs on an inverted background
static const Vec OUT_BOX_POS = Vec(150.f, CTRL_Y0);
static const Vec OUT_BOX_SIZE = Vec(26.88f, CTRL_Y1 - CTRL_Y0);
// Top row: master GAIN knob, its CV AMT trimpot and the GAIN CV jack. Bottom row: L, R and STOP.
static const float OUT_COL_X[3] = {155.3f, 163.4f, 171.6f};
static const Vec MASTER_GAIN_KNOB = Vec(OUT_COL_X[0], 85.4f);
static const Vec MASTER_GAIN_AMT = Vec(OUT_COL_X[1], 85.4f);
static const Vec MASTER_GAIN_CV = Vec(OUT_COL_X[2], 85.4f);
// STOP is a round warning bullet (no text), centred here.
static const Vec STOP_BUTTON = Vec(171.9f, 97.6f);
static const Vec STOP_BUTTON_SIZE = Vec(5.6f, 5.6f);
static const NVGcolor STOP_COLOR = nvgRGB(0xff, 0x5c, 0x5c);
static const float MASTER_GAIN_LABEL_Y = 79.6f;
static const Vec LEFT_OUTPUT = Vec(155.3f, 97.6f);
static const Vec RIGHT_OUTPUT = Vec(164.1f, 97.6f);
static const float OUTPUT_LABEL_Y = 104.1f;

// Right column: each artifact's own output, a stereo pair per row (artifact 1 on top), numbered in
// the artifact's color, on an inverted background like the main outputs.
static const Vec ARTOUT_BOX_POS = Vec(180.88f, 11.f);
static const Vec ARTOUT_BOX_SIZE = Vec(26.48f, 109.f);
static const float ARTOUT_L_X = 188.32f; // jacks centered in the box (center 194.12), like the number badges
static const float ARTOUT_R_X = 199.92f;
static const float ARTOUT_ROW0_Y = 28.f;
static const float ARTOUT_ROW_STEP = 19.5f;
/** Number badge above each pair */
static const float ARTOUT_BADGE_DY = -6.6f;
static const float ARTOUT_BADGE_R = 2.f;
static const float ARTOUT_HEADER_Y = 15.f;
static const float ARTOUT_LR_LABEL_Y = 115.5f;

// Bottom: wavetable dots
static const Vec DOTS_POS = Vec(6.f, 108.f);
static const Vec DOTS_SIZE = Vec(170.88f, 12.f);

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
