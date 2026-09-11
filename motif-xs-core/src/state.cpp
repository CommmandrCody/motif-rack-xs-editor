#include "motifxs/state.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace motifxs {
namespace {

std::string nowIso8601() {
    const auto t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string toHex(const Bytes& b) {
    std::string s;
    s.reserve(b.size() * 3);
    char tmp[4];
    for (std::size_t i = 0; i < b.size(); ++i) {
        std::snprintf(tmp, sizeof(tmp), "%02X", b[i]);
        if (i) s += ' ';
        s += tmp;
    }
    return s;
}

std::optional<Bytes> fromHex(std::string_view line) {
    Bytes out;
    int hi = -1;
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == '\r') continue;
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else return std::nullopt;
        if (hi < 0) hi = v;
        else { out.push_back(std::uint8_t((hi << 4) | v)); hi = -1; }
    }
    if (hi >= 0) return std::nullopt;      // odd number of nibbles
    return out;
}

}  // namespace

std::size_t State::payloadBytes() const {
    std::size_t n = 0;
    for (const auto& m : messages)
        if (const auto b = parseBulkDump(m)) n += b->data.size();
    return n;
}

std::string State::summary() const {
    int blocks = 0, parts = 0, voices = 0;
    for (const auto& m : messages) {
        const auto b = parseBulkDump(m);
        if (!b) continue;
        if (b->address.high == 0x0F) continue;              // footer
        if (b->address.high == 0x0E) {
            if (b->address.mid == 0x30) ++voices;            // one per part voice
            continue;
        }
        ++blocks;
        if (b->address.high == 0x37) ++parts;
    }
    std::ostringstream os;
    os << blocks << " blocks, " << parts << " parts";
    if (voices) os << ", " << voices << " voices";
    os << ", " << payloadBytes() << " bytes";
    return os.str();
}

std::string State::problem() const {
    if (messages.empty()) return "nothing was captured";

    bool multiHeader = false, multiFooter = false;
    std::array<bool, kParts> partSeen{};
    partSeen.fill(false);
    int voiceCommons = 0;
    for (const auto& m : messages) {
        const auto b = parseBulkDump(m);
        if (!b) continue;
        if (b->address.high == 0x0E && b->address.mid == 0x5F) multiHeader = true;
        if (b->address.high == 0x0F && b->address.mid == 0x5F) multiFooter = true;
        if (b->address.high == 0x37) partSeen[std::size_t(b->address.mid & 0x0F)] = true;
        if ((b->address.high == 0x40 || b->address.high == 0x46) &&
            b->address.mid == 0x00 && b->address.low == 0x00)
            ++voiceCommons;
    }

    // A voice-only capture: one Common block and its framing is the whole of it.
    if (!multiHeader && !multiFooter)
        return voiceCommons > 0 ? std::string{}
                                : "the voice came back without its Common block";

    if (!multiHeader || !multiFooter) return "the Multi dump was cut short";

    const int missing = int(std::count(partSeen.begin(), partSeen.end(), false));
    if (missing == kParts)
        return "the rack returned no part data at all - it is probably not in Multi mode";
    if (missing > 0)
        return std::to_string(missing) + " of 16 parts are missing from the Multi";
    return {};
}

