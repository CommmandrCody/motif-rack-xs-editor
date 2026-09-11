// Capturing and restoring the rack's working state.
//
// A DAW project needs the Motif's setup back, not just the notes that were
// played into it. The rack will hand over its entire Multi in one exchange:
// requesting the Bulk Header for the Multi edit buffer (0E 5F 00) makes it
// send the header, every block, and the footer -- Multi common, effects, all
// 16 parts and all 16 part arpeggios, about 2.7 kB.
//
// That is captured verbatim rather than as a curated parameter list, so
// nothing is quietly left out, and it restores as the same blocks. See
// docs/state-sync.md.
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "motifxs/device.hpp"

namespace motifxs {

/// The Bulk Header for the Multi edit buffer. Requesting a dump at this
/// address returns the whole Multi rather than a single block.
inline constexpr Address kMultiEditBufferHeader{0x0E, 0x5F, 0x00};
inline constexpr Address kMultiEditBufferFooter{0x0F, 0x5F, 0x00};

/// Bulk Header for one Multi Part's *voice* edit buffer; the low byte is the
/// part, 0-15. Requesting a dump here returns that part's whole voice: Common
/// plus all eight Elements for a Normal Voice, or Common plus the 73 keys for a
/// Drum Voice.
///
/// This is what makes voice-level edits recoverable. A Multi stores each part's
/// bank and program -- a *reference* to a patch -- so a Multi dump alone brings
/// back the stored patch, not the edits made to it. It is also addressed per
/// part, unlike Parameter Requests to 40/41/42, which only ever answer for
/// whichever part the rack's front panel has selected.
[[nodiscard]] inline constexpr Address partVoiceHeader(int part) {
    return {0x0E, 0x30, std::uint8_t(part & 0x0F)};
}
[[nodiscard]] inline constexpr Address partVoiceFooter(int part) {
    return {0x0F, 0x30, std::uint8_t(part & 0x0F)};
}
inline constexpr int kParts = 16;

struct State {
    int schemaVersion{1};
    std::string device{"Yamaha MOTIF-RACK XS"};
    std::uint8_t deviceNumber{};
    float firmware{};
    std::string capturedAt;          ///< ISO-8601, informational only
    std::string note;                ///< free text, e.g. the Live set name
    std::vector<Bytes> messages;     ///< raw SysEx, header .. footer inclusive

    [[nodiscard]] bool empty() const { return messages.empty(); }
    /// Total payload bytes, excluding SysEx framing.
    [[nodiscard]] std::size_t payloadBytes() const;
    /// A short human summary, e.g. "39 blocks, 16 parts".
    [[nodiscard]] std::string summary() const;
    /// Empty if this state can actually be put back on a rack; otherwise what
    /// is wrong with it. A capture that is missing blocks is worse than no
    /// capture at all: it saves into the project, replaces the good one, and is
    /// only discovered when the project is reopened and the rack rejects it.
    [[nodiscard]] std::string problem() const;
};

/// What a capture should include.
enum class CaptureScope {
    MultiOnly,      ///< the Multi: parts, arps, effects. Voices by reference.
    WithVoices,     ///< also every part's voice edit buffer, so edits survive
};

/// Requests the Multi, and optionally each part's voice, collecting blocks
/// until each footer arrives. `progress` receives (messagesSoFar).
///
/// `report`, if given, is filled with what each request actually produced --
/// including SysEx that arrived but was not a bulk block, which is otherwise
/// discarded silently. A capture that comes back short is only diagnosable if
/// we can tell "the rack never sent it" from "it arrived and we dropped it".
[[nodiscard]] std::optional<State> captureState(
    Device&, std::function<void(int)> progress = {},
    std::chrono::milliseconds quietTime = std::chrono::milliseconds{700},
    CaptureScope scope = CaptureScope::WithVoices,
    std::string* report = nullptr);

/// Sends the captured blocks back.
///
/// Two different waits matter here. `interBlockDelay` honours the rack's Bulk
/// Interval (`00 00 1F`); too fast and blocks are dropped silently.
/// `settleAfterMulti` is much longer, because
/// restoring the Multi issues a bank select and program change for all sixteen
/// parts and the rack then has to *load* those voices. Push a Normal Voice
/// element block at a part still holding a drum kit and the rack rejects it
/// with "illegal bulk data" on its display -- and the restore is left
/// half-applied.
bool restoreState(Device&, const State&,
                  std::function<void(int, int)> progress = {},
                  std::chrono::milliseconds interBlockDelay = std::chrono::milliseconds{20},
                  std::chrono::milliseconds settleAfterMulti = std::chrono::milliseconds{1200});

/// Captures one part's voice: Common plus all eight Elements for a Normal
/// Voice, or Common plus the 73 keys for a Drum Voice. About 2 kB.
///
/// This is the unit of a "custom patch": an edited voice, saved whole. The rack
/// can only hold 16 such voices itself (Mixing Voices, bank LSB 60) and only
/// inside the current Multi; as files they are unlimited and portable.
[[nodiscard]] std::optional<State> captureVoice(
    Device&, int part,
    std::chrono::milliseconds quietTime = std::chrono::milliseconds{400});

/// Applies a captured voice to a part, which need not be the part it came from.
/// Returns whether it actually landed, verified by reading the voice name back:
/// bulk writes are never acknowledged, so sending is not the same as applying.
///
/// The part lives in the low byte of the Bulk Header and Footer, so those are
/// re-addressed and their checksums recomputed; the blocks between carry no
/// part index and pass through untouched. Writing a voice this way works for
/// any part, unlike Parameter Change, which has no part index for the 40/41/42
/// blocks and only ever reaches the part the front panel has selected.
bool applyVoice(Device&, const State&, int targetPart,
                std::chrono::milliseconds interBlockDelay = std::chrono::milliseconds{15});

/// The voice name held in a captured voice, read from its Common block.
[[nodiscard]] std::string voiceName(const State&);

/// True if a captured voice is a Drum Voice (46/47 blocks) rather than a
/// Normal Voice (40/41/42). The rack will not accept one in place of the
/// other: push Normal Voice element blocks at a part holding a drum kit and it
/// rejects the lot with "illegal bulk data" on its display.
[[nodiscard]] bool voiceIsDrum(const State&);

/// Text serialisation: a small header then one hex line per SysEx message.
/// Chosen over binary so a saved state can be read, diffed and pasted into a
/// bug report, and over JSON so the core needs no parser.
[[nodiscard]] std::string toText(const State&);
[[nodiscard]] std::optional<State> fromText(std::string_view);

[[nodiscard]] bool saveStateFile(const State&, const std::string& path, std::string* error = nullptr);
[[nodiscard]] std::optional<State> loadStateFile(const std::string& path, std::string* error = nullptr);

}  // namespace motifxs
