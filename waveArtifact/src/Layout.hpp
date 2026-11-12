#pragma once
#include "plugin.hpp"

// Panel geometry in mm. res/waveArtifact.svg draws the same section boxes, so keep both in sync.
// 42 HP = 213.36 mm x 128.5 mm (the right 6 HP column holds the five artifact outputs).
//
//  y  11..43.5    per-artifact sections: ARTIFACT | WAVE | GAIN | FILTER | PITCH | PAN | tempo: BPM, CLOCK, RESET
//  y  44.7..52.1  strip: play mode; in POLY the selected marker's envelope; in GRAIN its KEEP
//  y  53.3..82.8  screen: marker waveforms, marker bar, file status (click to load)
//  y  84.3..112.8 POSITION | FREQ + V/OCT | MASTER (type, cutoff, res, drive, spread, fdbk, each with CV) | outputs
//  y 114.3..123.7 wavetable dots (they end above the bottom-left screw)

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
	{18.f, 82.5f},    // WAVE, SMOOTH | LFO: shape screen, RATE, AMT, SYNC / T / D (GRAIN, SAMPLE: their groups)
	{84.5f, 96.5f},   // GAIN, PAN
	{98.5f, 134.5f},  // FILTER: cutoff, drive, res, type
	{136.5f, 154.5f}, // PITCH, FINE, FREE
	{156.5f, 176.88f}, // tempo: BPM, CLK OUT, CLOCK, RESET (Auto arrange is in the right-click menu)
};
static const Vec MARKER_BADGE = Vec(11.f, ROW1_Y);
static const Vec WAVE_KNOB = Vec(27.f, ROW1_Y);
static const Vec SMOOTH_KNOB = Vec(27.f, ROW2_Y);
// The marker's WAVE LFO: shape screen and RATE on top; SYNC / T / D and AMT below.
static const Vec LFO_SHAPE_SCREEN = Vec(49.5f, 18.5f);
static const Vec LFO_SHAPE_SIZE = Vec(22.f, 9.f);
static const Vec LFO_RATE_KNOB = Vec(71.5f, ROW1_Y);
static const Vec LFO_SYNC_BUTTON = Vec(44.8f, ROW2_Y);
static const Vec LFO_TRIP_BUTTON = Vec(50.7f, ROW2_Y);
static const Vec LFO_DOT_BUTTON = Vec(55.5f, ROW2_Y);  // SYNC / T / D centered under the shape screen
static const Vec LFO_AMT_KNOB = Vec(71.5f, ROW2_Y);
static const Vec GAIN_KNOB = Vec(90.5f, ROW1_Y);
static const Vec CUTOFF_KNOB = Vec(108.5f, ROW1_Y);
static const Vec DRIVE_KNOB = Vec(124.5f, ROW1_Y);
static const Vec RES_KNOB = Vec(108.5f, ROW2_Y);
static const Vec FILTER_MODE_BUTTON = Vec(124.5f, ROW2_Y);
// PITCH steps in semitones; the FREE button next to FINE switches it to continuous.
static const Vec PITCH_KNOB = Vec(145.5f, ROW1_Y);
static const Vec FINE_KNOB = Vec(140.7f, ROW2_Y);
static const Vec PITCH_FREE_BUTTON = Vec(149.4f, ROW2_Y);
// PAN sits under GAIN (one "level and place" section).
static const Vec PAN_KNOB = Vec(90.5f, ROW2_Y);
// Internal tempo on top; CLOCK and RESET below, in two tight columns. The RESET jack's label is a
// button that restarts the beat grid and the LFOs (same as a gate).
static const Vec BPM_KNOB = Vec(161.3f, ROW1_Y);
/** Beat clock out (internal or external tempo), on an inverted box with its label */
static const Vec CLOCK_OUTPUT = Vec(171.2f, 18.4f);
static const Vec CLOCK_OUT_BOX_POS = Vec(166.4f, 12.2f);
static const Vec CLOCK_OUT_BOX_SIZE = Vec(9.6f, 15.6f);
static const Vec CLOCK_INPUT = Vec(161.3f, ROW2_Y);
static const Vec RESET_INPUT = Vec(171.2f, ROW2_Y);
static const Vec RESTART_BUTTON = Vec(171.2f, 40.6f);
static const Vec RESTART_BUTTON_SIZE = Vec(8.6f, 4.2f);

/** Size of the click-to-cycle filter type buttons */
static const Vec MODE_BUTTON_SIZE = Vec(10.f, 5.f);
/** Size of the small lit buttons: SYNC / FREE, and the narrower T and D */
static const Vec LFO_TOGGLE_SIZE = Vec(6.8f, 4.4f);
static const Vec LFO_TOGGLE_SMALL = Vec(4.2f, 4.4f);