namespace {

/// Requests one bulk sequence and collects blocks until its footer, then a
/// short quiet period in case anything trails it. Returns what arrived.
std::vector<Bytes> requestSequence(Device& device, Address header,
                                   std::uint8_t footerHigh,
                                   std::chrono::milliseconds quietTime,
                                   std::function<void(int)> progress,
                                   int alreadyHave,
                                   std::string* report = nullptr) {
    // The listener runs on the CoreMIDI read thread while this one polls for
    // progress, so the collection needs its own lock -- reading size() beside
    // an unsynchronised push_back is a race, and a torn read here ends the
    // sequence early and silently loses whatever had not arrived yet.
    std::mutex collected;
    std::vector<Bytes> received;
    std::atomic<bool> sawFooter{false};
    // Counted separately: SysEx that arrived during the sequence but was not a
    // bulk block. Dropping those without trace is how a short capture becomes
    // unexplainable -- a block damaged in transit looks the same as one the
    // rack never sent.
    std::atomic<int> unparsed{0};
    std::atomic<int> biggest{0};
    device.setSysExListener([&](const Bytes& m) {
        const auto b = parseBulkDump(m);
        if (int(m.size()) > biggest.load()) biggest.store(int(m.size()));
        if (!b) {
            unparsed.fetch_add(1);
            return;
        }
        {
            std::lock_guard lock(collected);
            received.push_back(m);
        }
        if (b->address.high == footerHigh) sawFooter.store(true);
    });

    device.send(dumpRequest(device.deviceNumber(), header));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    std::size_t lastCount = 0;
    auto lastChange = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        std::size_t have = 0;
        {
            std::lock_guard lock(collected);
            have = received.size();
        }
        if (have != lastCount) {
            lastCount = have;
            lastChange = std::chrono::steady_clock::now();
            if (progress) progress(alreadyHave + int(lastCount));
        }
        // The footer is a definitive terminator, so once it lands only a brief
        // settle is needed. Waiting the full quiet time after every sequence
        // turned a 17-request capture into fifteen seconds of mostly idling.
        const auto settle = sawFooter.load() ? std::chrono::milliseconds{80} : quietTime;
        if (sawFooter.load() && std::chrono::steady_clock::now() - lastChange > settle) break;
    }
    device.setSysExListener({});   // takes the device lock, so no call is in flight
    std::lock_guard lock(collected);
    if (report) {
        char line[160];
        std::snprintf(line, sizeof line,
                      "%02X %02X %02X: %zu blocks, %d unparsed, largest %d bytes%s\n",
                      header.high, header.mid, header.low, received.size(),
                      unparsed.load(), biggest.load(),
                      sawFooter.load() ? "" : ", NO FOOTER (timed out)");
        *report += line;
    }
    return received;
}

}  // namespace

std::optional<State> captureState(Device& device, std::function<void(int)> progress,
                                  std::chrono::milliseconds quietTime, CaptureScope scope,
                                  std::string* report) {
    if (!device.isOpen()) return std::nullopt;

    State state;
    state.deviceNumber = device.deviceNumber();
    state.capturedAt = nowIso8601();

    auto multi = requestSequence(device, kMultiEditBufferHeader,
                                 kMultiEditBufferFooter.high, quietTime, progress, 0, report);
    if (multi.empty()) return std::nullopt;
    state.messages = std::move(multi);

    if (scope == CaptureScope::WithVoices) {
        // A Multi records each part's bank and program -- a reference to a
        // patch. Without the voice buffers, restoring brings back the stored
        // patch and silently discards every edit made to it.
        for (int part = 0; part < kParts; ++part) {
            auto voice = requestSequence(device, partVoiceHeader(part),
                                         partVoiceFooter(part).high, quietTime, progress,
                                         int(state.messages.size()), report);
            state.messages.insert(state.messages.end(), voice.begin(), voice.end());
        }
    }

    // Saving a half-arrived capture is the worst outcome available: it looks
    // like it worked, replaces the good state in the project, and only fails
    // when the project is reopened and the rack rejects the stream.
    if (const auto bad = state.problem(); !bad.empty()) {
        if (report) *report += "REJECTED: " + bad + "\n";
        return std::nullopt;
    }
    return state;
}

std::optional<State> captureVoice(Device& device, int part,
                                  std::chrono::milliseconds quietTime) {
    if (!device.isOpen() || part < 0 || part >= kParts) return std::nullopt;
    auto msgs = requestSequence(device, partVoiceHeader(part), partVoiceFooter(part).high,
                                quietTime, {}, 0);
    if (msgs.empty()) return std::nullopt;

    State s;
    s.deviceNumber = device.deviceNumber();
    s.capturedAt = nowIso8601();
    s.messages = std::move(msgs);
    if (!s.problem().empty()) return std::nullopt;
    s.note = voiceName(s);
    return s;
}

std::string voiceName(const State& s) {
    // The Common block leads with 20 bytes of ASCII, for both Normal (40 00 00)
    // and Drum (46 00 00) voices.
    for (const auto& m : s.messages) {
        const auto b = parseBulkDump(m);
        if (!b) continue;
        if ((b->address.high != 0x40 && b->address.high != 0x46) ||
            b->address.mid != 0x00 || b->address.low != 0x00)
            continue;
        std::string name;
        for (std::size_t i = 0; i < 20 && i < b->data.size(); ++i) {
            const char c = char(b->data[i]);
            if (c >= 32 && c < 127) name.push_back(c);
        }
        while (!name.empty() && name.back() == ' ') name.pop_back();
        return name;
    }
    return {};
}

bool voiceIsDrum(const State& s) {
    for (const auto& m : s.messages) {
        const auto b = parseBulkDump(m);
        if (b && (b->address.high == 0x46 || b->address.high == 0x47)) return true;
    }
    return false;
}

