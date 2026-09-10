# Drum Voice Architecture

**Authority:** *Data List* pp. 63, 70–72 (`DRUM VOICE COMMON`, `DRUM VOICE
KEY`). Verified against hardware: 218 addresses, 0 size mismatches.

## Shape

A Drum Voice is **Common** plus **73 keys**, addressed `47 ee 00` with
`ee = 0 – 72`. 73 keys spans C0–C6 inclusive.

This is a genuinely different model from a Normal Voice and must not be forced
into the same UI:

| | Normal Voice | Drum Voice |
|---|---|---|
| Units | 8 elements | 73 keys |
| Address | `41 ee` + `42 ee` (two blocks) | `47 ee` (one block, 42 bytes) |
| Filter | 19 types, full FEG | LPF + HPF cutoff, **no FEG** |
| Envelope | 5-stage AEG + PEG + FEG | AEG attack / decay 1 / decay 2 only |
| Pitch | PEG, key follow, scaling | Coarse / Fine / velocity sens |
| LFO | per-element LFO | **none** |

A drum key is deliberately a much smaller parameter set. Attempting to render
the Normal Voice editor with most controls greyed out would misrepresent the
instrument.

## Common (`46`)

| Mid | Block | Bytes |
|---|---|---|
| `00` | Common 1 — name, categories | 74 |
| `01` / `02` | Reverb / Chorus | 37 / 38 |
| `03` / `04` | Insertion A / B | 35 / 35 |
| `07` | Audition | 4 |
| `08` | Controller | 30 |
| `30` | Arpeggio | 79 |

As with Normal Voices, the name is 20 ASCII bytes at `46 00 00`–`46 00 13`.

## Drum Key (`47 ee`, 42 bytes)

**Wave.** `Element Assign` (off/on), `Wave Type` (`0: Preset Wave`),
`Wave Number` (2 bytes, `msb_lsb`).

**Note behaviour.** `Receive Note Off` (off/on) — the parameter that decides
whether a cymbal chokes or rings; `Key Assign Mode` (single/multi);
`Alternate Group` (0 = off, 1–127) for hi-hat style mutual exclusion. These
three are the heart of a usable drum editor and should be prominent.

**Level and pan.** `Element Level`, `Level Velocity Sensitivity`, `Pan`,
`Random Pan Depth`, `Alternate Pan Depth`.

**Pitch.** `Coarse Tune` (−48…+48), `Fine Tune` (−64…+63),
`Pitch Velocity Sensitivity`.

**Filter.** `LPF Cutoff Frequency` (**2 bytes**, 0–255),
`LPF Cutoff Velocity Sensitivity`, `LPF Resonance`, and an independent
`HPF Cutoff Frequency` (2 bytes). No filter envelope.

**Amplitude envelope.** `AEG Attack Time`, `AEG Decay 1 Time`,
`AEG Decay 2 Time` (0–126 plus `hold`), `AEG Decay 1 Level`. Note the `hold`
value at the top of Decay 2 — a sustaining drum key.

**Key EQ.** `EQ Type` (EQ L/H, P.EQ, Boost 6/12/18, thru), `EQ Q`, and
frequency/gain for two bands. Frequency ranges depend on EQ type
(`EQ L/H: 46–182`, `P.EQ: 83–251`), so the UI must re-scale when type changes.

**Routing.** `Insertion Effect Switch` (thru / insA / insB),
`Output Select`, `Reverb Send Level`, `Chorus Send Level` — all per key.

## UI implications

* Present a **key map** (73 keys, C0–C6), not an element list. Show which keys
  are assigned (`47 ee 00`) and let the user click one to edit.
* Surface `Alternate Group` and `Receive Note Off` on the key page — they are
  what make a kit behave like a kit.
* 73 keys × 42 bytes ≈ 3 kB. Load by bulk dump; edit by Parameter Change.
* Drum blocks only answer when a Drum voice is loaded — see `state-sync.md`.
