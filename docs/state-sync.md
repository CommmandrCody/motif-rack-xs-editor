# State and Synchronisation

How the editor keeps its picture of the rack honest. Everything marked
**[verified]** was established on the physical unit.

## The four facts that shape this design

1. **Parameter Change is not acknowledged.** There is no ack, no nak, no error.
   A write is fire-and-forget and must be treated optimistically.
2. **[verified] Silence is ambiguous.** A Parameter Request that gets no reply
   means *reserved*, *mid-parameter*, **or** *wrong mode* — indistinguishable.
   So the software must know the map and the mode; it may never discover them
   by probing.
3. **[verified] Blocks are mode-gated.** Normal Voice (`40/41/42`) answers only
   with a Normal voice loaded; Drum (`46/47`) only with a Drum voice; Multi
   (`36/37/38`) only in Multi mode.
4. **[verified] The unit can be configured to ignore you.**
   `00 00 14` (Bank Select receive) and `00 00 15` (Program Change receive) both
   shipped **off** on the reference unit — patch selection failed silently.

## Connect sequence

```
1  enumerate CoreMIDI endpoints, find "YAMAHA MOTIF-R XS Port1"
2  sweep device numbers 0..15 with Identity Request  F0 7E 0n 06 01 F7
     -> confirms presence, device number and firmware version
     (do NOT rely on the 7F omni form; it does not reply)
3  read System block 00 00 00 .. 00 00 33
4  assert 00 00 14 == 01 and 00 00 15 == 01
     if not: warn the user, offer to enable. Without these, patch
     selection is a no-op with no error.
4b assert 00 00 0C == 00 (Layer 1-4 Parts off) for multi-timbral use
     if on, Parts 1-4 all receive on the Basic Receive Channel and
     their per-part Receive Channel is overridden.
5  read 0A 00 01 -> current mode
6  probe one address per candidate block to establish which edit
   buffer is live (40 00 00 vs 46 00 00)
7  load the live edit buffer by bulk dump
```

Step 4 is not optional. Skipping it produces the single most confusing failure
mode this device has.

## Ports

The unit exposes four USB-MIDI ports. **[verified]** only **Port1** carries
SysEx and Active Sensing; Ports 2–4 were silent to Identity Request and
Parameter Request. Use Port1 for all control traffic. Ports 2–4 are for
multi-port note routing.

## Feedback loops

The unit echoes parameter changes made from its front panel, and will happily
re-transmit what you send it if Soft MIDI Thru (`00 00 16`) is on.

Guards:
* **Check `00 00 16` at connect**; recommend off for editor use.
* **Echo suppression** — keep a short (~200 ms) table of `(address, value)`
  pairs recently written. Drop an inbound Parameter Change that matches. This
  is cheap and handles the common case.
* **Never re-send on receive.** Inbound changes update the model and the UI
  only; they never trigger an outbound write.

## Reading state

**Use bulk dump to load, Parameter Change to edit.** An 8-element Normal Voice
is ~1.4 kB; requesting it one parameter at a time is ~500 round-trips. A Dump
Request (`F0 43 2n 7F 03 aH aM aL F7`) returns the whole block with a checksum.

Respect `00 00 1F` *Bulk Interval* (0–30 ms) between blocks.

Verify every bulk dump's checksum and discard the block on mismatch — a
corrupted dump silently poisoning the model is worse than a failed load.

## Writing state

* Parameter Change per edit; no read-back per edit.
* **Throttle continuous controls.** A knob drag generates far more messages
  than the unit's input buffer wants. Coalesce to the latest value per address
  and emit at ~50–100 Hz. Two-byte parameters (cutoff) must be sent whole.
* **Re-read after risky operations** (voice change, mode change, bulk load)
  rather than after every edit.

## Reconciling hardware-side edits

The front panel transmits Parameter Changes when the user turns a knob, gated
by `[SW3]` device number. Treat inbound Parameter Change as authoritative: it
is what the hardware actually did.

There is no "tell me everything that changed" query. After any operation that
could have moved a lot of state at once (voice change, mode change), re-read
the affected block wholesale rather than trusting incremental messages.

## Capturing the whole Multi

**[verified]** The rack will hand over its entire Multi in one exchange.
Requesting a bulk dump at the **Bulk Header for the Multi edit buffer**
(`0E 5F 00`) makes it stream header, every block, and footer:

| Blocks | Payload |
|---|---|
| Multi Common, Reverb, Chorus, Master EQ, Master Effect, Multi Arp | 81, 37, 38, 20, 36, 13 |
| Part 1-16 | 61 each |
| Part 1-16 arpeggio | 66 each |
| Audio In Part | 8 |
| **41 messages, 39 data blocks** | **2265 bytes** |

Every payload matches the documented Bulk Dump Block byte counts exactly.

This is why the Data List's aside -- *"To execute 1 Voice bulk dump request,
designate its corresponding Bulk Header address"* -- matters more than it
looks. **[discrepancy]** A dump request aimed at an individual block address
(`37 00 00`, `36 00 00`, `40 00 00`) returns **nothing at all**, even though
those addresses are listed in the Bulk Dump Block table and answer Parameter
Requests perfectly well. Only the System blocks (`00 00 00`, `00 20 00`) dump
individually. Bulk dumps of voice and multi data must go through the Bulk
Header.

Capturing verbatim beats a curated parameter list: nothing is quietly omitted,
and it restores as the same blocks, so the ordering hazard below does not
arise. Restoring sends the blocks back with a small inter-block delay
(the rack's Bulk Interval, `00 00 1F`); too fast and blocks are dropped
silently.

Round-trip **[verified]** on hardware: capture, change the part's voice and
volume, restore -- both come back exactly.

`motifxs save <file>` / `motifxs load <file>` / `motifxs show <file>`, and the
SAVE / LOAD buttons in the app.

## What the DAW should save

The bulk capture above supersedes this list for anything in the Multi; it is
kept because the plugin still needs to know what belongs in project state, and
because voice-level edits live outside the Multi.

Start narrow and grow. Phase 1 — enough to recreate the setup:

```
device        : device number, firmware version, port name
mode          : Voice | Multi
per part 0-15 : bank MSB/LSB, program, receive channel,
                volume, pan, reverb send, chorus send,
                note shift, mono/poly
per part arp  : SF1-5 assign types, SF select, switch, hold,
                octave range, quantize, velocity/gate rate
edits         : every parameter the plugin itself wrote, as
                (scope, address, value) triples
```

Store **addresses and values**, not UI state. The address map is stable across
firmware; a UI layout is not.

### Restore order matters

```
1  set mode
2  per part: bank select + program change   (loads the voice)
3  wait for the voice to settle
4  per part: part parameters                (volume, pan, sends…)
5  per part: arp settings
6  replay explicitly-edited voice parameters
```

Steps 2 and 4 must not be reordered. If `Param. with Voice` (`37 pp 19`) is on,
a voice change overwrites part parameters — so part settings have to be written
*after* the program change, not before. The same applies to `Voice with ARP`
(`37 pp 1A`) and step 5.

Do not attempt full-rack capture in phase 1. Bulk-state capture of all user
voices is a librarian feature and a separate project.

## Open question

Entering Multi mode over SysEx does not work on the reference unit. Until that
is resolved, DAW recall of a Multi requires the unit to already be in Multi
mode, and the plugin should detect and report that rather than failing quietly.