// Middle screen: the 5 markers' waveforms side by side on top, the marker bar, the file line.
static const Vec SCREEN_POS = Vec(6.f, 53.3f);
static const Vec SCREEN_SIZE = Vec(170.88f, 29.5f);
// The rest is relative to SCREEN_POS. The bar matches the SVG's bar-track (y 72.2..73.4, x 14..168.88).
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
/** Previous / next WAV-in-folder arrows, fixed at the left end of the file line */
static const Vec FILE_ARROW_SIZE = Vec(4.2f, 3.6f);
static const float FILE_ARROW_GAP = 1.2f;
/** The file name always starts here (relative to BAR_X0): after both arrows and a 2 mm gap */
static const float FILE_TEXT_INDENT = 2.f * 4.2f + FILE_ARROW_GAP + 2.f;

// Controls band. Its y values are where it was before the strip moved up under the per-artifact sections, plus
// CTRL_DY (the panel SVG's control boxes too).
static const float CTRL_DY = 6.8f;
static const float CTRL_Y0 = 77.5f + CTRL_DY;
static const float CTRL_Y1 = 106.f + CTRL_DY;
static const float CONTROL_Y = 90.5f + CTRL_DY;
static const float CONTROL_LABEL_Y = 101.2f + CTRL_DY;
static const Section CTRL_SECTIONS[] = {
	{6.f, 57.f},    // POSITION, CV AMT, POS CV, position LFO
	{59.f, 81.f},   // FREQ, V/OCT
	{83.f, 148.f},  // master: TYPE, then CUTOFF RES DRIVE SPREAD FDBK
};
static const Vec POSITION_KNOB = Vec(14.f, CONTROL_Y);
// CV column next to the knob: amount trimpot, its label, then the jack and its label (aligned
// with the POSITION label).
static const float POS_CV_LABEL_Y = 88.6f + CTRL_DY;
/** Title above the position LFO's shape screen */
static const float POS_LFO_LABEL_Y = 79.9f + CTRL_DY;
static const Vec POS_ATTEN = Vec(27.f, 83.4f + CTRL_DY);
static const Vec POS_INPUT = Vec(27.f, 94.6f + CTRL_DY);
// The position LFO (same controls as a marker's): shape screen, SYNC / T / D, RATE and AMT. Its
// amount is also in RATE's right-click menu.
static const Vec POS_LFO_SHAPE_SCREEN = Vec(44.5f, 85.2f + CTRL_DY);
static const Vec POS_LFO_SHAPE_SIZE = Vec(17.f, 7.f);
static const Vec POS_LFO_SYNC_BUTTON = Vec(39.5f, 92.4f + CTRL_DY);
static const Vec POS_LFO_TRIP_BUTTON = Vec(45.9f, 92.4f + CTRL_DY);
static const Vec POS_LFO_DOT_BUTTON = Vec(50.8f, 92.4f + CTRL_DY);  // SYNC / T / D centered under the shape screen
static const Vec POS_LFO_RATE_KNOB = Vec(36.4f, 98.8f + CTRL_DY);
static const Vec POS_LFO_SMOOTH_KNOB = Vec(45.f, 98.8f + CTRL_DY);
static const Vec POS_LFO_AMT_KNOB = Vec(53.6f, 98.8f + CTRL_DY);
static const float POS_LFO_KNOB_LABEL_Y = 104.6f + CTRL_DY;
static const Vec FREQ_KNOB = Vec(64.5f, CONTROL_Y);
static const Vec VOCT_INPUT = Vec(75.5f, CONTROL_Y);

// Master section. Each of the five knobs has its CV amount trimpot and CV jack stacked under it.
static const float MASTER_LABEL_Y = 79.6f + CTRL_DY;
static const float MASTER_KNOB_Y = 84.8f + CTRL_DY;
// ~1.5 mm between each knob (7.7 mm) and its amount trimpot (6 mm), then the jack (8 mm)
static const float MASTER_AMT_Y = 93.2f + CTRL_DY;
static const float MASTER_CV_Y = 101.f + CTRL_DY;
static const Vec MASTER_MODE_BUTTON = Vec(89.f, MASTER_KNOB_Y);
static const float MASTER_COL_X[5] = {101.5f, 111.9f, 122.3f, 132.7f, 143.1f}; // cutoff, res, drive, spread, fdbk

