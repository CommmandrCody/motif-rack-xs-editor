// Declarative parameter model, generated from the Data List.
// See docs/address-map.md.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "motifxs/sysex.hpp"

namespace motifxs {

enum class Scope : std::uint8_t {
    System, ModeChange, PartSetControl, BulkControl,
    NormalVoiceCommon, NormalVoiceElement,
    DrumVoiceCommon, DrumVoiceKey,
    MultiCommon, MultiPart,
};

[[nodiscard]] std::string_view toString(Scope);

/// How a value wider than 7 bits is spread across data bytes.
enum class Encoding : std::uint8_t {
    Direct,   ///< one byte, 0..127
    MsbLsb,   ///< 7 bits per byte:  value = (d0 << 7) | d1
    Nibbles,  ///< 4 bits per byte:  value = (d0 << 12) | (d1 << 8) | ...
};

/// Which address byte carries a variable index, if any.
enum class AddressVariable : std::uint8_t {
    None,
    Element,  ///< 'ee' in the Mid byte: element 0-7, or drum key 0-72
    Part,     ///< 'pp' in the Mid byte: part 0-15
    Program,  ///< 'nn' in the Low byte
    Table,    ///< '3n'/'5n'/'6n': index in the low nibble of the Mid byte
};

struct Parameter {
    std::string_view id;
    std::string_view name;
    Scope scope{};
    std::uint8_t addrHigh{}, addrMid{}, addrLow{};
    AddressVariable variable{AddressVariable::None};
    std::uint8_t size{1};
    Encoding encoding{Encoding::Direct};
    std::int32_t min{}, max{};
    bool reserved{};
    std::string_view description;

    /// Resolves the variable address byte for a given element/part/table index.
    [[nodiscard]] Address addressFor(int index = 0) const;
};

/// The whole table, generated from data/parameters.json.
[[nodiscard]] std::span<const Parameter> allParameters();

[[nodiscard]] const Parameter* findParameterById(std::string_view id);
[[nodiscard]] const Parameter* findParameter(Scope, std::uint8_t high, std::uint8_t mid, std::uint8_t low);
/// Looks up by concrete address, tolerating the variable byte.
[[nodiscard]] const Parameter* findParameterByAddress(Address);
[[nodiscard]] std::vector<const Parameter*> parametersInScope(Scope);

/// Splits a value into SysEx data bytes per the parameter's encoding.
[[nodiscard]] Bytes encodeValue(const Parameter&, std::int32_t value);
/// Reassembles a value from SysEx data bytes. nullopt if the length is wrong.
[[nodiscard]] std::optional<std::int32_t> decodeValue(const Parameter&, std::span<const std::uint8_t>);

}  // namespace motifxs
