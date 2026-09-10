# MOTIF-RACK XS — SysEx Protocol

**Authority:** *MOTIF-RACK XS Data List*, "MIDI Data Format" (pp. 58–61) and
"MIDI Data Table" (pp. 62–76). Every statement below is either quoted from that
document or marked as **[verified]** / **[discrepancy]** against the physical
unit.

## Identifiers

| Field | Value |
|---|---|
| Manufacturer ID | `43` (Yamaha) |
| Model ID | `7F 03` (two bytes) |
| Device Number | `n`, 0–15. Front panel shows it **1-based**; the wire value is 0-based. |

**[verified]** On the reference unit the panel reads *Device Number 1* and the
wire value is `0`. A mismatched device number produces **no reply at all**, and
the unit displays a "device number mismatch" message.

## Message framing

Yamaha packs the device number into the high nibble of byte 2, which is what
distinguishes the five message types:

| Message | Framing | Checksum |
|---|---|---|
| Parameter Change | `F0 43 1n 7F 03 aH aM aL dd… F7` | **no** |
| Bulk Dump | `F0 43 0n 7F 03 bH bL aH aM aL dd… cc F7` | **yes** |
| Dump Request | `F0 43 2n 7F 03 aH aM aL F7` | — |
| Parameter Request | `F0 43 3n 7F 03 aH aM aL F7` | — |

The address is always three bytes: High, Mid, Low. Byte counts are two bytes.

A Parameter Request is answered with a **Parameter Change** carrying the same
address. A Dump Request is answered with a **Bulk Dump**.

### Checksum

> "The Check sum is the value that results in a value of 0 for the lower 7 bits
> when the Byte Count, Start Address, Data and Check sum itself are added."

```
cc = (0x80 - ((bH + bL + aH + aM + aL + Σdd) & 0x7F)) & 0x7F
```

Parameter Change carries **no** checksum. Only Bulk Dump does. Adding one to a
Parameter Change is a common way to make the unit ignore the message silently.

## Identity

```
Request (receive only):  F0 7E 0n 06 01 F7
Reply   (transmit only): F0 7E 7F 06 02 43 00 41 39 06 mm 00 00 7F F7
                         version mm = (version - 1.0) * 10
```

**[discrepancy]** The Data List says of the request that "this instrument
receives under *omni*". It does not. On the reference unit:

| Request | Result |
|---|---|
| `F0 7E 00 06 01 F7` (device 0) | replies |
| `F0 7E 7F 06 01 F7` (omni `7F`) | **no reply** |

Device discovery must therefore sweep device numbers `0`–`15` rather than
relying on a single broadcast. Reply observed: `43 00 41 39 06 00` → Yamaha,
family `00 41`, member `39 06`, version 1.0.

## Value encoding

Data bytes are 7-bit. Values wider than 7 bits are split, in one of two ways,
and the Data List signals which by how it names the rows:

* **`msb_lsb`** — 7 bits per byte. `value = (d0 << 7) | d1`.
  Named `<param> MSB` / `<param> LSB`.
  **[verified]** `ARP SF1 Assign Type` (`40 30 45`, size 2) round-trips
  1, 3861 and 6633 exactly.
* **`nibbles`** — 4 bits per byte, flagged by a note of the form
  "1st bit 3-0 → bit 15-12". Used by e.g. Master Tune (`00 00 02`, size 4).

Signed parameters are offset-encoded, typically with `0x40` as zero
(e.g. Master Note Shift `28`–`58` = −24…+24 semitones).

## Request behaviour

**[verified]** A Parameter Request aimed at an address that is not the *start*
of a parameter returns **nothing at all** — no error, no empty reply. Reading
`00 00 02` (4-byte Master Tune) returns 4 data bytes; reading `00 00 03`,
`04` or `05` returns silence.

Three different conditions are therefore indistinguishable from the wire:

1. the address is `reserved`;
2. the address is mid-parameter;
3. the address belongs to a block that is not valid in the current mode.

A reader must know parameter sizes up front and must track the device mode.
Never discover the map by probing.

## Mode gating

The Data List's `[SW4]` note says the edit buffer "will be transmitted/received
only in the corresponding mode". **[verified]** — this is strict:

| Block | Responds when |
|---|---|
| `40/41/42` Normal Voice | a **Normal** voice is loaded |
| `46/47` Drum Voice | a **Drum** voice is loaded |
| `36/37/38` Multi | the unit is in **Multi** mode |
| `00` System | always |

Selecting a Drum voice makes the whole Normal Voice map go silent, and vice
versa. See `state-sync.md`.

## Gotchas that cost real time

1. **Bank Select and Program Change receive can be switched off.**
   `00 00 14` and `00 00 15` (both `00 = off`, `01 = on`). **[verified]** the
   reference unit shipped with **both off**, so every patch change was silently
   ignored — no error, the synth simply does not move. Check these at connect.
2. **Mode Change lives at `00 01` of its block, not `00 00`.** The p62 memory
   map lists MODE CHANGE at top address `0A 00 00`; the parameter table on p64
   puts the parameter at `0A 00 01`. **[verified]** `0A 00 01` reads back;
   `0A 00 00` does not. The memory-map entry is a *block base*, not a parameter.
3. **[discrepancy] Mode Change appears to be inert over SysEx.** Writes of
   every documented value (`00`–`05`) to both `0A 00 00` and `0A 00 01` were
   accepted without error and did not change the mode on the reference unit.
   Entering Multi mode currently requires the front panel. Unresolved.
4. **[discrepancy] Multi Part `Program Number` is 0-based, not "1 - 128".**
   `37 pp 03` is documented as ranging 1-128; the hardware reports 0-based
   values matching MIDI Program Change exactly. Do not convert. See
   `multi-architecture.md`.
5. **Active Sensing (`FE`) floods the input.** The unit sends it continuously;
   strip `FE` and `F8` before SysEx reassembly.

## Clock

**MIDI Sync** (`00 05 0B`, hardware-discovered) decides whether arpeggios follow
the DAW or the rack's own clock:

| Value | Meaning |
|---|---|
| `0` | internal -- ignores incoming clock |
| `1` | external -- follows incoming MIDI clock **only**; silent without it |
| `2` | auto -- follows incoming clock when present, internal otherwise |

For DAW use prefer `auto`. On `external` the arpeggiators produce nothing at all
unless the host is actively sending MIDI clock, which reads as "the arpeggiator
is broken".

## Rate limiting

`00 00 1F` *Bulk Interval* (0–30 ms) governs the gap the unit wants between
bulk blocks. Parameter Changes need no acknowledgement and are fire-and-forget,
but the unit's input buffer is finite — see `state-sync.md` for the send policy.
