#include "motifxs/state.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
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
    int blocks = 0, parts = 0;
    for (const auto& m : messages) {
        const auto b = parseBulkDump(m);
        if (!b) continue;
        if (b->address.high == 0x0E || b->address.high == 0x0F) continue;  // header/footer
        ++blocks;
        if (b->address.high == 0x37) ++parts;
    }
    std::ostringstream os;
    os << blocks << " blocks, " << parts << " parts, " << payloadBytes() << " bytes";
    return os.str();
}

std::optional<State> captureState(Device& device, std::function<void(int)> progress,
                                  std::chrono::milliseconds quietTime) {
    if (!device.isOpen()) return std::nullopt;

    State state;
    state.deviceNumber = device.deviceNumber();
    state.capturedAt = nowIso8601();

    std::vector<Bytes> received;
    bool sawFooter = false;
    device.setSysExListener([&](const Bytes& m) {
        const auto b = parseBulkDump(m);
        if (!b) return;
        received.push_back(m);
        if (b->address.high == kMultiEditBufferFooter.high) sawFooter = true;
    });

    device.send(dumpRequest(device.deviceNumber(), kMultiEditBufferHeader));

    // The rack streams the whole Multi unprompted; wait for the footer, then a
    // short quiet period in case anything trails it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    std::size_t lastCount = 0;
    auto lastChange = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        if (received.size() != lastCount) {
            lastCount = received.size();
            lastChange = std::chrono::steady_clock::now();
            if (progress) progress(int(lastCount));
        }
        if (sawFooter && std::chrono::steady_clock::now() - lastChange > quietTime) break;
    }
    device.setSysExListener({});

    if (received.empty()) return std::nullopt;
    state.messages = std::move(received);
    return state;
}

bool restoreState(Device& device, const State& state,
                  std::function<void(int, int)> progress,
                  std::chrono::milliseconds interBlockDelay) {
    if (!device.isOpen() || state.messages.empty()) return false;

    const int total = int(state.messages.size());
    for (int i = 0; i < total; ++i) {
        Bytes m = state.messages[std::size_t(i)];
        // Re-stamp the device number: a state file may have come from a rack
        // set to a different one.
        if (m.size() > 2) m[2] = std::uint8_t((m[2] & 0xF0) | (device.deviceNumber() & 0x0F));
        device.send(m);
        if (progress) progress(i + 1, total);
        std::this_thread::sleep_for(interBlockDelay);
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
