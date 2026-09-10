// Factory Voice and Arpeggio catalogs, generated from Yamaha documentation.
// See docs/arp-architecture.md and data/voices.json, data/arpeggios.json.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace motifxs {

enum class VoiceKind : std::uint8_t { Normal, Drum };

struct Voice {
    std::string_view bank;      ///< "PRE1".."PRE8", "USR1".., "GM", "Preset"
    std::uint8_t msb{}, lsb{};
    std::uint8_t program{};     ///< 0-based, as MIDI Program Change carries it
    std::string_view slot;      ///< "A01".."H16", empty for drum kits
    std::string_view name;
    std::string_view categoryMain, categorySub;
    VoiceKind kind{VoiceKind::Normal};
};

[[nodiscard]] std::span<const Voice> allVoices();
/// Case-insensitive substring search over voice names, best-effort ranked.
[[nodiscard]] std::vector<const Voice*> searchVoices(std::string_view query, std::size_t limit = 20);
[[nodiscard]] const Voice* findVoice(std::uint8_t msb, std::uint8_t lsb, std::uint8_t program);

struct Arpeggio {
    std::uint16_t number{};     ///< 1..6633
    std::string_view name;
    std::string_view mainCategory, subCategory;
    std::string_view timeSignature;
    std::uint8_t length{};
    std::uint16_t originalTempo{};
    bool accent{}, randomSfx{};
    std::string_view voiceType;
};

[[nodiscard]] std::span<const Arpeggio> allArpeggios();
[[nodiscard]] const Arpeggio* findArpeggio(int number);

struct ArpFilter {
    std::string_view text;          ///< matched against name and voice type
    std::string_view mainCategory;  ///< exact, empty = any
    std::string_view timeSignature; ///< exact, empty = any
    int tempoMin{0}, tempoMax{0};   ///< 0,0 = any
};

[[nodiscard]] std::vector<const Arpeggio*> searchArpeggios(const ArpFilter&, std::size_t limit = 40);

}  // namespace motifxs
