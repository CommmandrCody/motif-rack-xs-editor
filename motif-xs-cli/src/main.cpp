// motifxs -- command line proof of concept (milestone 1).
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "motifxs/catalog.hpp"
#include "motifxs/device.hpp"
#include "motifxs/parameters.hpp"
#include "motifxs/sysex.hpp"

using namespace motifxs;

namespace {

std::string gPort;  // --port override

int usage() {
    std::puts(
        "motifxs -- Yamaha MOTIF-RACK XS controller\n"
        "\n"
        "  motifxs list-midi                     list CoreMIDI endpoints\n"
        "  motifxs identify                      find the rack, report device number\n"
        "  motifxs status                        connection and mode report\n"
        "  motifxs voices [query]                search the factory voice list\n"
        "  motifxs patch <bank> <n>              select voice n (1-based, e.g. PRE1 1)\n"
        "  motifxs patch-name <query>            select the best-matching voice by name\n"
        "  motifxs get <param-id> [index]        read a parameter\n"
        "  motifxs set <param-id> <value> [ix]   write a parameter\n"
        "  motifxs read <HH> <MM> <LL>           read a raw address\n"
        "  motifxs params [scope] [query]        list known parameters\n"
        "  motifxs arp-list [query]              search the arpeggio catalog\n"
        "  motifxs arp <slot 1-5> <number>       assign an arpeggio type\n"
        "  motifxs dump-state                    read the live edit buffer\n"
        "\n"
        "  --port <name>   use a specific MIDI port (default: MOTIF ... Port1)\n");
    return 1;
}

std::optional<long> toLong(std::string_view s) {
    long v{};
    const auto* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc{} || p != end) return std::nullopt;
    return v;
}

std::optional<int> toHex(std::string_view s) {
    int v{};
    const auto* end = s.data() + s.size();
    auto [p, ec] = std::from_chars(s.data(), end, v, 16);
    if (ec != std::errc{} || p != end) return std::nullopt;
    return v;
}

