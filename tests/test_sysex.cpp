// Core tests. No hardware required.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "motifxs/catalog.hpp"
#include "motifxs/parameters.hpp"
#include "motifxs/state.hpp"
#include "motifxs/sysex.hpp"

using namespace motifxs;

namespace {
int gFailures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++gFailures;
    }
}

std::string hex(const Bytes& b) {
    std::string s;
    char buf[4];
    for (auto x : b) {
        std::snprintf(buf, sizeof(buf), "%02X", x);
        s += buf;
        s += ' ';
    }
    if (!s.empty()) s.pop_back();
    return s;
}

void testFraming() {
    // Captured from the reference unit: a request for the System block.
    check(hex(parameterRequest(0, {0x00, 0x00, 0x00})) == "F0 43 30 7F 03 00 00 00 F7",
          "parameter request framing");
    // Master Note Shift = 0x41, the exact write verified on hardware.
    const Bytes one{0x41};
    check(hex(parameterChange(0, {0x00, 0x00, 0x01}, one)) == "F0 43 10 7F 03 00 00 01 41 F7",
          "parameter change framing (no checksum)");
    check(hex(dumpRequest(0, {0x40, 0x00, 0x00})) == "F0 43 20 7F 03 40 00 00 F7",
          "dump request framing");
    check(hex(identityRequest(0)) == "F0 7E 00 06 01 F7", "identity request framing");
    // device number must land in the low nibble
    check(parameterRequest(5, {}) [2] == 0x35, "device number nibble");
}

void testChecksum() {
    // The rule: byte count + address + data + checksum == 0 in the low 7 bits.
    const Bytes data{0x01, 0x02, 0x03};
    const Bytes msg = bulkDump(0, {0x40, 0x00, 0x00}, data);
    int sum = 0;
    for (std::size_t i = 5; i + 1 < msg.size(); ++i) sum += msg[i];  // count..checksum
    check((sum & 0x7F) == 0, "bulk dump checksum sums to zero");
    check(msg.front() == 0xF0 && msg.back() == 0xF7, "bulk dump framing");
    check(msg[5] == 0x00 && msg[6] == 0x03, "bulk dump byte count");
}

void testRoundTrip() {
    const Bytes data{0x11, 0x22};
    const Bytes msg = parameterChange(3, {0x40, 0x30, 0x45}, data);
    const auto parsed = parseParameterChange(msg);
    check(parsed.has_value(), "parameter change parses");
    if (parsed) {
        check(parsed->device == 3, "round trip device");
        check((parsed->address == Address{0x40, 0x30, 0x45}), "round trip address");
        check(parsed->data == data, "round trip data");
    }
    // an identity reply must not parse as a parameter change
    check(!parseParameterChange(Bytes{0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x43, 0xF7}).has_value(),
          "identity reply is not a parameter change");
}

void testIdentityReply() {
    // Exact bytes captured from the reference unit.
    const Bytes reply{0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x43, 0x00, 0x41,
                      0x39, 0x06, 0x00, 0x00, 0x00, 0x7F, 0xF7};
    const auto id = parseIdentityReply(reply);
    check(id.has_value(), "identity reply parses");
    if (id) {
        check(id->isMotifRackXs(), "identified as MOTIF-RACK XS");
        check(id->version == 1.0f, "firmware version 1.0");
    }
}

void testReassembler() {
    SysExReassembler r;
    // The unit floods Active Sensing, including between SysEx bytes.
    const Bytes stream{0xFE, 0xF0, 0x43, 0x10, 0xFE, 0x7F, 0x03,
                       0x00, 0x00, 0x00, 0x7D, 0xF7, 0xFE};
    const auto msgs = r.feed(stream);
    check(msgs.size() == 1, "one message recovered from a noisy stream");
    if (msgs.size() == 1) {
        check(hex(msgs[0]) == "F0 43 10 7F 03 00 00 00 7D F7", "realtime bytes stripped");
    }
    // split across packet boundaries
    SysExReassembler s;
    check(s.feed(Bytes{0xF0, 0x43, 0x10}).empty(), "partial message yields nothing");
    check(s.feed(Bytes{0x7F, 0x03, 0x00, 0x00, 0x00, 0x7D, 0xF7}).size() == 1,
          "message completes across packets");
}

