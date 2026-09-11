#pragma once
#include <cstddef>

/// The PERFORM macros, defined once.
///
/// These are addressed three ways -- the knobs in the editor, the host
/// parameters a controller like Push drives, and the Multi Part parameters on
/// the rack -- and all three have to agree. Keeping separate lists in the UI
/// and the processor let them drift, and let the same rack parameter be written
/// by two paths that did not know about each other.
namespace macros {

struct Macro {
    const char* id;        ///< host parameter id, stable: do not change
    const char* label;     ///< shown in the host's automation list
    const char* shortLabel;///< above the knob, where space is tight
    const char* parameter; ///< motifxs parameter id
};

inline constexpr Macro kAll[] = {
    {"volume",  "Volume",      "VOLUME",  "multi_part_volume"},
    {"pan",     "Pan",         "PAN",     "multi_part_pan"},
    {"cutoff",  "Cutoff",      "CUTOFF",  "multi_part_filter_cutoff_frequency"},
    {"reso",    "Resonance",   "RESO",    "multi_part_filter_resonance_width"},
    {"attack",  "Attack",      "ATTACK",  "multi_part_aeg_attack_time"},
    {"decay",   "Decay",       "DECAY",   "multi_part_aeg_decay_time"},
    {"release", "Release",     "RELEASE", "multi_part_aeg_release_time"},
    {"reverb",  "Reverb Send", "REVERB",  "multi_part_reverb_send"},
    {"chorus",  "Chorus Send", "CHORUS",  "multi_part_chorus_send"},
};

inline constexpr std::size_t kCount = sizeof(kAll) / sizeof(kAll[0]);

}  // namespace macros
