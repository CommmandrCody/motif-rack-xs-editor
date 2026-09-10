# Forwarding the Arpeggiator to Another Instrument

The MOTIF-RACK XS can transmit its arpeggiator output as ordinary MIDI, so an
arp playing on the Motif can drive a second sound engine -- a Roland INTEGRA-7's
SuperNATURAL tones, say -- while the Motif plays its own voice, or silently.

**[verified]** on real hardware: with ARP MIDI Out armed, a single held note
produced 54 note-ons plus programmed controller data (`B0 54 xx`, CC 84) in four
seconds, all forwarded to an INTEGRA-7 endpoint.

## What the rack sends

Two switches gate it, and both must be on:

| Address | Parameter | Notes |
|---|---|---|
| `00 00 28` | ARP MIDI Out Switch (System) | global gate |
| `00 00 29` | ARP MIDI Out Channel (System) | |
| `38 pp 01` | **ARP MIDI Out Switch (per part)** | **off by default** |
| `38 pp 02` | ARP MIDI Out Channel (per part) | 1-16, or `rcv-ch` (`0x10`) |

The per-part switch is the one that catches people out: it ships **off**, so the
arpeggio is audible but transmits nothing and the routing looks broken.

What comes out is the *arpeggiated* phrase, not the key you played -- the notes
the arpeggiator generates, with its velocity and gate-time treatment applied,
plus any Control Change or Pitch Bend programmed into that arp type. Arps in the
`Cntr` category transmit controller data and no notes at all.

## What the software does

`Device::setThru(destination)` relays inbound **channel messages** to another
CoreMIDI destination.

* SysEx is **never** forwarded. The editor's own parameter traffic must not
  reach the other instrument.
* System realtime (clock, active sensing) is never forwarded; clock is the
  host's job, and the rack floods `FE`.
* `setThruChannel(n)` rewrites the channel, so several parts arping on
  different channels can be collapsed onto one target, or left alone with `-1`.
* `silenceThru()` sends All Notes Off / All Sound Off to the target on all 16
  channels. `panic()` calls it, so the panic button stops the *other* device too
  -- otherwise a forwarded note-on outlives the arp that produced it.

Forwarding happens on the CoreMIDI read thread, so it never touches a UI or
audio thread.

The rack's channel messages were previously discarded: `SysExReassembler` drops
anything outside `F0..F7`. `ChannelMessageParser` extracts them properly,
including running status, which the rack does use.

## Using it

CLI:

```sh
motifxs list-midi                 # find the destination name
motifxs thru "INTEGRA-7" 30       # forward for 30 seconds
motifxs thru "INTEGRA-7" 30 5     # ...and force everything to channel 5
```

App: pick the target in the **THRU** box in the header, and arm **OUT** on the
part whose arp you want to send. `ARP` must also be on, or there is no arp.

## On the receiving instrument

Nothing here configures the target. On the INTEGRA-7 you still need a part
listening on the channel the Motif is sending (per-part `38 pp 02`, or whatever
`setThruChannel` rewrote it to) with a tone loaded.

Worth knowing: a Motif arp written for a drum kit sends General-MIDI-ish drum
note numbers, which land as pitched notes on a melodic SuperNATURAL tone. Melodic
arp categories (`ApKb`, `Bass`, `GtPl`, `Seq`, `Lead`, `Chord`) travel far better
than `DrPc` unless the target is a drum kit.

## Feedback

The thru list deliberately excludes the rack's own ports. Forwarding the Motif
to itself is a loop, and with an arpeggiator in the path it is a fast one.
