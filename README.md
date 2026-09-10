# Motif Rack XS Modern Editor

A native macOS editor/controller for the Yamaha MOTIF-RACK XS, targeting
Apple Silicon and Ableton Live. The hardware stays the sound engine; this makes
it feel like a modern synth module inside a DAW.

**Status: milestone 1 complete** — a command-line proof of concept that talks to
real hardware, verified against a physical MOTIF-RACK XS.

## Layout

```
motif-xs-core/     reusable C++20 core -- no JUCE, no UI, no VST
motif-xs-cli/      motifxs command line tool (milestone 1)
motif-xs-app/      standalone GUI            (milestone 2)
motif-xs-plugin/   VST3 / AU                 (milestone 3)
data/              machine-readable catalogs extracted from Yamaha docs
docs/              protocol and architecture documentation
tools/             extraction and code-generation scripts
reference/         Yamaha PDFs (the implementation authority)
tests/             core tests, no hardware needed
```

## Build

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
always build. Apple Silicon native (arm64).

## The app

Auto-connects to MOTIF-RACK XS Port1 and reads the rack's live state.

* 16-part strip with each part's resolved voice name
* Voice browser: 1217 voices in collapsible categories, searchable, bank filter
* Arp browser: 6633 types filtered by text, category, metre and tempo range
* Nine PERFORM macros (volume, pan, cutoff, reso, attack, decay, release,
  reverb, chorus) bound to Multi Part offsets, so they shift all eight elements
  together the way the rack's own knobs do
* **PANIC** -- all notes off plus ARP switch/hold cleared on all 16 parts

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
./build/motifxs dump-state                 # read the live edit buffer
```

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
`state-sync.md`, `vst-architecture.md`.

## Approach

Published specification → hardware experiment → captured behaviour →
implementation. No reverse engineering of Yamaha binaries. Yamaha's PDFs in
`reference/` are the implementation authority; every claim in `docs/` is either
cited to them or marked as verified/contradicted on hardware.
