# Normal Voice Architecture

**Authority:** *Data List* pp. 63 (block sizes), 66–69 (`NORMAL VOICE COMMON`,
`NORMAL VOICE ELEMENT`). Verified against hardware: 366 addresses, 0 size
mismatches.

## Shape

A Normal Voice is **Common** plus **8 Elements**. The element count is not an
assumption — the Bulk Dump Block table lists ELEMENT 1 … ELEMENT 8 explicitly,
and the parameter table gives `ee: Element No. (0 – 7)`.

Each element is split across **two address blocks**, which is the single most
important structural fact for the UI and the state model:

| Block | Address | Bytes | Contents |
|---|---|---|---|
| Element group 1 | `41 ee 00` | 95 | Oscillator, Amplitude, Pitch |
| Element group 2 | `42 ee 00` | 70 | Filter, EQ, LFO |

An element is therefore *not* one contiguous region. Editing "the filter of
element 3" means `42 02 xx`, while "the amp envelope of element 3" means
`41 02 xx`. The parameter model hides this; nothing above it should hard-code
the split.

## Common (`40`)

| Mid | Block | Bytes |
|---|---|---|
| `00` | Common 1 — name, categories, general | 82 |
| `01` | Reverb | 37 |
| `02` | Chorus | 38 |
| `03` | Insertion A | 35 |
| `04` | Insertion B | 35 |
| `06` | Controller + LFO | 69 |
| `07` | Audition | 4 |
| `30` | Arpeggio | 79 |

**Voice Name** is 20 bytes at `40 00 00`–`40 00 13`, ASCII, range
`00, 20–7E`. **[verified]** reading these 20 addresses after selecting PRE1
program 0 returns `Full Concert Grand  `, matching the Data List exactly.

Voice Category is four bytes at `40 00 18`–`1B` (Category 1 Main/Sub,
Category 2 Main/Sub), mirroring the four category columns in the Voice List.

## Element group 1 — Oscillator / Amplitude / Pitch (`41 ee`)

**Oscillator.** `Element Assign` (off/on), `Wave Number` (2 bytes, 1–2670),
`Element Group Number`, note and velocity limits, `Velocity Cross Fade`,
`Key On Delay` (+ tempo sync), pan controls (fixed, random, alternate,
scaling), and per-element `Reverb`/`Chorus Send Level`, `Insertion Effect
Switch` and `Output Select`.

**XA Control** (`41 ee 0C`) is the Expanded Articulation selector:
`normal, legato, key off sound, wave cycle, wave random, all AF off, AF 1 on,
AF 2 on`. This is what makes MOTIF XS voices behave the way they do and has no
equivalent in a generic subtractive UI — expose it as a first-class element
control, not buried.

**Amplitude.** `Element Level`, level velocity sensitivity/offset/curve, and a
4-breakpoint **Level Scaling** curve (`33`–`3E`: four break points with four
2-byte offsets).

**AEG** is a 5-stage, level-and-time envelope, not ADSR:

| Stage | Time | Level |
|---|---|---|
| Init | — | `2A` |
| Attack | `25` | `2B` |
| Decay 1 | `26` | `2C` |
| Decay 2 | `27` | `2D` |
| Release | `29` | — |

plus `AEG Time Velocity Segment/Sensitivity` and `Time Key Follow
Sensitivity/Center Note`. A "release" knob in PERFORM maps to `41 ee 29`.

**Pitch.** `Coarse Tune`, `Fine Tune`, `Random Pitch Depth`, pitch key-follow,
and a **PEG** with Hold/Attack/Decay1/Decay2/Release times *and* 2-byte levels
for every stage (`4E`–`57`), plus `PEG Depth`.

## Element group 2 — Filter / EQ / LFO (`42 ee`)

**Filter Type** (`42 ee 00`, range `00–15`) — 19 types:

```
LPF24D  LPF24A  LPF18  LPF18s  LPF12  LPF6
HPF24D  HPF12
BPF12D  BPFw  BPF6
BEF12  BEF6
Dual LPF  Dual HPF  Dual BPF  Dual BEF
LPF12+BPF6
THRU
```

`Filter Cutoff Frequency` is **2 bytes** (`42 ee 01`) — a PERFORM cutoff knob
must use the `msb_lsb` encoding, not a single byte. `Filter Resonance/Width/
Band` (`05`) changes meaning with filter type: resonance for LPF/HPF, width for
BPFw, band for the dual types. Label it dynamically.

There is a second, independent `HPF Cutoff Frequency` (`07`, 2 bytes) plus
`Distance` and `Filter Gain` for the dual/combination types.

**FEG** mirrors the PEG shape: Hold/Attack/Decay1/Decay2/Release times with
2-byte levels, `FEG Depth`, and velocity/key-follow modifiers. Cutoff also has
its own 4-breakpoint scaling curve (`24`–`2F`).

**Element EQ** is a 2-band parametric: `EQ Type`, `EQ Q`, and frequency/gain
for bands 1 and 2.

**Element LFO**: `LFO Wave`, `Key On Sync`, `Key On Delay Time`, `Speed`, and
three destinations — `AMod`, `PMod`, `FMod` depth — plus `Fade In Time`. Note
this is *per element* and separate from the Common LFO in `40 06`; the three
`Common LFO Box n Depth Ratio` parameters at `42 ee 43`–`45` are how an element
subscribes to the Common LFO.

## UI implications

* Model the two-block split in the parameter layer only.
* An 8-element voice with 165 bytes per element is ~1320 bytes of element state
  plus common — too much for per-knob round-trips. Use bulk dump to load,
  Parameter Change to edit (see `state-sync.md`).
* PERFORM's macro knobs map to: cutoff `42 ee 01` (2 bytes), resonance
  `42 ee 05`, attack `41 ee 25`, decay `41 ee 26`, release `41 ee 29`. These
  are *per element* — a musical "cutoff" knob must apply an offset across all
  assigned elements, which is exactly what the front-panel knobs do.
