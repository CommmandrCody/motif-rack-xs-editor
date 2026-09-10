// Yamaha MOTIF-RACK XS SysEx framing.
// See docs/protocol.md. Authority: Data List pp.58-61.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace motifxs {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint8_t kSysExStart = 0xF0;
inline constexpr std::uint8_t kSysExEnd = 0xF7;
inline constexpr std::uint8_t kYamahaId = 0x43;
inline constexpr std::uint8_t kModelIdHigh = 0x7F;  // MOTIF-RACK XS
inline constexpr std::uint8_t kModelIdLow = 0x03;

/// A three-byte parameter address (High / Mid / Low).
struct Address {
    std::uint8_t high{}, mid{}, low{};

    friend constexpr bool operator==(const Address&, const Address&) = default;
    /// Ordering so addresses can key a map.
    friend constexpr auto operator<=>(const Address&, const Address&) = default;

    [[nodiscard]] constexpr std::uint32_t packed() const {
        return (std::uint32_t(high) << 16) | (std::uint32_t(mid) << 8) | low;
    }
};

/// Bulk Dump checksum: the low 7 bits of (byte count + address + data + sum)
/// must come to zero. Parameter Change carries no checksum.
[[nodiscard]] std::uint8_t checksum(std::span<const std::uint8_t> counted);

/// F0 43 1n 7F 03 aH aM aL dd.. F7   -- no checksum
[[nodiscard]] Bytes parameterChange(std::uint8_t device, Address, std::span<const std::uint8_t> data);

/// F0 43 3n 7F 03 aH aM aL F7
[[nodiscard]] Bytes parameterRequest(std::uint8_t device, Address);

/// F0 43 2n 7F 03 aH aM aL F7
[[nodiscard]] Bytes dumpRequest(std::uint8_t device, Address);

/// F0 43 0n 7F 03 bH bL aH aM aL dd.. cc F7
[[nodiscard]] Bytes bulkDump(std::uint8_t device, Address, std::span<const std::uint8_t> data);

/// F0 7E 0n 06 01 F7
/// Note: the omni form (7F) is documented but does NOT reply on real hardware,
/// so device discovery must sweep 0..15. See docs/protocol.md.
[[nodiscard]] Bytes identityRequest(std::uint8_t device);

/// A decoded inbound Parameter Change (the reply to a Parameter Request).
struct ParameterMessage {
    std::uint8_t device{};
    Address address{};
    Bytes data;
};

/// Parse F0 43 1n 7F 03 .. F7. Returns nullopt if this is not a MOTIF-RACK XS
/// parameter change.
[[nodiscard]] std::optional<ParameterMessage> parseParameterChange(std::span<const std::uint8_t>);

/// A decoded inbound Bulk Dump.
struct BulkMessage {
    std::uint8_t device{};
    Address address{};
    Bytes data;
    bool checksumOk{};
};

/// Parse F0 43 0n 7F 03 bH bL aH aM aL dd.. cc F7.
/// Returns nullopt if this is not a MOTIF-RACK XS bulk dump; a message whose
/// checksum fails still parses but is flagged, so the caller can reject it
/// rather than silently trusting a corrupted block.
[[nodiscard]] std::optional<BulkMessage> parseBulkDump(std::span<const std::uint8_t>);

/// A decoded Identity Reply.
struct IdentityReply {
    std::uint8_t manufacturer{};
    std::uint16_t family{};
    std::uint16_t member{};
    float version{};
    [[nodiscard]] bool isMotifRackXs() const;
};

[[nodiscard]] std::optional<IdentityReply> parseIdentityReply(std::span<const std::uint8_t>);

/// Extracts complete channel messages (note, CC, program, aftertouch, bend)
/// from a MIDI byte stream, honouring running status.
///
/// The rack transmits its arpeggiator output as ordinary channel messages when
/// ARP MIDI Out is on, so anything that wants to forward or record an arpeggio
/// needs these -- the SysEx reassembler discards them.
class ChannelMessageParser {
public:
    /// Feeds raw bytes; returns any complete channel messages, in order.
    std::vector<Bytes> feed(std::span<const std::uint8_t> raw);
    void reset();

private:
    std::uint8_t status_{0};      ///< running status
    Bytes partial_;
    bool inSysEx_{false};
};

/// Number of data bytes a channel status byte expects.
[[nodiscard]] constexpr int channelMessageLength(std::uint8_t status) {
    switch (status & 0xF0) {
        case 0xC0:            // program change
        case 0xD0: return 1;  // channel aftertouch
        default: return 2;    // note off/on, poly AT, CC, pitch bend
    }
}

/// Reassembles SysEx from a MIDI byte stream, dropping realtime bytes.
/// The unit floods the input with Active Sensing (FE), so this is not optional.
class SysExReassembler {
public:
    /// Feeds raw bytes; returns any complete F0..F7 messages.
    std::vector<Bytes> feed(std::span<const std::uint8_t> raw);
    void reset() { buffer_.clear(); }

private:
    Bytes buffer_;
};

}  // namespace motifxs