namespace {

/// Puts a part onto a voice of the right *kind* so the bulk that follows is
/// accepted. Any patch of that kind will do -- the bulk replaces its contents
/// entirely; this only establishes the type of edit buffer the rack allocates.
void ensureVoiceType(Device& device, int part, bool drum) {
    if (drum) device.selectVoice(63, 32, 0, std::uint8_t(part));   // drum preset
    else device.selectVoice(63, 0, 0, std::uint8_t(part));          // PRE1
    std::this_thread::sleep_for(std::chrono::milliseconds{450});
}

}  // namespace

bool applyVoice(Device& device, const State& voice, int targetPart,
                std::chrono::milliseconds interBlockDelay) {
    if (!device.isOpen() || voice.messages.empty()) return false;
    if (targetPart < 0 || targetPart >= kParts) return false;

    // The target part may be holding the other kind of voice entirely, in
    // which case every block below would be rejected -- silently, since bulk
    // writes are never acknowledged.
    ensureVoiceType(device, targetPart, voiceIsDrum(voice));

    for (const auto& original : voice.messages) {
        Bytes m = original;
        if (m.size() > 2)
            m[2] = std::uint8_t((m[2] & 0xF0) | (device.deviceNumber() & 0x0F));

        const auto parsed = parseBulkDump(m);
        // Re-address the header and footer to the target part, then fix the
        // checksum, which covers the byte count, address and data.
        if (parsed && (parsed->address.high == 0x0E || parsed->address.high == 0x0F) &&
            parsed->address.mid == 0x30 && m.size() >= 12) {
            m[9] = std::uint8_t(targetPart & 0x0F);              // address low
            const std::size_t counted = m.size() - 7;            // count..data
            m[m.size() - 2] = checksum(std::span<const std::uint8_t>(m).subspan(5, counted));
        }
        device.send(m);
        std::this_thread::sleep_for(interBlockDelay);
    }

    // Bulk writes are not acknowledged, so the only way to know it landed is to
    // read the voice back. Returning true regardless is how a rejected apply
    // looked like a successful one.
    std::this_thread::sleep_for(std::chrono::milliseconds{250});
    const std::string wanted = voiceName(voice);
    const std::uint8_t nameBlock = voiceIsDrum(voice) ? 0x46 : 0x40;
    std::string got;
    for (int i = 0; i < 20; ++i) {
        auto b = device.readAddress({nameBlock, 0x00, std::uint8_t(i)});
        if (!b || b->empty()) break;
        const char ch = char((*b)[0]);
        if (ch >= 32 && ch < 127) got.push_back(ch);
    }
    while (!got.empty() && got.back() == ' ') got.pop_back();
    return !wanted.empty() && got == wanted;
}

