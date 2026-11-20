#pragma once

// Panel geometry in mm (18 HP). Mirrored in res/NurPrisma.svg; change both together.

namespace np {
namespace layout {

static const float PANEL_W = 91.44f;
static const float PANEL_H = 128.5f;
/** Centre of the panel */
static const float FILTER_CX = 45.72f;

/** Text */
static const float TITLE_Y = 12.f;
static const float TITLE_SIZE = 8.6f;
static const float SUBTITLE_Y = 19.6f;
static const float SUBTITLE_SIZE = 2.3f;
/** Gap between a knob's edge and its label's centre */
static const float LABEL_GAP = 2.7f;
/** Jack label centre below the jack centre */
static const float JACK_LABEL_DY = 6.6f;

/** Knob diameters (Rack component sizes) */
static const float KNOB_LARGE = 12.2f;
static const float KNOB_MEDIUM = 9.6f;
static const float KNOB_SMALL = 7.7f;

/** Top row */
static const float TOP_Y = 31.5f;
static const float TYPE_X = 16.f;
static const float RES_X = 75.44f;
static const float SHAPE_Y = 29.f;
/** STRUM and STRIKE: small knobs between TYPE, SHAPE and RES */
static const float STRUM_X = 30.9f;
static const float STRIKE_X = 60.54f;
/** Shape LEDs: row below the button */
static const float SHAPE_LED_DY = 4.4f;
/** SHAPE LEDs: pitch between the small ones (the big Random LED in the middle gets 0.8 mm extra each side) */
static const float SHAPE_LED_PITCH = 2.4f;

/** Hero: inner CUTOFF knob, outer SPREAD ring */
static const float HERO_Y = 58.f;
static const float HERO_RING_D = 29.f;
static const float HERO_KNOB_D = 18.f;
static const float HERO_LABEL_Y = 75.f;
/** "SCALE" inside the bottom of the ring, below the CUTOFF knob, while CUTOFF is quantized */
static const float HERO_SCALE_Y = 69.3f;
/** "LPG" tag inside the top of the ring, above the CUTOFF knob, while the low-pass gate is on */
static const float HERO_LPG_Y = 47.4f;

/** Core selector: ALL pill and four bullets under the hero */
static const float BULLET_Y = 79.4f;
static const float BULLET_ALL_X = 34.5f;
static const float BULLET_X0 = 41.f;
static const float BULLET_PITCH = 5.6f;

/** Side columns beside the hero */
static const float SIDE_L_X = 11.f;
static const float SIDE_R_X = 80.44f;
static const float SIDE_Y1 = 50.5f;
static const float SIDE_Y2 = 67.f;

/** Character row */
static const float CHAR_Y = 87.f;
/** Gap between a character knob's edge and its label (tighter than LABEL_GAP: the CV jacks are close) */
static const float CHAR_LABEL_GAP = 2.2f;
/** NOISE, COLOR, FEEL (with its ANALOG / DIGITAL switch), GAIN, DRIVE */
static const float CHAR_X[5] = {11.72f, 28.72f, 43.2f, 62.72f, 79.72f};
/** FEEL's switch, right of the FEEL knob */
static const float FEEL_SWITCH_X = 51.7f;
/** FEEL's label is centred under the knob and its switch */
static const float FEEL_LABEL_X = 47.45f;

/** Jack rows */
static const float CV_Y = 100.f;
static const float CV_X[6] = {8.22f, 23.22f, 38.22f, 53.22f, 68.22f, 83.22f};
static const float DIVIDER_Y = 109.4f;
static const float IO_Y = 116.f;
static const float IO_X[5] = {13.5f, 29.6f, 45.72f, 61.84f, 77.94f};


} // namespace layout
} // namespace np
