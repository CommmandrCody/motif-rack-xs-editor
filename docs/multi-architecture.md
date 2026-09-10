# Multi Architecture

**Authority:** *Data List* pp. 63, 73–75 (`MULTI COMMON`, `MULTI PART`).

> **Status: hardware-validated.** 202 concrete addresses read back with exactly
> the documented byte count, 0 size mismatches, 0 unexplained silences. Part
> indexing verified across all 16 parts. Multi mode had to be entered from the
> front panel; see `protocol.md`, gotcha 3.

## Shape

A Multi is **Common** plus **16 Parts**. Each part is described by *two*
blocks:

| Block | Address | Bytes |
|---|---|---|
| Multi Common | `36 00 00` | 81 |
| Multi Reverb / Chorus | `36 01/02 00` | 37 / 38 |
| Multi Master EQ / Master Effect | `36 10/11 00` | 20 / 36 |
| Multi Arp (common) | `36 30 00` | 13 |
| **Part 1–16** | `37 pp 00`, `pp = 0–15` | 61 each |
| **Part 1–16 arpeggio** | `38 pp 00`, `pp = 0–15` | 66 each |
| Audio In Part (mLAN) | `39 41 00` | 8 |

The split between `37` (the part) and `38` (the part's arpeggio) matters: a
part's arp settings are *not* in the part block. Total per part is 127 bytes.

## Part parameters (`37 pp`)

| Low | Parameter | Notes |
|---|---|---|
| `01` / `02` | Bank Select MSB / LSB | see `address-map.md` for bank table |
| `03` | Program Number | **0–127, 0-based** -- the table's "1 - 128" is wrong, see below |
| `04` | Receive Channel | 1–16, off (`7F`) |
| `05` | Mono/Poly | |
| `06` / `07` | Velocity Limit Low / High | |
| `08` / `09` | Note Limit Low / High | C-2 – G8 |
| `0A` / `0B` | Pitch Bend Range Upper / Lower | −48…+24 |
| `0C` / `0D` | Velocity Sense Depth / Offset | |
| `0E` | **Volume** | 0–127 |
| `0F` | **Pan** | L63 – C – R63 |
| `11` | Detune | 2 bytes, `nibbles`, −12.8…+12.7 Hz |
| `13` | **Reverb Send** | 0–127 |
| `14` | **Chorus Send** | 0–127 |
| `16` | Dry Level | |
| `17` | Note Shift | −24…+24 semitones |
| `19` | Param. with Voice | whether the part adopts the voice's own settings |
| `1A` | Voice with ARP | whether selecting a voice also loads its arp |

Bold entries are the PERFORM macro controls.

> **[discrepancy] `Program Number` is 0-based, despite the table.** The Data
> List prints its range as "1 - 128". The hardware reports 0-based values:
> part 1 read back program `62` while the loaded kit was `Synthetic Kit`, which
> is 0-based program 62 of the drum Preset bank, and parts set to the first
> voice of a bank report `0` -- a value "1 - 128" cannot express. The raw range
> column (`00 - 7F`) was right all along; only the prose was wrong.
>
> So **no conversion is needed** between this field, MIDI Program Change and
> `data/voices.json`, which all use the same 0-based numbering. Converting
> "to be safe" is what actually produces the off-by-one here. The correction
> is recorded in `data/parameters_corrections.json`.

`Param. with Voice` and `Voice with ARP` decide whether a part's stored
settings survive a voice change. They directly affect DAW recall: with
`Param. with Voice = on`, restoring a voice will overwrite part parameters the
plugin also wants to restore. Restore order therefore matters — see
`state-sync.md`.

## Part arpeggio (`38 pp`)

Same parameter family as the Voice arp block (see `arp-architecture.md`), minus
`ARP Tempo` (parts follow the Multi tempo) and plus:

* `ARP MIDI Out Switch` (off/on)
* `ARP MIDI Out Channel` (1–16, or rcv-ch)

These let an arpeggio be captured back into the DAW as MIDI, which is a
headline feature for the Ableton workflow.

## Why Multi is the DAW context

* 16 parts on 16 receive channels maps directly onto DAW tracks.
* Up to four parts can run arpeggios simultaneously.
* Bank/Program per part means the whole rack configuration is describable as
  data — which is exactly what the plugin needs to serialise.

## Verified behaviour

* All 16 part blocks (`37 00`..`37 0F`) respond, and the `pp` index resolves
  correctly across the full range.
* A Multi's part voice assignments can be read straight out of
  `37 pp 01/02/03` (bank MSB/LSB, program) and resolved against
  `data/voices.json` to real voice names.
* The Audio In Part's mid byte is printed as `pp` but is fixed at `41`
  (`39 41 00`, and `00 41 00` for the Voice-mode copy) -- it is not a part
  index. Treating it as one makes all 8 of its addresses read back nothing.

## Open questions

1. **How to enter Multi mode programmatically.** The documented Mode Change
   parameter is inert on the reference unit; the front panel works. Still
   unresolved, and it blocks unattended DAW recall.
2. Whether `37 pp` responds for parts whose Receive Channel is `off`.
3. Multi bulk dump size and whether a full Multi (Common + 16 x 127 B) can be
   requested as one block via the Bulk Header at `0E mm nn`.