bool restoreState(Device& device, const State& state,
                  std::function<void(int, int)> progress,
                  std::chrono::milliseconds interBlockDelay,
                  std::chrono::milliseconds settleAfterMulti) {
    if (!device.isOpen() || state.messages.empty()) return false;
    // Never push a capture that is already known to be incomplete: the rack
    // rejects the stream, shows "illegal bulk data", and half-applies it.
    if (!state.problem().empty()) return false;

    // Put the rack in Multi first -- the same thing as pressing MULTI on the
    // front panel. The edit buffers this restores into only exist in Multi, so
    // a restore that arrives while the rack is on a Voice has nowhere to land.
    if (const auto* mode = findParameter(Scope::ModeChange, 0x0A, 0x00, 0x01)) {
        device.writeParameter(*mode, 5);                       // 5: Multi
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }

    // Split into bulk sequences: a header (0E) starts one, a footer (0F) ends
    // it. They must be sent whole and in order, and a voice sequence needs its
    // part put on the right kind of voice first.
    struct Sequence {
        std::vector<Bytes> messages;
        bool isVoice{};      // 0E 30 nn -- a part's voice, rather than the Multi
        int part{};
        bool drum{};
    };
    // Each part's own block from the captured Multi. Re-sending a part's block
    // immediately before its voice puts that part on the right patch -- and so
    // the right *kind* of voice -- using the capture's own data.
    //
    // The alternative, forcing the type with a Program Change to some neutral
    // patch, rewrites the part's bank and program as a side effect. Done for
    // every part it silently reset an entire Multi to PRE1 000.
    std::array<const Bytes*, kParts> partBlock{};
    partBlock.fill(nullptr);
    for (const auto& m : state.messages) {
        const auto b = parseBulkDump(m);
        if (b && b->address.high == 0x37)
            partBlock[std::size_t(b->address.mid & 0x0F)] = &m;
    }

    std::vector<Sequence> sequences;
    for (const auto& m : state.messages) {
        const auto b = parseBulkDump(m);
        if (b && b->address.high == 0x0E) {
            sequences.push_back({});
            sequences.back().isVoice = b->address.mid == 0x30;
            sequences.back().part = b->address.low & 0x0F;
        }
        if (sequences.empty()) sequences.push_back({});
        sequences.back().messages.push_back(m);
        if (b && (b->address.high == 0x46 || b->address.high == 0x47))
            sequences.back().drum = true;
    }

    int sent = 0;
    const int total = int(state.messages.size());
    for (auto& seq : sequences) {
        // The Multi issues a bank select and program change for all sixteen
        // parts; the rack then has to load those voices before anything can be
        // written into their edit buffers.
        if (!seq.isVoice) {
            for (const auto& m : seq.messages) {
                Bytes out = m;
                if (out.size() > 2)
                    out[2] = std::uint8_t((out[2] & 0xF0) | (device.deviceNumber() & 0x0F));
                device.send(out);
                if (progress) progress(++sent, total);
                std::this_thread::sleep_for(interBlockDelay);
            }
            std::this_thread::sleep_for(settleAfterMulti);
            continue;
        }

        // Restate this part's patch right before its voice, so the rack has
        // definitely allocated the right kind of edit buffer. Pushing Normal
        // Voice blocks at a part still holding a drum kit is rejected wholesale
        // and shows "illegal bulk data".
        if (const Bytes* pb = partBlock[std::size_t(seq.part & 0x0F)]) {
            Bytes out = *pb;
            if (out.size() > 2)
                out[2] = std::uint8_t((out[2] & 0xF0) | (device.deviceNumber() & 0x0F));
            device.send(out);
            std::this_thread::sleep_for(std::chrono::milliseconds{380});
        }

        for (const auto& m : seq.messages) {
            Bytes out = m;
            if (out.size() > 2)
                out[2] = std::uint8_t((out[2] & 0xF0) | (device.deviceNumber() & 0x0F));
            device.send(out);
            if (progress) progress(++sent, total);
            std::this_thread::sleep_for(interBlockDelay);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{120});
    }
    return true;
}

std::string toText(const State& s) {
    std::ostringstream os;
    os << "motifxs-state " << s.schemaVersion << "\n";
    os << "device " << s.device << "\n";
    os << "device-number " << int(s.deviceNumber) << "\n";
    os << "firmware " << s.firmware << "\n";
    os << "captured " << s.capturedAt << "\n";
    if (!s.note.empty()) os << "note " << s.note << "\n";
    os << "summary " << s.summary() << "\n";
    os << "messages " << s.messages.size() << "\n";
    for (const auto& m : s.messages) os << toHex(m) << "\n";
    return os.str();
}

std::optional<State> fromText(std::string_view text) {
    State s;
    std::istringstream is{std::string(text)};
    std::string line;
    bool sawMagic = false;
    while (std::getline(is, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;

        if (!sawMagic) {
            if (line.rfind("motifxs-state", 0) != 0) return std::nullopt;
            s.schemaVersion = std::atoi(line.c_str() + 13);
            sawMagic = true;
            continue;
        }
        const auto sp = line.find(' ');
        const std::string key = line.substr(0, sp);
        const std::string val = sp == std::string::npos ? std::string() : line.substr(sp + 1);

        if (key == "device") s.device = val;
        else if (key == "device-number") s.deviceNumber = std::uint8_t(std::atoi(val.c_str()));
        else if (key == "firmware") s.firmware = float(std::atof(val.c_str()));
        else if (key == "captured") s.capturedAt = val;
        else if (key == "note") s.note = val;
        else if (key == "summary" || key == "messages") continue;
        else if (auto bytes = fromHex(line)) {
            if (!bytes->empty()) s.messages.push_back(*bytes);
        }
    }
    if (!sawMagic) return std::nullopt;
    return s;
}

bool saveStateFile(const State& s, const std::string& path, std::string* error) {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "could not open " + path + " for writing";
        return false;
    }
    const auto text = toText(s);
    f.write(text.data(), std::streamsize(text.size()));
    if (!f) {
        if (error) *error = "could not write " + path;
        return false;
    }
    return true;
}

std::optional<State> loadStateFile(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "could not open " + path;
        return std::nullopt;
    }
    std::ostringstream os;
    os << f.rdbuf();
    auto s = fromText(os.str());
    if (!s && error) *error = path + " is not a motifxs state file";
    return s;
}

}  // namespace motifxs
