#include "motifxs/catalog.hpp"

#include <algorithm>
#include <cctype>

namespace motifxs {
namespace {

std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return o;
}

bool contains(std::string_view haystack, const std::string& needleLower) {
    return needleLower.empty() || lower(haystack).find(needleLower) != std::string::npos;
}

}  // namespace

std::vector<const Voice*> searchVoices(std::string_view query, std::size_t limit) {
    const std::string q = lower(query);
    std::vector<const Voice*> exact, prefix, rest;
    for (const auto& v : allVoices()) {
        const std::string n = lower(v.name);
        if (q.empty() || n == q) exact.push_back(&v);
        else if (n.rfind(q, 0) == 0) prefix.push_back(&v);
        else if (n.find(q) != std::string::npos) rest.push_back(&v);
    }
    exact.insert(exact.end(), prefix.begin(), prefix.end());
    exact.insert(exact.end(), rest.begin(), rest.end());
    if (exact.size() > limit) exact.resize(limit);
    return exact;
}

const Voice* findVoice(std::uint8_t msb, std::uint8_t lsb, std::uint8_t program) {
    for (const auto& v : allVoices())
        if (v.msb == msb && v.lsb == lsb && v.program == program) return &v;
    return nullptr;
}

const Waveform* findWaveform(int number) {
    for (const auto& w : allWaveforms())
        if (w.number == number) return &w;
    return nullptr;
}

const Arpeggio* findArpeggio(int number) {
    for (const auto& a : allArpeggios())
        if (a.number == number) return &a;
    return nullptr;
}

std::vector<const Arpeggio*> searchArpeggios(const ArpFilter& f, std::size_t limit) {
    const std::string text = lower(f.text);
    std::vector<const Arpeggio*> out;
    for (const auto& a : allArpeggios()) {
        if (!f.mainCategory.empty() && a.mainCategory != f.mainCategory) continue;
        if (!f.timeSignature.empty() && a.timeSignature != f.timeSignature) continue;
        if (f.tempoMin || f.tempoMax) {
            if (f.tempoMin && a.originalTempo < f.tempoMin) continue;
            if (f.tempoMax && a.originalTempo > f.tempoMax) continue;
        }
        if (!text.empty() && !contains(a.name, text) && !contains(a.voiceType, text))
            continue;
        out.push_back(&a);
        if (out.size() >= limit) break;
    }
    return out;
}

}  // namespace motifxs
