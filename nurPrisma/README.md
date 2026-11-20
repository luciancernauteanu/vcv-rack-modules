# Nur Prisma

A four-core stereo spread filter for VCV Rack 2 (18 HP). Four resonant filter cores move together (CUTOFF), fan
apart (SPREAD), stack in parallel or in series and spread across the stereo field; each core can also be set on its
own. (MOTION, the cores moving by themselves, is being built in the branch `feat/nur-prisma-motion`.)
A poly input gives each voice its own core. ANALOG and DIGITAL give the stack its character.

**Status:** in development (branch `feat/nur-prisma`). Panel and engine work; custom knobs and by-ear tuning next.

## Ping

Patch a trigger into **PING**: the cores ring like struck bars at their pitches. RES sets how long they ring (lower
notes ring longer); the top of RES sustains and self-oscillates. The trigger voltage is the velocity (10 V = full).
**STRIKE** is the mallet: a hard click, a softer felt that darkens high notes, then a breathy noise burst.
**STRUM** spreads a ping over the cores (right = upward, left = downward). Right-click > Ping mode: All, Cycle (each
trigger the next core: a clock arpeggiates) or Random. A poly PING pings core i from channel i.
SHAPE **Chord** tunes the cores to a chord (right-click > Chord); SPREAD climbs through its inversions.

## Low-pass gate (LPG)

A vactrol-style low-pass gate on every core, set from the right-click menu (a gold **LPG** shows inside the ring while
it is on). **Depth** 0 = off; at 100% a core is shut (silent and dark) until something opens it. The gate opens in
about 2 ms and closes slowly: its release is fast at first and slows as it closes, like a vactrol, and level and
cutoff close together (it gets duller as it gets quieter). **Release** sets the time, 40 ms to 1.5 s. **Gate opens
on**: ping only (every ping plucks a core), input level only, or both (default). On the input the gate follows its
transients: it reads the clean input (before DRIVE and NOISE, so noise alone never opens it), in dB (-40 dBV shut,
+6 dBV open, with a vactrol-like curve), so a hit opens it fully and it closes down the hit's decay (a drum: 96% open
just after the hit, 38% at 200-300 ms with the fastest release). A long Release closes slower than the hit, like a
real vactrol. Level compensation stays at 0 dB while the gate is on.

## 24 dB slope

Each core can have a steeper, ladder-like low-pass: right-click a core's bullet > "24 dB slope" (a small mark shows
above the bullet), or right-click the module > "Low-pass slope, all cores". A 24 dB core adds a second low-pass stage
at the same cutoff (about 24 dB more attenuation two octaves above it), so it sounds heavier and rounder. It only
affects the low-pass share of the sound (TYPE LP; BP and HP are left alone). Under **ANALOG**, high RES also thins
the bass of a 24 dB core, the classic ladder trait; that loss is given back at the core's output, so a 24 dB core with ANALOG 1 keeps its level (pings, noise and input
within about 1.5 dB).

## Noise tail (noise follows the ping / input)

**Hold the NOISE label** (0.5 s) for the **noise tail** slider (also in the right-click menu, with what the noise
follows: ping, input level, or both, the default). The noise follows the envelope of a ping and / or of the input, and
the slider sets how long it lingers after it:

- **0%**: no tail. The noise sounds only while a ping or the input sounds (a ping gets a ~10 ms breath).
- **in between**: the tail grows (50% ~ 140 ms, 90% ~ 1.2 s), so the noise blooms after each hit and fades.
- **100%** (default): full tail, plain noise, no envelope.

The label reads **NOISE ~** while the noise follows (tail below 100%). A ping opens its own core's noise by its
velocity; the input is followed by its level in dB (-40 dBV closed, -10 dBV open), so the noise moves with the
dynamics. With nothing to follow the cores go idle (no CPU).

## Pings and DRIVE / ANALOG

**DRIVE** (before the cores or both) drives pings too: the strike is saturated (a brighter, grittier hit) and every
core's ring goes through a saturating waveshaper, so the ring gets odd harmonics in its attack and a compressed,
sustained tail (a click ping: 3rd harmonic from -45 dB to -10 dB at full drive, loudness within 1 dB; it fades in with
the amount, so a little DRIVE stays clean). Before the cores the drive therefore acts on the input and on each core's
ring; after the cores it acts on the mixed output only. **ANALOG** adds a mild saturation to the strike and gives
every ping a little life: a random
+-12% in strength at full ANALOG, so no two hits are identical. At DRIVE 0 and ANALOG 0 pings are exactly as before.

## Digital and analog feel

- **Digital noise** (right-click > Noise type): NOISE becomes a one-bit shift-register sequence, as in old hardware
  noise generators. COLOR steps the register length (17, 15, 11, 9, 7, 5, 4, 3 bits): long is white, short repeats
  every 2^n - 1 samples and turns into a pitched, metallic buzz (3 bits: sample rate / 7). Pinged through the cores it
  gives digital bells.
- **PCM gate** (top of DIGITAL, above 75%): every sample below a threshold is zeroed, with no envelope: a gritty,
  broken-digital texture. Quiet material disappears first.
- **Thermal drift** (ANALOG): each core wanders a few cents in pitch (up to +-4) and a few percent in resonance (+-8%),
  slowly (targets every 7-20 s), on top of the fixed per-instance mismatch.
