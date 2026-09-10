#include "motifxs/parameters.hpp"

#include <algorithm>
#include <unordered_map>

namespace motifxs {

std::string_view toString(Scope s) {
    switch (s) {
        case Scope::System: return "system";
        case Scope::ModeChange: return "mode-change";
        case Scope::PartSetControl: return "part-set-control";
        case Scope::BulkControl: return "bulk-control";
        case Scope::NormalVoiceCommon: return "normal-voice-common";
        case Scope::NormalVoiceElement: return "normal-voice-element";
        case Scope::DrumVoiceCommon: return "drum-voice-common";
        case Scope::DrumVoiceKey: return "drum-voice-key";
        case Scope::MultiCommon: return "multi-common";
        case Scope::MultiPart: return "multi-part";
    }
    return "?";
}

Address Parameter::addressFor(int index) const {
    Address a{addrHigh, addrMid, addrLow};
    switch (variable) {
        case AddressVariable::Element:
        case AddressVariable::Part:
            a.mid = std::uint8_t(index & 0x7F);
            break;
        case AddressVariable::Table:
            // '3n' style: the index occupies the low nibble
            a.mid = std::uint8_t((addrMid & 0xF0) | (index & 0x0F));
            break;
        case AddressVariable::Program:
            a.low = std::uint8_t(index & 0x7F);
            break;
        case AddressVariable::None:
            break;
    }
    return a;
}

const Parameter* findParameterById(std::string_view id) {
    for (const auto& p : allParameters())
        if (p.id == id) return &p;
    return nullptr;
}

const Parameter* findParameter(Scope s, std::uint8_t high, std::uint8_t mid, std::uint8_t low) {
    for (const auto& p : allParameters())
        if (p.scope == s && p.addrHigh == high && p.addrMid == mid && p.addrLow == low)
            return &p;
    return nullptr;
}

const Parameter* findParameterByAddress(Address a) {
    for (const auto& p : allParameters()) {
        if (p.addrHigh != a.high || p.addrLow != a.low) continue;
        switch (p.variable) {
            case AddressVariable::None:
                if (p.addrMid == a.mid) return &p;
                break;
            case AddressVariable::Element:
            case AddressVariable::Part:
                return &p;  // any mid byte is a valid index
            case AddressVariable::Table:
                if ((p.addrMid & 0xF0) == (a.mid & 0xF0)) return &p;
                break;
            case AddressVariable::Program:
                if (p.addrMid == a.mid) return &p;
                break;
        }
    }
    return nullptr;
}

std::vector<const Parameter*> parametersInScope(Scope s) {
    std::vector<const Parameter*> out;
    for (const auto& p : allParameters())
        if (p.scope == s) out.push_back(&p);
    return out;
}

Bytes encodeValue(const Parameter& p, std::int32_t value) {
    Bytes out;
    switch (p.encoding) {
        case Encoding::Direct:
            out.push_back(std::uint8_t(value & 0x7F));
            break;
        case Encoding::MsbLsb:
            for (int i = p.size - 1; i >= 0; --i)
                out.push_back(std::uint8_t((value >> (7 * i)) & 0x7F));
            break;
        case Encoding::Nibbles:
            for (int i = p.size - 1; i >= 0; --i)
                out.push_back(std::uint8_t((value >> (4 * i)) & 0x0F));
            break;
    }
    return out;
}

std::optional<std::int32_t> decodeValue(const Parameter& p, std::span<const std::uint8_t> data) {
    if (data.size() != p.size) return std::nullopt;
    std::int32_t v = 0;
    switch (p.encoding) {
        case Encoding::Direct:
            v = data[0] & 0x7F;
            break;
        case Encoding::MsbLsb:
            for (std::uint8_t b : data) v = (v << 7) | (b & 0x7F);
            break;
        case Encoding::Nibbles:
            for (std::uint8_t b : data) v = (v << 4) | (b & 0x0F);
            break;
    }
    return v;
}

}  // namespace motifxs