void testEncoding() {
    const auto* arp = findParameter(Scope::NormalVoiceCommon, 0x40, 0x30, 0x45);
    check(arp != nullptr, "ARP SF1 Assign Type is in the table");
    if (arp) {
        check(arp->size == 2 && arp->encoding == Encoding::MsbLsb, "arp assign is 2-byte msb/lsb");
        // 6633 is the highest arpeggio; verified round-tripping on hardware.
        const Bytes enc = encodeValue(*arp, 6633);
        check(hex(enc) == "33 69", "6633 encodes to 33 69");
        check(decodeValue(*arp, enc) == 6633, "6633 decodes back");
        check(decodeValue(*arp, encodeValue(*arp, 3861)) == 3861, "3861 round trips");
    }
    // Master Tune is four nibbles, and 0x0400 is the centre value read from hardware.
    const auto* tune = findParameter(Scope::System, 0x00, 0x00, 0x02);
    check(tune && tune->size == 4 && tune->encoding == Encoding::Nibbles, "master tune is nibble-packed");
    if (tune) {
        check(hex(encodeValue(*tune, 0x0400)) == "00 04 00 00", "nibble encoding");
        check(decodeValue(*tune, Bytes{0x00, 0x04, 0x00, 0x00}) == 0x0400, "nibble decoding");
    }
}

void testVariableAddresses() {
    const auto* filter = findParameter(Scope::NormalVoiceElement, 0x42, 0x00, 0x00);
    check(filter != nullptr, "element filter type is in the table");
    if (filter) {
        check(filter->variable == AddressVariable::Element, "element address is variable");
        check((filter->addressFor(3) == Address{0x42, 0x03, 0x00}), "element 3 resolves");
        check((filter->addressFor(7) == Address{0x42, 0x07, 0x00}), "element 7 resolves");
    }
    const auto* vol = findParameter(Scope::MultiPart, 0x37, 0x00, 0x0E);
    check(vol != nullptr, "multi part volume is in the table");
    if (vol) check((vol->addressFor(15) == Address{0x37, 0x0F, 0x0E}), "part 16 resolves");
}

void testCatalogs() {
    check(allVoices().size() == 1217, "1217 voices");
    check(allArpeggios().size() == 6633, "6633 arpeggios");

    // Verified on hardware: PRE1 program 0 reads back "Full Concert Grand".
    const auto* v = findVoice(63, 0, 0);
    check(v && v->name == "Full Concert Grand", "PRE1 pc=0 is Full Concert Grand");

    const auto* a = findArpeggio(6633);
    check(a && a->name == "Mute 9/8", "arp 6633 is Mute 9/8");
    const auto* a1 = findArpeggio(1);
    check(a1 && a1->mainCategory == "ApKb", "arp 1 category");

    // Accent belongs to drums; random SFX to the two Mega Voice categories.
    std::size_t accent = 0, sfx = 0, accentNonDrum = 0, sfxNonMega = 0;
    for (const auto& x : allArpeggios()) {
        if (x.accent) {
            ++accent;
            if (x.mainCategory != "DrPc") ++accentNonDrum;
        }
        if (x.randomSfx) {
            ++sfx;
            if (x.mainCategory != "GtMG" && x.mainCategory != "BaMG") ++sfxNonMega;
        }
    }
    check(accent == 1909 && accentNonDrum == 0, "accent flags are exactly the drum category");
    check(sfx == 1012 && sfxNonMega == 0, "random SFX flags are exactly the Mega Voice categories");

    check(!searchVoices("grand", 5).empty(), "voice search finds 'grand'");
    ArpFilter f;
    f.mainCategory = "DrPc";
    f.tempoMin = 120;
    f.tempoMax = 130;
    check(!searchArpeggios(f, 5).empty(), "arp filter by category and tempo");
}

