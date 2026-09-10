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
};

/// Requests the whole Multi and collects blocks until the footer arrives.
/// `progress` receives (messagesSoFar) as they land.
[[nodiscard]] std::optional<State> captureState(
    Device&, std::function<void(int)> progress = {},
    std::chrono::milliseconds quietTime = std::chrono::milliseconds{700});

/// Sends the captured blocks back. `interBlockDelay` honours the rack's Bulk
/// Interval setting (00 00 1F); too fast and blocks are dropped silently.
bool restoreState(Device&, const State&,
                  std::function<void(int, int)> progress = {},
                  std::chrono::milliseconds interBlockDelay = std::chrono::milliseconds{15});

/// Text serialisation: a small header then one hex line per SysEx message.
/// Chosen over binary so a saved state can be read, diffed and pasted into a
/// bug report, and over JSON so the core needs no parser.
[[nodiscard]] std::string toText(const State&);
[[nodiscard]] std::optional<State> fromText(std::string_view);

[[nodiscard]] bool saveStateFile(const State&, const std::string& path, std::string* error = nullptr);
[[nodiscard]] std::optional<State> loadStateFile(const std::string& path, std::string* error = nullptr);

}  // namespace motifxs