// Outputs on an inverted background
static const Vec OUT_BOX_POS = Vec(150.f, CTRL_Y0);
static const Vec OUT_BOX_SIZE = Vec(26.88f, CTRL_Y1 - CTRL_Y0);
// Top row: master GAIN knob, its CV AMT trimpot and the GAIN CV jack. Bottom row: L, R and STOP.
static const float OUT_COL_X[3] = {155.3f, 163.4f, 171.6f};
static const Vec MASTER_GAIN_KNOB = Vec(OUT_COL_X[0], 85.4f + CTRL_DY);
static const Vec MASTER_GAIN_AMT = Vec(OUT_COL_X[1], 85.4f + CTRL_DY);
static const Vec MASTER_GAIN_CV = Vec(OUT_COL_X[2], 85.4f + CTRL_DY);
// STOP is a round warning bullet (no text), centred here.
static const Vec STOP_BUTTON = Vec(171.9f, 97.6f + CTRL_DY);
static const Vec STOP_BUTTON_SIZE = Vec(5.6f, 5.6f);
static const NVGcolor STOP_COLOR = nvgRGB(0xff, 0x5c, 0x5c);
static const float MASTER_GAIN_LABEL_Y = 79.6f + CTRL_DY;
static const Vec LEFT_OUTPUT = Vec(155.3f, 97.6f + CTRL_DY);
static const Vec RIGHT_OUTPUT = Vec(164.1f, 97.6f + CTRL_DY);
static const float OUTPUT_LABEL_Y = 104.1f + CTRL_DY;

// Right column: each artifact's SEND (left) and RETURN (right), a pair per row (artifact 1 on top), numbered in
// the artifact's color, on an inverted background like the main outputs.
static const Vec ARTOUT_BOX_POS = Vec(180.88f, 11.f);
static const Vec ARTOUT_BOX_SIZE = Vec(26.48f, 112.7f); // its bottom lines up with the wavetable dots
static const float ARTOUT_L_X = 188.32f; // SEND; jacks centered in the box (center 194.12), like the number badges
static const float ARTOUT_R_X = 199.92f; // RETURN
static const float ARTOUT_ROW0_Y = 28.f;
static const float ARTOUT_ROW_STEP = 19.5f;
/** Number badge above each pair */
static const float ARTOUT_BADGE_DY = -6.6f;
static const float ARTOUT_BADGE_R = 2.f;
static const float ARTOUT_HEADER_Y = 15.f;
static const float ARTOUT_LR_LABEL_Y = 115.5f;

// Bottom: wavetable dots
static const Vec DOTS_POS = Vec(6.f, 114.3f);
static const Vec DOTS_SIZE = Vec(170.88f, 9.4f);

// Strip under the per-artifact sections (above the screen): play mode, then the selected marker's ADSR. Labels sit
// left of their trimpots.
static const float STRIP_Y0 = 44.7f;
static const float STRIP_H = 7.4f;
static const float ADSR_Y = STRIP_Y0 + STRIP_H / 2.f;
static const float PLAY_MODE_LABEL_X = 15.5f;
static const Vec PLAY_MODE_BUTTON = Vec(27.f, ADSR_Y);
static const Vec PLAY_MODE_BUTTON_SIZE = Vec(14.f, 5.f);
static const Vec ADSR_BOX_POS = Vec(36.f, STRIP_Y0);
static const Vec ADSR_BOX_SIZE = Vec(88.f, STRIP_H);
// Attack, decay, sustain, hold (note length), release
static const float ADSR_KNOB_X[5] = {43.f, 55.f, 67.f, 79.f, 91.f};
static const float ADSR_LABEL_DX = 4.7f;
static const Vec ADSR_SCREEN_POS = Vec(97.f, ADSR_Y - 2.8f);
static const Vec ADSR_SCREEN_SIZE = Vec(25.f, 5.6f);

// GRAIN: its own grain group in the WAVE / LFO section (18..66), where the wavetable's SMOOTH and LFO are.
// START stays the WAVE knob. Row 1: START, DENSITY, SIZE, SCAN, FADE. Row 2: ◀ HIT ▶ and the HITS slider under
// START (SAMPLE's, in the same place), SYNC / T / D under DENSITY and SIZE, SPRAY under SCAN, LOOP | PING under
// FADE. LENGTH, P.SPRD, SPREAD and WINDOW in two extra columns on the right (GRAIN_LENGTH_KNOB...); KEEP in the strip.
static const Vec GRAIN_DENSITY_KNOB = Vec(37.5f, ROW1_Y);
static const Vec GRAIN_SIZE_KNOB = Vec(46.f, ROW1_Y);
static const Vec GRAIN_SCAN_KNOB = Vec(54.5f, ROW1_Y);
static const Vec GRAIN_FADE_KNOB = Vec(62.5f, ROW1_Y);
// SYNC starts just after ▶ (it ends at 35.0)
static const Vec GRAIN_SYNC_BUTTON = Vec(38.8f, ROW2_Y);
static const Vec GRAIN_TRIP_BUTTON = Vec(44.6f, ROW2_Y);
static const Vec GRAIN_DOT_BUTTON = Vec(49.1f, ROW2_Y);
static const Vec GRAIN_SPRAY_KNOB = Vec(55.2f, ROW2_Y);
static const Vec GRAIN_SCAN_MODE_BUTTON = Vec(62.5f, ROW2_Y);
static const Vec GRAIN_SCAN_MODE_SIZE = Vec(6.8f, 4.4f);

