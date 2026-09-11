# Motif Rack XS Modern Editor

A native macOS editor/controller for the Yamaha MOTIF-RACK XS, targeting
Apple Silicon and Ableton Live. The hardware stays the sound engine; this makes
it feel like a modern synth module inside a DAW.

**Status: complete and in use.** A CLI, a standalone GUI and a VST3/AU plugin,
all driving a physical MOTIF-RACK XS. The plugin passes Apple's `auval`
validation and stores the rack's whole rig -- Multi *and* all 16 part voices --
in the DAW project, captured automatically so a saved session comes back.

## Requirements

| | |
|---|---|
| Platform | **macOS only** for now, universal (Apple Silicon + Intel) |
| Hardware | a Yamaha MOTIF-RACK XS, connected by USB |
| Build | CMake 3.21+, a C++20 compiler; JUCE for the app and plugin |

**Windows is not supported yet**, and the reason is narrow: the whole project is
portable C++20 except `motif-xs-core/src/device.cpp`, which is CoreMIDI. A
Windows build needs a WinMM or WinRT MIDI implementation behind the same
`Device` interface -- one file, no changes anywhere else. That is the single
most useful contribution anyone with a Windows machine could make.

The binaries are **unsigned and not notarized**, so on any Mac other than the
one that built them, Gatekeeper will refuse to open them until you allow it in
System Settings → Privacy & Security. Building from source avoids that.

## Layout

```
motif-xs-core/     reusable C++20 core -- no JUCE, no UI, no VST
motif-xs-cli/      motifxs command line tool (milestone 1)
motif-xs-app/      standalone GUI            (milestone 2)
motif-xs-plugin/   VST3 / AU -- builds, passes auval
data/              machine-readable catalogs extracted from Yamaha docs
docs/              protocol and architecture documentation
tools/             extraction and code-generation scripts
reference/         Yamaha PDFs (the implementation authority)
tests/             core tests, no hardware needed
```

## Build

> **After a macOS major upgrade**, Apple's `xcode-select` shim can end up
> pointing at an Xcode that is too old for the new OS, and then `git`, `clang`
> and `xcrun` all fail with `Symbol not found: _XPCTypeBool`. The compiler
> itself is fine; only the wrapper is broken. Either point the shim at the
> standalone tools once:
>
> ```sh
> sudo xcode-select -s /Library/Developer/CommandLineTools
> ```
>
> or set `DEVELOPER_DIR=/Library/Developer/CommandLineTools` per shell, which
> needs no sudo. Updating Xcode from the App Store also fixes it.

