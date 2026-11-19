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

## Controls

| Control | What it does |
|---|---|
| **CUTOFF** (centre knob) | Moves all four cores at once; the spacing between them stays the same |
| **SPREAD** (prism ring around it) | From all four cores on one peak to fanned out over ±2.5 octaves. The dots on the ring show each core's frequency |
| **Core bullets** (ALL and four colours, under the ring) | Pick a core to edit on its own: CUTOFF (centre knob), TYPE, RES, BAND and the SPREAD ring then set that core only, WIDTH becomes its PAN and MIX its LEVEL. A core follows the global knobs until you change one of them in focus; from then on it keeps its own value (an own CUTOFF takes the core out of SPREAD and global CUTOFF; an own PAN replaces WIDTH's placement and moves both channels of the core). Click it again (or ALL) to go back. The bullets never move, so a core can be picked while MOTION moves its dot. A ring around a bullet = that core has its own settings; right-click a bullet to make it follow the global knobs again or to reset it |
| **Core dots** (on the ring) | Each core's own share of SPREAD: drag a dot up / down (-100% .. 200%, double-click = 100%). 0% keeps that core on CUTOFF, negative puts it on the other side. In Scale and Chord, amounts other than 100% leave the scale |
| **SHAPE** | How the cores are spaced: Even, Harmonic, Scale, Random, Chord |
| **TYPE** | LP → BP → HP → notch, morphing continuously |
| **RES** | Resonance, self-oscillating at the top of the knob |
| **BAND** | Narrows each core to a steep band at its pitch (3 octaves down to 1/24 octave): a drastic filter bank. 0 = off |
| **DRIVE** | Saturation before the cores, from warm to rich (up to about +22 dB into the curve) |
| **STACK** | Parallel (resonator, vowels, chords) → series (steep, aggressive) |
| **WIDTH** | Spreads the cores over left and right; 0 is mono-safe |
| **MIX** | Dry / wet |
| Level compensation (right-click) | **Full** (default): the output keeps the input's loudness whatever BAND, TYPE or RES do (-12 .. +24 dB). **Gentle**: +-6 dB. **Off**: a closing filter gets quieter, as on a classic filter |
| **NOISE**, **COLOR** | Noise fed into the cores (white → pink → brown → crackle): with high RES the stack sings on its own |
| **ANALOG** | Slightly mismatched cores, saturation, drift, a soft noise floor |
| **DIGITAL** | Coarse filter maths and rate reduction: the sound of old DSP |
| **STRUM**, **STRIKE** | See Ping above |


**Jacks:** V/OCT, CUTOFF and RES take poly CV (one channel per core). IN L / IN R take stereo or poly audio (one voice per
core). CORES outputs each core on its own channel.

## Building

From PowerShell with `scripts/dev.ps1` loaded: `np-build`, `np-test`, and `np-install-public` for the downloaded
Rack (see `scripts/README.md`).

## Credits

Fonts: Cinzel (The Cinzel Project Authors) and Jost (The Jost Project Authors), SIL Open Font License 1.1
(`res/fonts/OFL-*.txt`).