// SAMPLE: its own group in the same section, in playing order. Row 1: START (the WAVE knob), LENGTH, LOOP, FADE.
// Row 2, each under what it belongs to: ◀ HIT ▶ under START, BEAT under LENGTH, ONE | LOOP | PING
// under LOOP, the direction under FADE.
static const Vec SAMPLE_LENGTH_KNOB = Vec(38.5f, ROW1_Y);
static const Vec SAMPLE_LOOP_KNOB = Vec(48.5f, ROW1_Y);
static const Vec SAMPLE_FADE_KNOB = Vec(58.5f, ROW1_Y);
static const Vec SAMPLE_PREV_BUTTON = Vec(21.2f, ROW2_Y);
/** HIT (START snaps to the hits), between the arrows that step START from hit to hit */
static const Vec SAMPLE_HIT_BUTTON = Vec(27.f, ROW2_Y);
static const Vec SAMPLE_HIT_SIZE = Vec(6.f, 4.4f);
static const Vec SAMPLE_NEXT_BUTTON = Vec(32.8f, ROW2_Y);
static const Vec SAMPLE_ARROW_SIZE = Vec(4.4f, 4.4f);
static const Vec SAMPLE_BEAT_SIZE = Vec(6.6f, 4.4f);
static const Vec SAMPLE_BEAT_BUTTON = Vec(38.5f, ROW2_Y);
static const Vec SAMPLE_LOOP_MODE_BUTTON = Vec(48.5f, ROW2_Y);
static const Vec SAMPLE_DIR_BUTTON = Vec(58.5f, ROW2_Y);
static const Vec SAMPLE_BUTTON_SIZE = Vec(7.4f, 4.4f);
/** HITS: how many hits are found (0 .. 100 %), a slider under ◀ HIT ▶ (top-left corner, size) */
static const Vec SAMPLE_HITS_SLIDER_POS = Vec(19.2f, 38.3f);
static const Vec SAMPLE_HITS_SLIDER_SIZE = Vec(15.8f, 3.2f);

// GRAIN's two extra columns at the right end of its section (WAVE's LFO RATE / AMT are there in WAVE): LENGTH and
// P.SPRD (pitch scatter) on top, SPREAD and a small WINDOW screen (no title: click it for the list) below.
static const Vec GRAIN_LENGTH_KNOB = Vec(70.5f, ROW1_Y);
static const Vec GRAIN_PSCATTER_KNOB = Vec(78.f, ROW1_Y);
static const Vec SPREAD_KNOB = Vec(70.5f, ROW2_Y);
static const Vec GRAIN_WINDOW_SCREEN = Vec(78.f, ROW2_Y);
static const Vec GRAIN_WINDOW_SIZE = Vec(7.f, 4.4f);

// KEEP (speed) (GRAIN only) in the strip, right of the envelope, in its own box. It keeps ~1.6 mm on its right for
// its upright step value bar (StepValueBars).
static const Vec SPREAD_BOX_POS = Vec(126.f, STRIP_Y0);
static const Vec SPREAD_BOX_SIZE = Vec(12.f, STRIP_H);
static const Vec KEEP_SPEED_BUTTON = Vec(131.5f, ADSR_Y);
static const Vec KEEP_SPEED_BUTTON_SIZE = Vec(9.f, 4.4f);

// Pastel, blue through purple to red.
static const NVGcolor MARKER_COLORS[5] = {
	nvgRGB(0x8f, 0xb8, 0xff), // blue
	nvgRGB(0xa9, 0xa4, 0xff), // periwinkle
	nvgRGB(0xc7, 0x9b, 0xf2), // lavender
	nvgRGB(0xe6, 0x9a, 0xc8), // orchid
	nvgRGB(0xff, 0x9a, 0x9e), // rose
};
/** A muted marker's bullet and slot */
static const NVGcolor MUTED_COLOR = nvgRGB(0x7c, 0x80, 0x8a);
/** Screen text and highlights */
static const NVGcolor SCREEN_TEXT = nvgRGB(0xb8, 0xc4, 0xff);
static const NVGcolor SCREEN_DIM = nvgRGB(0x5a, 0x60, 0x80);

} // namespace layout