bool connect(Device& d) {
    std::string err;
    if (!d.open(gPort, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        if (gPort.empty())
            std::fprintf(stderr, "hint: run 'motifxs list-midi' and pass --port <name>\n");
        return false;
    }
    return true;
}

/// Connects and identifies. Identification also fixes the device number.
bool connectAndIdentify(Device& d, DeviceInfo* info = nullptr) {
    if (!connect(d)) return false;
    auto id = d.identify();
    if (!id) {
        std::fprintf(stderr,
                     "error: no reply from a MOTIF-RACK XS on '%s'\n"
                     "hint: only Port1 carries SysEx; check the rack is powered on\n",
                     d.portName().c_str());
        return false;
    }
    if (info) *info = *id;
    return true;
}

/// Warns if the rack is configured to ignore patch selection. This shipped
/// switched off on the reference unit and fails completely silently.
void checkPatchReceiveSwitches(Device& d) {
    const auto* bank = findParameter(Scope::System, 0x00, 0x00, 0x14);
    const auto* prog = findParameter(Scope::System, 0x00, 0x00, 0x15);
    if (!bank || !prog) return;
    const auto b = d.readParameter(*bank);
    const auto p = d.readParameter(*prog);
    if ((b && *b == 0) || (p && *p == 0)) {
        std::fprintf(stderr,
                     "warning: this rack is set to ignore %s%s%s.\n"
                     "         Patch selection will do nothing, with no error.\n"
                     "         Fix: motifxs set system_receive_transmit_bank_select 1\n"
                     "              motifxs set system_receive_transmit_program_change 1\n",
                     (b && *b == 0) ? "Bank Select" : "",
                     (b && *b == 0) && (p && *p == 0) ? " and " : "",
                     (p && *p == 0) ? "Program Change" : "");
    }
}

int cmdListMidi() {
    std::puts("sources (device -> host):");
    for (const auto& e : listSources()) std::printf("  [%2d] %s\n", e.index, e.name.c_str());
    std::puts("destinations (host -> device):");
    for (const auto& e : listDestinations()) std::printf("  [%2d] %s\n", e.index, e.name.c_str());
    return 0;
}

int cmdIdentify() {
    Device d;
    DeviceInfo info;
    if (!connectAndIdentify(d, &info)) return 1;
    std::printf("MOTIF-RACK XS on '%s'\n", info.portName.c_str());
    std::printf("  device number : %u (panel shows %u)\n", info.deviceNumber,
                unsigned(info.deviceNumber + 1));
    std::printf("  firmware      : %.1f\n", double(info.firmwareVersion));
    return 0;
}

/// Reads mode and works out which edit buffer is currently live.
int cmdStatus() {
    Device d;
    DeviceInfo info;
    if (!connectAndIdentify(d, &info)) return 1;
    std::printf("port          : %s\n", info.portName.c_str());
    std::printf("device number : %u\n", info.deviceNumber);
    std::printf("firmware      : %.1f\n", double(info.firmwareVersion));

    long modeValue = -1;
    if (const auto* mode = findParameter(Scope::ModeChange, 0x0A, 0x00, 0x01)) {
        if (auto v = d.readParameter(*mode)) {
            modeValue = long(*v);
            const char* name = (*v == 0) ? "Voice" : (*v == 5) ? "Multi" : (*v == 6) ? "Demo" : "?";
            std::printf("mode          : %s (%ld)\n", name, modeValue);
        }
    }

    // Which edit buffer answers tells us what is loaded. In Multi mode the
    // Voice blocks also answer -- they describe the *selected part's* voice --
    // so report the Multi context first or the mode reads as "Drum Voice".
    const bool normal = d.readAddress({0x40, 0x00, 0x00}).has_value();
    const bool drum = d.readAddress({0x46, 0x00, 0x00}).has_value();
    const bool multi = d.readAddress({0x36, 0x00, 0x00}).has_value();
    if (multi) {
        std::printf("edit buffer   : Multi (selected part's voice: %s)\n",
                    normal ? "Normal" : drum ? "Drum" : "none");
    } else {
        std::printf("edit buffer   : %s\n",
                    normal ? "Normal Voice" : drum ? "Drum Voice" : "none responding");
    }

    auto name = [&](std::uint8_t high) {
        std::string s;
        for (int i = 0; i < 20; ++i) {
            auto b = d.readAddress({high, 0x00, std::uint8_t(i)});
            if (!b || b->empty()) break;
            s.push_back(char((*b)[0]));
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };
    if (normal) std::printf("voice name    : %s\n", name(0x40).c_str());
    if (drum) std::printf("kit name      : %s\n", name(0x46).c_str());

    if (multi) {
        // Resolve each part's bank/program against the factory catalog.
        // These are 0-based on the wire despite the Data List's "1 - 128".
        std::puts("parts         :");
        for (int part = 0; part < 16; ++part) {
            const auto msb = d.readAddress({0x37, std::uint8_t(part), 0x01});
            const auto lsb = d.readAddress({0x37, std::uint8_t(part), 0x02});
            const auto pgm = d.readAddress({0x37, std::uint8_t(part), 0x03});
            const auto ch = d.readAddress({0x37, std::uint8_t(part), 0x04});
            if (!msb || !lsb || !pgm || msb->empty() || lsb->empty() || pgm->empty()) continue;
            const auto* v = findVoice((*msb)[0], (*lsb)[0], (*pgm)[0]);
            char chan[8] = "?";
            if (ch && !ch->empty())
                std::snprintf(chan, sizeof(chan), (*ch)[0] == 0x7F ? "off" : "%d", (*ch)[0] + 1);
            std::printf("  %2d  ch=%-3s  %s\n", part + 1, chan,
                        v ? std::string(v->name).c_str() : "(unmapped)");
        }
    }

    checkPatchReceiveSwitches(d);
    return 0;
}

int cmdVoices(int argc, char** argv) {
    const std::string q = argc > 0 ? argv[0] : "";
    auto hits = searchVoices(q, 30);
    if (hits.empty()) {
        std::printf("no voice matches '%s'\n", q.c_str());
        return 1;
    }
    for (const auto* v : hits)
        std::printf("  %-6s %-4s pc=%-3u  %-24s %s/%s\n", std::string(v->bank).c_str(),
                    std::string(v->slot).c_str(), v->program, std::string(v->name).c_str(),
                    std::string(v->categoryMain).c_str(), std::string(v->categorySub).c_str());
    std::printf("(%zu shown)\n", hits.size());
    return 0;
}

const Voice* resolveBank(std::string_view bank, long program) {
    for (const auto& v : allVoices())
        if (v.bank == bank && v.program == program) return &v;
    return nullptr;
}

int cmdPatch(int argc, char** argv) {
    if (argc < 2) return usage();
    const auto program = toLong(argv[1]);
    if (!program) {
        std::fprintf(stderr, "error: program must be a number\n");
        return 1;
    }
    // Voice numbers are 1-based here, matching the Data List and the front
    // panel. Program Change on the wire is 0-based; convert once, here.
    // Accepting both silently is how "everything is one patch out" happens.
    if (*program < 1 || *program > 128) {
        std::fprintf(stderr, "error: voice number must be 1..128 (as printed in the Data List)\n");
        return 1;
    }
    const auto* v = resolveBank(argv[0], *program - 1);
    if (!v) {
        std::fprintf(stderr, "error: no voice at %s %ld\n", argv[0], *program);
        std::fprintf(stderr, "hint: banks are PRE1..PRE8, GM, Preset (drums)\n");
        return 1;
    }
    Device d;
    if (!connectAndIdentify(d)) return 1;
    checkPatchReceiveSwitches(d);
    d.selectVoice(v->msb, v->lsb, v->program);
    std::printf("selected %s %s (MSB=%u LSB=%u PC=%u)  %s\n", std::string(v->bank).c_str(),
                std::string(v->slot).c_str(), v->msb, v->lsb, v->program,
                std::string(v->name).c_str());
    return 0;
}

int cmdPatchName(int argc, char** argv) {
    if (argc < 1) return usage();
    std::string q = argv[0];
    for (int i = 1; i < argc; ++i) q += " " + std::string(argv[i]);
    auto hits = searchVoices(q, 1);
    if (hits.empty()) {
        std::fprintf(stderr, "error: no voice matches '%s'\n", q.c_str());
        return 1;
    }
    const auto* v = hits.front();
    Device d;
    if (!connectAndIdentify(d)) return 1;
    checkPatchReceiveSwitches(d);
    d.selectVoice(v->msb, v->lsb, v->program);
    std::printf("selected %s %s  %s\n", std::string(v->bank).c_str(),
                std::string(v->slot).c_str(), std::string(v->name).c_str());
    return 0;
}

int cmdGet(int argc, char** argv) {
    if (argc < 1) return usage();
    const auto* p = findParameterById(argv[0]);
    if (!p) {
        std::fprintf(stderr, "error: unknown parameter '%s' (try 'motifxs params')\n", argv[0]);
        return 1;
    }
    const int index = argc > 1 ? int(toLong(argv[1]).value_or(0)) : 0;
    Device d;
    if (!connectAndIdentify(d)) return 1;
    const auto v = d.readParameter(*p, index);
    const Address a = p->addressFor(index);
    if (!v) {
        std::printf("%s @ %02X %02X %02X: no reply\n", std::string(p->name).c_str(), a.high,
                    a.mid, a.low);
        std::puts("  (reserved, mid-parameter, or the wrong mode is active)");
        return 1;
    }
    std::printf("%s @ %02X %02X %02X = %ld\n", std::string(p->name).c_str(), a.high, a.mid,
                a.low, long(*v));
    if (!p->description.empty()) std::printf("  %s\n", std::string(p->description).c_str());
    return 0;
}

int cmdSet(int argc, char** argv) {
    if (argc < 2) return usage();
    const auto* p = findParameterById(argv[0]);
    if (!p) {
        std::fprintf(stderr, "error: unknown parameter '%s'\n", argv[0]);
        return 1;
    }
    const auto value = toLong(argv[1]);
    if (!value) {
        std::fprintf(stderr, "error: value must be a number\n");
        return 1;
    }
    if (p->max > p->min && (*value < p->min || *value > p->max))
        std::fprintf(stderr, "warning: %ld is outside the documented range %d..%d\n", *value,
                     p->min, p->max);
    const int index = argc > 2 ? int(toLong(argv[2]).value_or(0)) : 0;
    Device d;
    if (!connectAndIdentify(d)) return 1;
    d.writeParameter(*p, std::int32_t(*value), index);
    // Parameter Change is not acknowledged, so read back to confirm.
    std::this_thread::sleep_for(std::chrono::milliseconds{40});
    if (auto got = d.readParameter(*p, index)) {
        std::printf("%s = %ld%s\n", std::string(p->name).c_str(), long(*got),
                    (*got == *value) ? "" : "  (differs from the value sent)");
    } else {
        std::printf("%s: wrote %ld (no read-back available)\n", std::string(p->name).c_str(),
                    *value);
    }
    return 0;
}

int cmdRead(int argc, char** argv) {
    if (argc < 3) return usage();
    const auto h = toHex(argv[0]), m = toHex(argv[1]), l = toHex(argv[2]);
    if (!h || !m || !l) {
        std::fprintf(stderr, "error: address bytes must be hex, e.g. 40 00 00\n");
        return 1;
    }
    Device d;
    if (!connectAndIdentify(d)) return 1;
    const Address a{std::uint8_t(*h), std::uint8_t(*m), std::uint8_t(*l)};
    auto data = d.readAddress(a);
    if (!data) {
        std::printf("%02X %02X %02X: no reply\n", a.high, a.mid, a.low);
        return 1;
    }
    std::printf("%02X %02X %02X: %zu byte(s):", a.high, a.mid, a.low, data->size());
    for (auto b : *data) std::printf(" %02X", b);
    std::putchar('\n');
    if (const auto* p = findParameterByAddress(a)) {
        std::printf("  %s", std::string(p->name).c_str());
        if (auto v = decodeValue(*p, *data)) std::printf(" = %ld", long(*v));
        std::putchar('\n');
    }
    return 0;
}

int cmdParams(int argc, char** argv) {
    const std::string scopeArg = argc > 0 ? argv[0] : "";
    const std::string query = argc > 1 ? argv[1] : "";
    std::size_t shown = 0;
    for (const auto& p : allParameters()) {
        if (p.reserved) continue;
        if (!scopeArg.empty() && toString(p.scope) != scopeArg) continue;
        if (!query.empty()) {
            std::string n(p.name), q(query);
            for (auto& c : n) c = char(std::tolower((unsigned char)c));
            for (auto& c : q) c = char(std::tolower((unsigned char)c));
            if (n.find(q) == std::string::npos) continue;
        }
        std::printf("  %-52s %02X %02X %02X sz=%u %s\n", std::string(p.id).c_str(), p.addrHigh,
                    p.addrMid, p.addrLow, p.size, std::string(p.name).c_str());
        if (++shown >= 60) {
            std::puts("  ... (narrow the search)");
            break;
        }
    }
    if (!shown) {
        std::puts("no parameters match. scopes:");
        std::puts("  system mode-change normal-voice-common normal-voice-element");
        std::puts("  drum-voice-common drum-voice-key multi-common multi-part");
    }
    return shown ? 0 : 1;
}

int cmdArpList(int argc, char** argv) {
    ArpFilter f;
    std::string text;
    for (int i = 0; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a.rfind("cat=", 0) == 0) f.mainCategory = argv[i] + 4;
        else if (a.rfind("sig=", 0) == 0) f.timeSignature = argv[i] + 4;
        else if (a.rfind("tempo=", 0) == 0) {
            const std::string spec = argv[i] + 6;
            const auto dash = spec.find('-');
            if (dash == std::string::npos) {
                f.tempoMin = f.tempoMax = int(toLong(spec).value_or(0));
            } else {
                f.tempoMin = int(toLong(spec.substr(0, dash)).value_or(0));
                f.tempoMax = int(toLong(spec.substr(dash + 1)).value_or(0));
            }
        } else {
            text += (text.empty() ? "" : " ") + std::string(a);
        }
    }
    f.text = text;
    auto hits = searchArpeggios(f, 40);
    for (const auto* a : hits)
        std::printf("  %5u  %-24s %-6s %-6s %-5s %3u bpm%s%s  %s\n", a->number,
                    std::string(a->name).c_str(), std::string(a->mainCategory).c_str(),
                    std::string(a->subCategory).c_str(), std::string(a->timeSignature).c_str(),
                    a->originalTempo, a->accent ? " acc" : "", a->randomSfx ? " sfx" : "",
                    std::string(a->voiceType).c_str());
    std::printf("(%zu shown of %zu total)\n", hits.size(), allArpeggios().size());
    return 0;
}

int cmdArp(int argc, char** argv) {
    if (argc < 2) return usage();
    const auto slot = toLong(argv[0]);
    const auto number = toLong(argv[1]);
    if (!slot || *slot < 1 || *slot > 5) {
        std::fprintf(stderr, "error: slot must be 1..5\n");
        return 1;
    }
    if (!number || *number < 0 || *number > 6633) {
        std::fprintf(stderr, "error: arpeggio number must be 0 (off) or 1..6633\n");
        return 1;
    }
    // ARP SF1..SF5 Assign Type live at 40 30 45/47/49/4B/4D, two bytes each.
    const std::uint8_t low = std::uint8_t(0x45 + (*slot - 1) * 2);
    const auto* p = findParameter(Scope::NormalVoiceCommon, 0x40, 0x30, low);
    if (!p) {
        std::fprintf(stderr, "error: arp assign parameter not found in the table\n");
        return 1;
    }
    Device d;
    if (!connectAndIdentify(d)) return 1;
    d.writeParameter(*p, std::int32_t(*number));
    std::this_thread::sleep_for(std::chrono::milliseconds{40});
    const auto got = d.readParameter(*p);
    const auto* meta = findArpeggio(int(*number));
    std::printf("SF%ld = %ld  %s\n", *slot, got ? long(*got) : *number,
                meta ? std::string(meta->name).c_str() : (*number == 0 ? "off" : "?"));
    if (meta)
        std::printf("  %s/%s  %s  %u bpm  %s\n", std::string(meta->mainCategory).c_str(),
                    std::string(meta->subCategory).c_str(),
                    std::string(meta->timeSignature).c_str(), meta->originalTempo,
                    std::string(meta->voiceType).c_str());
    return 0;
}

/// Reads whichever edit buffer is live and prints its non-reserved parameters.
int cmdDumpState() {
    Device d;
    DeviceInfo info;
    if (!connectAndIdentify(d, &info)) return 1;

    const bool normal = d.readAddress({0x40, 0x00, 0x00}).has_value();
    const bool drum = d.readAddress({0x46, 0x00, 0x00}).has_value();
    const Scope scope = normal ? Scope::NormalVoiceCommon
                       : drum  ? Scope::DrumVoiceCommon
                               : Scope::System;
    std::printf("# %s edit buffer (device %u, firmware %.1f)\n", std::string(toString(scope)).c_str(),
                info.deviceNumber, double(info.firmwareVersion));

    int ok = 0, silent = 0;
    // Name fields are 20 consecutive one-byte ASCII parameters; print them as
    // the string they are rather than twenty decimal numbers.
    std::string pendingName;
    Address nameStart{};
    auto flushName = [&] {
        if (pendingName.empty()) return;
        while (!pendingName.empty() && pendingName.back() == ' ') pendingName.pop_back();
        std::printf("%02X %02X %02X  %-46s \"%s\"\n", nameStart.high, nameStart.mid,
                    nameStart.low, "Name", pendingName.c_str());
        pendingName.clear();
    };

    for (const auto* p : parametersInScope(scope)) {
        if (p->reserved || p->variable != AddressVariable::None) continue;
        const auto v = d.readParameter(*p, 0, std::chrono::milliseconds{60});
        if (!v) {
            ++silent;
            continue;
        }
        ++ok;
        const Address a = p->addressFor();
        if (p->name.rfind("Voice Name", 0) == 0) {
            if (pendingName.empty()) nameStart = a;
            pendingName.push_back(char(*v));
            continue;
        }
        flushName();
        std::printf("%02X %02X %02X  %-46s %ld\n", a.high, a.mid, a.low,
                    std::string(p->name).c_str(), long(*v));
    }
    flushName();
    std::printf("# %d parameters read, %d silent (reserved or inactive)\n", ok, silent);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<char*> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            gPort = argv[++i];
        } else {
            args.push_back(argv[i]);
        }
    }
    if (args.empty()) return usage();

    const std::string cmd = args[0];
    const int n = int(args.size()) - 1;
    char** rest = args.data() + 1;

    if (cmd == "list-midi") return cmdListMidi();
    if (cmd == "identify") return cmdIdentify();
    if (cmd == "status") return cmdStatus();
    if (cmd == "voices") return cmdVoices(n, rest);
    if (cmd == "patch") return cmdPatch(n, rest);
    if (cmd == "patch-name") return cmdPatchName(n, rest);
    if (cmd == "get") return cmdGet(n, rest);
    if (cmd == "set") return cmdSet(n, rest);
    if (cmd == "read") return cmdRead(n, rest);
    if (cmd == "params") return cmdParams(n, rest);
    if (cmd == "arp-list") return cmdArpList(n, rest);
    if (cmd == "arp") return cmdArp(n, rest);
    if (cmd == "dump-state") return cmdDumpState();
    return usage();
}