The CLI, core and tests need nothing but CMake 3.21+ and a C++20 compiler:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/motif-xs-tests
```

The standalone GUI additionally needs JUCE, which is not vendored:

```sh
git clone --depth 1 --branch 8.0.4 https://github.com/juce-framework/JUCE.git external/JUCE
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target motif-xs-app -j8
open "build/motif-xs-app_artefacts/Release/Motif Rack XS.app"
```

CMake skips the app target if `external/JUCE` is absent, so the core and CLI
always build.

Binaries are **universal (arm64 + x86_64)**. Note that
`CMAKE_OSX_ARCHITECTURES` must be set *before* `project()` -- afterwards the
cache entry already exists and a non-`FORCE` `set()` is silently ignored, at
which point CMake builds for its own architecture. A Homebrew CMake running
under Rosetta will then quietly produce Intel binaries on an Apple Silicon Mac.

## The app

Auto-connects to MOTIF-RACK XS Port1 and reads the rack's live state.

* 16-part strip with each part's resolved voice name
* Voice browser: 1217 voices in collapsible categories, searchable, bank filter
* Arp browser: 6633 types filtered by text, category, metre and tempo range
* Nine PERFORM macros (volume, pan, cutoff, reso, attack, decay, release,
  reverb, chorus) bound to Multi Part offsets, so they shift all eight elements
  together the way the rack's own knobs do
* **SAVE / LOAD** -- capture the whole rig to a file and restore it: the Multi
  (every part, arp and effect) *and* all 16 part voices, so voice-level edits
  survive too. 423 blocks, ~29 kB, about 5 seconds. See `docs/state-sync.md`
* **SAVE PATCH / LOAD PATCH** -- an edited voice lives only in a part's edit
  buffer and dies at the next patch change. Save it whole as a file and put it
  back on any part. The rack manages 16 of these (Mixing Voices) and only within
  one Multi; as files they are unlimited
* **THRU** -- forward the rack's arpeggiator to another instrument (an
  INTEGRA-7, say); see `docs/midi-routing.md`
* **ARP -> DAW** (plugin only) -- relay the arpeggiator into the host track as
  MIDI, so the phrase can be recorded without an IAC bus
* **PANIC** -- all notes off plus ARP switch/hold cleared on all 16 parts,
  and All Notes Off to the thru target too
* **SCOPE** -- a waveform and log-spaced spectrum of the rack's audio. In the
  plugin it analyses whatever reaches the track. In the standalone app, pick the
  interface *and the stereo pair* the rack is on -- a rack on an 18-input desk
  is rarely on channels 1/2. macOS asks for microphone permission, which it
  requires even for a line input. When there is nothing to draw the page says
  why rather than showing a flat line

## Use

```sh
./build/motifxs list-midi                  # find the rack
./build/motifxs identify                   # device number and firmware
./build/motifxs status                     # mode, live edit buffer, voice name
./build/motifxs voices "concert grand"     # search 1217 factory voices
./build/motifxs patch PRE1 1               # select a voice (1-based)
./build/motifxs patch-name "Rock Grand"    # select by name
./build/motifxs arp-list cat=DrPc tempo=120-128 house
./build/motifxs arp 1 3861                 # assign an arpeggio to slot SF1
./build/motifxs get  normal_voice_element_filter_cutoff_frequency 0
./build/motifxs set  normal_voice_element_filter_cutoff_frequency 80 0
./build/motifxs read 40 00 00              # raw address
./build/motifxs save live-set.motifxs      # capture the whole Multi
./build/motifxs show live-set.motifxs      # what a saved file holds
./build/motifxs load live-set.motifxs      # restore it
./build/motifxs voice-save 1 my-organ.motifxs   # one part's voice, edits and all
./build/motifxs voice-load 5 my-organ.motifxs   # ...onto any part
./build/motifxs thru "INTEGRA-7" 30        # forward the arp to another device
./build/motifxs panic                      # all notes off, arpeggiators off
./build/motifxs dump-state                 # read the live edit buffer
```

## One client at a time

**Only one of the app, the plugin or the CLI may talk to the rack at once.**
This is enforced, not advised: opening the rack takes an exclusive lock, and a
second client is refused with a message naming the first.

The reason is that the rack has a single MIDI port with no arbitration, and
CoreMIDI merges the output of every client that opens a destination. A second
client's Parameter Requests land *inside* the first one's bulk transfer,
splitting the stream -- the rack rejects the sequence with "illegal bulk data"
on its display and the transfer silently half-applies. Leaving the standalone
app open while the plugin restored a project did exactly this.

The lock is advisory on a file, so the operating system releases it if a process
dies; a crash cannot leave the rack permanently locked.

## Using it in Ableton Live

One track does everything. The plugin declares itself an **audio effect** (VST3
sub-category `Fx`), so it can sit *after* an External Instrument device:

```
MIDI track
├─ MIDI clips
├─ External Instrument      MIDI To:    YAMAHA MOTIF-R XS Port1, channel N
│                           Audio From: the interface input the rack returns on
└─ Motif Rack XS            <- editor, state recall, and the SCOPE sees the audio
```

Being in the audio chain costs the plugin nothing: it ignores host MIDI
entirely and talks to the rack over its own CoreMIDI connection, for the reasons
in `docs/vst-architecture.md`. Put it before the External Instrument and it
still controls the rack -- it just sees no audio, so the SCOPE page stays empty
and says so.

**Recording the arpeggiator** is the one thing that wants a second track,
because a plugin in the audio chain cannot record its MIDI output to its own
track. Arm **ARP -> DAW** in the plugin, then on a second MIDI track set
*MIDI From* to that track and the **Motif Rack XS** plugin, and record-arm it.
The part also needs `ARP` and `OUT` enabled or the rack transmits nothing.

## Data

Extracted from Yamaha's published documentation, not from any binary.

| File | Contents |
|---|---|
| `data/parameters.json` | 1136 parameters (920 non-reserved) across 10 scopes |
| `data/voices.json` | 1217 factory voices with bank/MSB/LSB/program and categories |
| `data/arpeggios.json` | 6633 arpeggio types with category, tempo, metre, flags |

Regenerate with:

```sh
python3 tools/extract_parameters.py   # raw table rows
python3 tools/build_parameters.py     # -> data/parameters.json
python3 tools/extract_voices.py       # -> data/voices.json
python3 tools/extract_arpeggios.py    # -> data/arpeggios.json
python3 tools/gen_tables.py           # -> C++ tables
```

## Verification

The address map is not trusted on paper. Every concrete address was read from a
physical unit and the reply length compared to the documented size:

| Scope | Addresses exact | Size mismatches | Unexplained silence |
|---|---|---|---|
| System | 73 | 0 | 0 |
| Normal Voice Common | 236 | 0 | 0 |
| Normal Voice Element | 130 | 0 | 0 |
| Drum Voice Common | 186 | 0 | 0 |
| Drum Voice Key | 32 | 0 | 0 |
| Multi Common | 106 | 0 | 0 |
| Multi Part | 96 | 0 | 0 |
| **Total** | **859** | **0** | **0** |

Also verified end to end: 10/10 randomly sampled voice names read back from the
device match the catalog; arpeggio numbers 1, 3861 and 6633 round-trip through
the 2-byte encoding; and all 16 Multi parts resolve their bank/program to real
voice names.

## Things the documentation gets wrong

Found the hard way; all three are in `docs/protocol.md`.

1. **Identity Request is not omni.** The Data List says the unit "receives under
   omni", but `F0 7E 7F 06 01 F7` gets no reply. Only the explicit device-number
   form works, so discovery must sweep 0–15.
2. **Mode Change is at `0A 00 01`,** not the `0A 00 00` printed in the p62
   memory map — that is the block base, not the parameter.
3. **Bank Select / Program Change receive can be off** (`00 00 14`, `00 00 15`).
   The reference unit shipped with both off, so every patch change was ignored
   with no error at all. `motifxs` warns about this.
4. **Multi Part `Program Number` is 0-based**, not the documented "1 - 128".
   Converting "to be safe" is what causes the off-by-one. Recorded in
   `data/parameters_corrections.json`.
5. **The Audio In Part's mid byte is fixed at `41`**, though the table prints it
   as the part variable `pp`.

## Documented gap, closed by experiment

Yamaha lists Sequencer Setup (`00 05 00`) as a 22-byte bulk block but publishes
no parameter table for it. Probing all 22 addresses found exactly two that
answer: `00 05 0B` **MIDI Sync** and `00 05 0C` **MIDI Clock Out**, confirmed by
panel reading, value-range fingerprinting and the Quick Setup block's ordering.
They live in `data/parameters_discovered.json`, flagged `source: "hardware"`.

## Known issue

**Entering Multi mode over SysEx does not work** on the reference unit. Writes
of every documented value to both candidate addresses are accepted and do
nothing; the front panel `[MULTI]` button works. The address map is now fully
validated, but unattended DAW recall will need this solved -- or the rack left
in Multi mode, which `Power on Mode = multi` makes permanent.

## Documentation

`docs/protocol.md`, `address-map.md`, `voice-architecture.md`,
`drum-architecture.md`, `arp-architecture.md`, `multi-architecture.md`,
`state-sync.md`, `vst-architecture.md`, `midi-routing.md`.

## Licence

**AGPL-3.0**, because this links JUCE, which is dual-licensed AGPLv3 or
commercial. If you build on this, your work inherits those terms.

The VST3 SDK is MIT since late 2025, so it imposes nothing.

### About the data

`data/*.json` holds facts extracted from Yamaha's published documentation:
parameter addresses, sizes and ranges, and the factory voice, arpeggio and
waveform lists. The PDFs themselves are **not** redistributed here -- they are
in `.gitignore`. Every file under `data/` can be regenerated from your own copy
of Yamaha's documents with the scripts in `tools/`, which is also how you would
check them.

MOTIF-RACK XS, MOTIF and Yamaha are trademarks of Yamaha Corporation. This
project is not affiliated with or endorsed by Yamaha.

## Contributing

The most useful thing anyone could add is **another device**. The parameter
model is data-driven -- a device is a table, not a code path -- so a MOTIF XS or
XF keyboard is mostly a matter of extracting its Data List and confirming the
model ID and address map against hardware. The extraction tools in `tools/` are
written to be pointed at a different PDF.

If you do that, please keep the discipline the rest of the project uses: read
every address back off the real instrument before writing it down, and mark
anything the documentation gets wrong. It gets things wrong more than you would
expect -- see the list above.

## Approach

Published specification → hardware experiment → captured behaviour →
implementation. No reverse engineering of Yamaha binaries. Yamaha's PDFs in
`reference/` are the implementation authority; every claim in `docs/` is either
cited to them or marked as verified/contradicted on hardware.
