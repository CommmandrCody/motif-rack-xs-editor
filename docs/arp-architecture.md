# Arpeggio Architecture

**Authority:** *MOTIF-RACK XS Arpeggio Type List* (48 pp., a **separate**
download — the Data List's own table of contents does not mention it) and
*Data List* pp. 68–69 (`40 30`), 75 (`38 pp`).

## Catalog

`data/arpeggios.json` — **6633** types, numbered 1–6633 with no gaps.

Two independent confirmations that 6633 is correct and complete:
1. The Owner's Manual states "6,633 Arpeggio types".
2. The hardware parameter `ARP SF1 Assign Type` documents its range as
   `off, 0001 – 6633`.

Per entry: number, name, main category, sub category, time signature, length
(bars), original tempo, accent flag, random-SFX flag, voice type.

### Categories

`ApKb BaMG Bass Brass Chord Cntr CPrc DrPc GtMG GtPl Hybrd Lead Organ PdMe
RdPp Seq Strng`

### Flags

In the printed list, Accent and Random SFX are marked with a bare capital `O`;
which column it lands in is decided by character offset. The resulting
distribution is musically coherent and is a good sanity check:

* **Accent** — 1909 entries, *exactly* the whole `DrPc` category. Accent
  phrases are a drum feature.
* **Random SFX** — 1012 entries, *exactly* `GtMG` (607) + `BaMG` (405), the two
  **Mega Voice** categories. Random SFX is what Mega Voices exist for (fret
  noise, slaps, breath).

A naive substring match for `O` corrupts this — a voice type such as
"Rock Organ" reads as flagged. Only a standalone `O` token counts.

### Voice Type

Free text naming the voice the arp was written for. Values **in quotation
marks** (2974 entries) name an actual MOTIF-RACK XS voice, e.g.
`"Power Standard Kit 2"`; the rest are general types like `Acoustic Piano`.
The list uses a ditto mark (`:`) for "same as above"; these are forward-filled
in the JSON.

For the `Cntr` category the column instead carries the programmed controller,
e.g. `(CC#11)`, `(no event)` — these arps transmit control data rather than
notes.

## Selection

Five arpeggio slots per Voice **and** per Multi Part, `SF1`–`SF5`.

| Parameter | Voice (`40 30`) | Size |
|---|---|---|
| ARP SF1…SF5 Assign Type | `45 47 49 4B 4D` | 2 (`msb_lsb`) |
| ARP SF Select (which slot is live) | `00` | 1 |
| ARP Switch | `0D` | 1 |

**[verified]** Writing 1, 3861 and 6633 to `40 30 45` round-trips exactly,
confirming both the addresses and the `msb_lsb` encoding
(`d0 = n >> 7`, `d1 = n & 0x7F`).

## Playback parameters

Present in both the Voice arp block (`40 30`, 79 bytes) and the Multi Part arp
block (`38 pp`, 66 bytes):

| Group | Parameters |
|---|---|
| Transport | `ARP Switch`, `ARP Loop`, `ARP Hold` (sync-off/off/on), `ARP Tempo` (5–300, voice block only) |
| Range | `Note Limit Low/High`, `Velocity Limit Low/High`, `Octave Range` (−3…+3), `Output Octave Shift` (−10…+10) |
| Feel | `Quantize Value` (32nd…), `Quantize Strength` (0–100 %), `Swing` (−120…+120), `Unit Multiply` (50/66/75/100/133/150/200 %) |
| Dynamics | `Velocity Rate` (0–200 %), `Gate Time Rate` (0–200 %) |
| Behaviour | `Key Mode` (sort / thru / direct / sort+direct / thru+direct), `Vel Mode` (original / thru), `Change Timing` (realtime / measure), `Trigger Mode` (gate / toggle) |
| Accent | `Accent Velocity Threshold` (off, 1–127), `Accent Start Quantize` |
| Random SFX | `Random SFX` (off/on), `Key On Control`, `Velocity Offset` |
| Per-slot offsets | `Assign Velocity Rate Offset SF1–5`, `Assign Gate Time Offset SF1–5` (−100…+100) |

The Multi Part block adds `ARP MIDI Out Switch` and `ARP MIDI Out Channel`,
which let a part's arpeggio be recorded into the DAW.

## Design notes

* The five per-slot velocity/gate offsets exist so a performer can switch
  between SF1–SF5 and get a different *feel* from the same phrase. Expose the
  slot selector prominently in ARP mode.
* `ARP Tempo` is in the Voice arp block only; in Multi the parts follow the
  Multi tempo. Do not offer a per-part tempo in MULTI mode.
* Search needs to be fast over 6633 rows: index name, categories, tempo, time
  signature and voice type. Filtering by `original_tempo` near the project
  tempo is the single most useful filter for DAW work.
* Naming convention in arp names encodes the section: `MA_`/`MB_`/`MC_`/`MD_`
  are main variations, `FA_`/`FB_`/`FC_` fills, `BA_` break. Parsing this
  prefix gives a free "song section" facet.