- **Level compensation hears like an ear** (right-click > Level compensation hears; default): the detector is
  weighted like a loudness meter (a high-pass at 120 Hz and +4 dB above ~1.7 kHz), so a dark low-pass and a bright
  high-pass at the same setting feel equally loud. Flat (RMS) is still available.

## Controls

| Control | What it does |
|---|---|
| **CUTOFF** (centre knob) | Moves all four cores at once; the spacing between them stays the same |
| **SPREAD** (prism ring around it) | From all four cores on one peak to fanned out over ±2.5 octaves. The dots on the ring show each core's frequency |
| **Core bullets** (ALL and four colours, under the ring) | Pick a core to edit on its own: CUTOFF (centre knob), TYPE, RES, BAND and the SPREAD ring then set that core only, WIDTH becomes its PAN and MIX its LEVEL. A core follows the global knobs until you change one of them in focus; from then on it keeps its own value (an own CUTOFF is a detune from the core's place, so CUTOFF, SPREAD and SHAPE still move every core; an own PAN replaces WIDTH's placement and moves both channels of the core). Right-click > "All cores follow the global knobs again" clears every core's own settings. Click it again (or ALL) to go back. The bullets never move, so a core can be picked while MOTION moves its dot. A ring around a bullet = that core has its own settings; right-click a bullet to make it follow the global knobs again or to reset it |
| **Core dots** (on the ring) | Each core's own share of SPREAD: drag a dot up / down (-100% .. 200%, double-click = 100%). 0% keeps that core on CUTOFF, negative puts it on the other side. In Scale and Chord, amounts other than 100% leave the scale |
| **Quantize to the scale** (right-click, scale and root from the same menu; SCALE shows in the ring) | CUTOFF and every core that SPREAD fans out snap to notes of the scale, so the cores always play scale notes. SHAPE Chord and Scale already build their notes from the root and are not re-snapped |
| **SHAPE** | How the cores are spaced: Even, Harmonic, Scale, **Random** (the big rainbow LED in the middle of the SHAPE LEDs: click it for new random places, which glide in), Chord, **Bell** (hum, prime, minor-third tierce, nominal: 1 : 2 : 2.4 : 4), **Vowel** (formants; SPREAD morphs A > E > I > O > U, CUTOFF shifts the voice, 1 kHz = adult). Bell / Harmonic are exact at SPREAD 50%. The label shows the shape's name. Right-click > Chord and scale tuning: Equal or **Just** (pure ratios: pinged chords fuse instead of beating) |
| **TYPE** | LP → BP → HP → notch, morphing continuously |
| **RES** | Resonance, self-oscillating at the top of the knob |
| **BAND** | Narrows each core to a steep band at its pitch (3 octaves down to 1/24 octave): a drastic filter bank. 0 = off |
| **DRIVE** | Saturation, from warm to rich (up to about +22 dB into the curve). Right-click: **voicing** Warm (tanh), Diode (one-sided: strong even harmonics), Fold (wavefolder: gritty); **position** before the cores (the input and each core's ring), after them (the mixed output: the harmonics are not filtered) or both at 70% each. The label shows the choice: DRIVE / DIODE / FOLD, + POST or BOTH. **Click the label** for the next voicing, **hold it** (0.5 s) for the next position. Diode and Fold add a high cut that grows with the amount |
| **STACK** | Parallel (resonator, vowels, chords) → series (steep, aggressive) |
| **WIDTH** | Spreads the cores over left and right; 0 is mono-safe |
| **MIX** | Dry / wet |
| **GAIN** | Output gain, -12 .. +12 dB (right of DRIVE, the last knob of the row), to set the level yourself; ramped over 5 ms, before the 10 V safety limiter |
| Level compensation (right-click) | **Full** (default): the output keeps the input's loudness whatever BAND, TYPE or RES do (-12 .. +24 dB). **Gentle**: +-6 dB. **Off**: a closing filter gets quieter, as on a classic filter |
| **NOISE**, **COLOR** (mini slider right of NOISE) | Noise fed into the cores (COLOR: white → pink → brown → crackle, bottom to top): with high RES the stack sings on its own. **Hold the NOISE label** for how much it follows the ping / input (see Noise follows) |
| **FEEL** + switch | One knob for **ANALOG** and **DIGITAL**: the switch beside it (down = ANALOG, up = DIGITAL) picks which one the knob edits, and each keeps its own value (saved with the patch). Flip the switch and the knob jumps to that one's value; a small dot on the knob's rim shows the other one (amber = ANALOG, cyan = DIGITAL), the label reads FEEL · ANALOG / FEEL · DIGITAL and the tooltip shows both |
| ANALOG (FEEL) | Slightly mismatched cores, saturation, a soft noise floor, and resonance that reacts to level like an analog filter: loud input widens and softens the peak, quiet signals and pings ring freely, a hard ping blooms (settles fast, then rings on) |
| DIGITAL (FEEL) | Coarse filter maths and rate reduction: the sound of old DSP |
| **STRUM**, **STRIKE** | See Ping above |


**Jacks:** V/OCT, CUTOFF and RES take poly CV (one channel per core). IN L / IN R take stereo or poly audio (one voice per
core). CORES outputs each core on its own channel.

## Building

From PowerShell with `scripts/dev.ps1` loaded: `np-build`, `np-test`, and `np-install-public` for the downloaded
Rack (see `scripts/README.md`).

## Credits

Fonts: Cinzel (The Cinzel Project Authors) and Jost (The Jost Project Authors), SIL Open Font License 1.1
(`res/fonts/OFL-*.txt`).
