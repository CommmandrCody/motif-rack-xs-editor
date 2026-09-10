#include "motifxs/sysex.hpp"

#include <numeric>

namespace motifxs {
namespace {
constexpr std::uint8_t kParameterChangeHi = 0x10;
constexpr std::uint8_t kBulkDumpHi = 0x00;
constexpr std::uint8_t kDumpRequestHi = 0x20;
constexpr std::uint8_t kParameterRequestHi = 0x30;

Bytes header(std::uint8_t kind, std::uint8_t device) {
    return {kSysExStart, kYamahaId, std::uint8_t(kind | (device & 0x0F)),
            kModelIdHigh, kModelIdLow};
}

/// True if this looks like a MOTIF-RACK XS message with the given high nibble.
bool matches(std::span<const std::uint8_t> m, std::uint8_t kind) {
    return m.size() >= 8 && m.front() == kSysExStart && m.back() == kSysExEnd &&
           m[1] == kYamahaId && (m[2] & 0xF0) == kind &&
           m[3] == kModelIdHigh && m[4] == kModelIdLow;
}
}  // namespace

std::uint8_t checksum(std::span<const std::uint8_t> counted) {
    const int sum = std::accumulate(counted.begin(), counted.end(), 0);
    return std::uint8_t((0x80 - (sum & 0x7F)) & 0x7F);
}

Bytes parameterChange(std::uint8_t device, Address a, std::span<const std::uint8_t> data) {
    Bytes m = header(kParameterChangeHi, device);
    m.insert(m.end(), {a.high, a.mid, a.low});
    m.insert(m.end(), data.begin(), data.end());
    m.push_back(kSysExEnd);
    return m;
}

Bytes parameterRequest(std::uint8_t device, Address a) {
    Bytes m = header(kParameterRequestHi, device);
    m.insert(m.end(), {a.high, a.mid, a.low, kSysExEnd});
    return m;
}

Bytes dumpRequest(std::uint8_t device, Address a) {
    Bytes m = header(kDumpRequestHi, device);
    m.insert(m.end(), {a.high, a.mid, a.low, kSysExEnd});
    return m;
}

Bytes bulkDump(std::uint8_t device, Address a, std::span<const std::uint8_t> data) {
    Bytes m = header(kBulkDumpHi, device);
    const auto n = std::uint16_t(data.size());
    // the checksum covers byte count, address and data -- but not the framing
    Bytes counted{std::uint8_t((n >> 7) & 0x7F), std::uint8_t(n & 0x7F),
                  a.high, a.mid, a.low};
    counted.insert(counted.end(), data.begin(), data.end());
    m.insert(m.end(), counted.begin(), counted.end());
    m.push_back(checksum(counted));
    m.push_back(kSysExEnd);
    return m;
}

Bytes identityRequest(std::uint8_t device) {
    return {kSysExStart, 0x7E, std::uint8_t(device & 0x0F), 0x06, 0x01, kSysExEnd};
}

std::optional<ParameterMessage> parseParameterChange(std::span<const std::uint8_t> m) {
    if (!matches(m, kParameterChangeHi)) return std::nullopt;
    ParameterMessage out;
    out.device = m[2] & 0x0F;
    out.address = {m[5], m[6], m[7]};
    out.data.assign(m.begin() + 8, m.end() - 1);
    return out;
}

std::optional<BulkMessage> parseBulkDump(std::span<const std::uint8_t> m) {
    // F0 43 0n 7F 03 bH bL aH aM aL dd.. cc F7 -- at least framing + counts
    if (!matches(m, kBulkDumpHi) || m.size() < 12) return std::nullopt;
    const int count = (int(m[5] & 0x7F) << 7) | (m[6] & 0x7F);
    if (int(m.size()) != count + 12) return std::nullopt;   // 11 framing + checksum

    BulkMessage out;
    out.device = m[2] & 0x0F;
    out.address = {m[7], m[8], m[9]};
    out.data.assign(m.begin() + 10, m.end() - 2);

    // the checksum covers byte count, address and data
    const auto counted = m.subspan(5, std::size_t(count) + 5);
    out.checksumOk = checksum(counted) == m[m.size() - 2];
    return out;
}

bool IdentityReply::isMotifRackXs() const {
    return manufacturer == kYamahaId && family == 0x4100 && member == 0x0639;
}

std::optional<IdentityReply> parseIdentityReply(std::span<const std::uint8_t> m) {
    // F0 7E 7F 06 02 43 00 41 39 06 mm 00 00 7F F7
    if (m.size() < 15 || m.front() != kSysExStart || m.back() != kSysExEnd) return std::nullopt;
    if (m[1] != 0x7E || m[3] != 0x06 || m[4] != 0x02) return std::nullopt;
    IdentityReply r;
    r.manufacturer = m[5];
    // Device family and member are sent LSB first (MIDI 1.0 universal SysEx).
    r.family = std::uint16_t(m[6] | (m[7] << 8));
    r.member = std::uint16_t(m[8] | (m[9] << 8));
    r.version = 1.0f + float(m[10]) / 10.0f;  // mm = (version - 1.0) * 10
    return r;
}

void ChannelMessageParser::reset() {
    status_ = 0;
    partial_.clear();
    inSysEx_ = false;
}

std::vector<Bytes> ChannelMessageParser::feed(std::span<const std::uint8_t> raw) {
    std::vector<Bytes> out;
    for (std::uint8_t b : raw) {
        if (b >= 0xF8) continue;             // realtime, may interleave anywhere

        if (b == kSysExStart) {
            inSysEx_ = true;
            partial_.clear();
            status_ = 0;                     // SysEx cancels running status
            continue;
        }
        if (b == kSysExEnd) {
            inSysEx_ = false;
            continue;
        }
        if (inSysEx_) continue;

        if (b >= 0x80) {
            if (b < 0xF0) {                  // channel status
                status_ = b;
                partial_.clear();
            } else {                         // system common cancels running status
                status_ = 0;
                partial_.clear();
            }
            continue;
        }

        if (status_ == 0) continue;          // data with no status: nothing to do
        partial_.push_back(b);
        if (int(partial_.size()) == channelMessageLength(status_)) {
            Bytes msg{status_};
            msg.insert(msg.end(), partial_.begin(), partial_.end());
            out.push_back(std::move(msg));
            partial_.clear();                // running status stays armed
        }
    }
    return out;
}

std::vector<Bytes> SysExReassembler::feed(std::span<const std::uint8_t> raw) {
    std::vector<Bytes> done;
    for (std::uint8_t b : raw) {
        // System realtime may be interleaved anywhere, including mid-SysEx.
        // The unit emits Active Sensing continuously.
        if (b >= 0xF8) continue;
        if (b == kSysExStart) {
            buffer_.clear();
            buffer_.push_back(b);
        } else if (!buffer_.empty()) {
            buffer_.push_back(b);
            if (b == kSysExEnd) {
                done.push_back(buffer_);
                buffer_.clear();
            }
        }
    }
    return done;
}

}  // namespace motifxs