void testParameterTable() {
    check(allParameters().size() == 1136, "1136 parameters");
    // Two parameters in the otherwise-undocumented Sequencer Setup block,
    // established by hardware experiment. Their same-named counterparts at
    // 00 6n are Quick Setup template values, not the live settings.
    const auto* sync = findParameterById("sequencer_setup_midi_sync");
    check(sync != nullptr, "discovered MIDI Sync is merged");
    if (sync) {
        check((sync->addressFor() == Address{0x00, 0x05, 0x0B}), "MIDI Sync at 00 05 0B");
        check(sync->min == 0 && sync->max == 2, "MIDI Sync has three values");
    }
    const auto* clk = findParameterById("sequencer_setup_midi_clock_out");
    check(clk != nullptr, "discovered MIDI Clock Out is merged");
    if (clk) check(clk->min == 0 && clk->max == 1, "MIDI Clock Out has two values");
    // the Quick Setup template copies must still be distinct
    check(findParameterById("system_midi_sync") != nullptr, "Quick Setup MIDI Sync kept");
    // Mode Change lives at 0A 00 01, not the 0A 00 00 in the p62 memory map.
    check(findParameter(Scope::ModeChange, 0x0A, 0x00, 0x01) != nullptr,
          "mode change at 0A 00 01");
    // The two switches that silently disable patch selection.
    check(findParameter(Scope::System, 0x00, 0x00, 0x14) != nullptr, "bank select receive switch");
    check(findParameter(Scope::System, 0x00, 0x00, 0x15) != nullptr, "program change receive switch");
    // 73 drum keys, 8 elements.
    const auto* key = findParameter(Scope::DrumVoiceKey, 0x47, 0x00, 0x00);
    check(key && key->variable == AddressVariable::Element, "drum key address is variable");
    if (key) check((key->addressFor(72) == Address{0x47, 0x48, 0x00}), "drum key 72 resolves");
}

void testBulkAndState() {
    // A bulk dump must survive its own checksum, and a corrupted one must be
    // rejected rather than silently trusted.
    const Bytes data{0x10, 0x20, 0x30, 0x40};
    Bytes msg = bulkDump(0, {0x37, 0x00, 0x00}, data);
    const auto parsed = parseBulkDump(msg);
    check(parsed.has_value(), "bulk dump parses");
    if (parsed) {
        check(parsed->checksumOk, "checksum verifies");
        check((parsed->address == Address{0x37, 0x00, 0x00}), "bulk address");
        check(parsed->data == data, "bulk payload");
    }
    msg[11] ^= 0x01;                       // corrupt one data byte
    const auto bad = parseBulkDump(msg);
    check(bad && !bad->checksumOk, "a corrupted bulk dump is flagged, not trusted");

    // State serialisation must round-trip exactly: this is what a DAW project
    // stores, so a lossy trip would silently lose the user's setup.
    State st;
    st.deviceNumber = 3;
    st.firmware = 1.0f;
    st.capturedAt = "2026-01-01T00:00:00Z";
    st.note = "a note with spaces";
    st.messages.push_back(bulkDump(0, {0x0E, 0x5F, 0x00}, {}));
    st.messages.push_back(bulkDump(0, {0x37, 0x00, 0x00}, data));
    st.messages.push_back(bulkDump(0, {0x0F, 0x5F, 0x00}, {}));

    const auto text = toText(st);
    const auto back = fromText(text);
    check(back.has_value(), "state text parses");
    if (back) {
        check(back->messages.size() == st.messages.size(), "message count round-trips");
        check(back->messages == st.messages, "messages round-trip byte for byte");
        check(back->deviceNumber == 3, "device number round-trips");
        check(back->note == st.note, "note with spaces round-trips");
    }
    check(!fromText("not a state file").has_value(), "junk is rejected");
}

}  // namespace

int main() {
    std::puts("motif-xs-core tests");
    testFraming();
    testChecksum();
    testRoundTrip();
    testIdentityReply();
    testReassembler();
    testEncoding();
    testVariableAddresses();
    testCatalogs();
    testParameterTable();
    testBulkAndState();
    if (gFailures == 0) {
        std::puts("all tests passed");
        return 0;
    }
    std::printf("%d test(s) failed\n", gFailures);
    return 1;
}
