# Plugin Architecture

## The finding that decides the design

**VST3 SysEx output is not dependable, so the plugin must own its own CoreMIDI
connection.**

Evidence:

* Steinberg's position is that VST3 is an audio API and MIDI output is not an
  intended use.
* VST3 SysEx output is widely reported not to work; note events through the
  same output bus do.
* JUCE's VST3 wrapper strips the `F0`/`F7` framing, so SysEx from a JUCE VST3
  fails in most third-party hosts, Cubase and Reaper included.

Since essentially *everything* this device needs is SysEx, routing control
traffic through the VST3 event path would mean depending on the one part of the
API that is least reliable. It also does not survive the plugin being used in a
host that gives the plugin no MIDI output at all.

So:

```
        ┌──────────────────────────────────────────┐
        │ Host (Ableton Live)                      │
        │   audio: silent passthrough              │
        │   automation: normalised parameters      │
        │   state:  getState / setState            │
        └───────────────┬──────────────────────────┘
                        │  (no SysEx crosses this line)
        ┌───────────────┴──────────────────────────┐
        │ motif-xs-plugin (JUCE)                   │
        │   AudioProcessor, editor, parameter map  │
        └───────────────┬──────────────────────────┘
                        │  lock-free FIFO
        ┌───────────────┴──────────────────────────┐
        │ motif-xs-core                            │
        │   MIDI worker thread  ->  CoreMIDI       │
        └───────────────┬──────────────────────────┘
                        │  USB
                    MOTIF-RACK XS  Port1
```

The plugin talks to the hardware directly through CoreMIDI on its own thread.
The host is used for what it is good at: automation, project state, and UI
hosting.

### Consequences

* Works identically in every host, and in the standalone app — one code path.
* The plugin needs no MIDI output bus at all.
* The user selects the MIDI port in the plugin UI, not in the host's routing.
* Two instances must not both open the port for writing — see below.

## Threading

**No CoreMIDI call may happen on the audio thread.** `processBlock` must not
allocate, lock, or send.

| Thread | Does |
|---|---|
| Audio (`processBlock`) | reads automation, pushes `(address, value)` into a lock-free FIFO. Nothing else. |
| MIDI worker | drains the FIFO, coalesces per address, encodes SysEx, sends via CoreMIDI, ~50–100 Hz |
| CoreMIDI read callback | parses inbound SysEx, hands complete messages to the model |
| Message/UI | reads the model, repaints |

Coalescing in the worker is what makes automation safe: a host sweeping a
cutoff parameter at audio rate must not produce one SysEx per sample.

## Parameters and automation

The device has 918 non-reserved parameters. Do **not** expose all of them as
automatable — hosts degrade badly and Live's device view becomes unusable.

Expose a curated automatable set (the PERFORM controls: per-part volume, pan,
cutoff, resonance, attack, decay, release, reverb/chorus send, arp on/off, arp
slot) and keep the deep editor parameters as plugin state only, not host
automation. This matches how the hardware's own knobs work.

Every automatable parameter is `0..1` normalised at the host boundary and
converted through the parameter model's range on the way out.

## Project state

`getStateInformation` writes the phase-1 state described in `state-sync.md`:
mode, per-part bank/program/part parameters, arp settings, and explicit edits
as `(scope, address, value)`.

Serialise **addresses**, not UI positions or parameter indices — index-based
schemes break the moment the parameter table is regenerated.

Include a schema version. On load, unknown addresses are skipped with a
warning rather than aborting the restore.

`setStateInformation` must not send MIDI directly: it populates the model and
enqueues a restore sequence on the worker thread, in the order given in
`state-sync.md`.

## Port contention

CoreMIDI allows multiple clients to open the same destination, so two plugin
instances *can* both write to Port1 and interleave nonsense.

Mitigation: a process-wide singleton device connection shared by all instances
in the host, with instances registering as observers. Two instances editing
different parts is legitimate and useful; two instances both driving the same
part is a user error worth warning about.

## Targets

* **VST3 first**, Apple Silicon native (arm64). Intel optional; build
  universal only if asked.
* **AU after** — JUCE gives it from the same code once the architecture works.
* **Standalone** falls out of the same JUCE project and is the fastest way to
  test without a host.
* The plugin produces no audio. Register it as an effect with a silent
  passthrough; Live will not host a plugin that declares no audio bus cleanly.

## Porting

Everything except `motif-xs-core/src/device.cpp` is portable C++20 -- the
parameter model, the catalogs, state capture, the worker, the whole UI. That
one file is CoreMIDI: endpoint enumeration, an input port with a read callback,
and `MIDISend`.

A Windows port is that file reimplemented against WinMM or WinRT MIDI behind the
same `Device` interface. Two details it must preserve, both learned the hard
way:

* **strip realtime bytes before SysEx reassembly.** The rack floods Active
  Sensing (`FE`), and it can land mid-message.
* **only accept a reply that arrived after the request was sent.** The rack
  transmits Parameter Changes of its own accord when its front panel is touched,
  and they are byte-identical to a reply. See `state-sync.md`.

## Build

CMake + JUCE as a submodule. The core is a plain C++20 static library with no
JUCE dependency, so it can be linked by the CLI, the plugin, and the tests
alike — and bound from another language later.
