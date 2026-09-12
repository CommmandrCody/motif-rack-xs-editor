#include "motifxs/device.hpp"

#include <CoreMIDI/CoreMIDI.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace motifxs {
namespace {

std::string endpointName(MIDIObjectRef obj) {
    CFStringRef cf = nullptr;
    if (MIDIObjectGetStringProperty(obj, kMIDIPropertyDisplayName, &cf) != noErr || !cf)
        return {};
    char buf[256]{};
    CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cf);
    return buf;
}

std::vector<MidiEndpoint> enumerate(bool sources) {
    std::vector<MidiEndpoint> out;
    const ItemCount n = sources ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
        MIDIEndpointRef e = sources ? MIDIGetSource(i) : MIDIGetDestination(i);
        out.push_back({endpointName(e), int(i)});
    }
    return out;
}

}  // namespace

std::vector<MidiEndpoint> listSources() { return enumerate(true); }
std::vector<MidiEndpoint> listDestinations() { return enumerate(false); }

struct Device::Impl {
    MIDIClientRef client{};
    MIDIPortRef in{}, out{};
    MIDIEndpointRef source{}, dest{};
    std::string port;
    std::uint8_t deviceNumber{0};

    RackLock lock;
    SysExReassembler reassembler;
    ChannelMessageParser channels;
    MIDIEndpointRef thru{};
    std::string thruName;
    int thruChannel{-1};              // -1 keeps the source channel
    std::function<void(const Bytes&)> channelListener;
    std::mutex mutex;
    std::condition_variable cv;
    // Inbound messages carry an arrival sequence number. The rack transmits
    // Parameter Changes of its own accord whenever the front panel touches a
    // parameter, and those are byte-identical to a reply -- there is no request
    // id to correlate on. Requiring a reply to have arrived strictly after the
    // request was sent is what stops a panel transmission from satisfying a
    // read. Draining alone leaves a window between drain and send.
    std::vector<std::pair<std::uint64_t, Bytes>> inbox;
    std::uint64_t arrivals{0};
    std::function<void(const Bytes&)> listener;

    ~Impl() { teardown(); }

    /// Releases every CoreMIDI object. Leaving one behind means a later open
    /// adds a second delivery path rather than replacing the first.
    void teardown() {
        if (in && source) MIDIPortDisconnectSource(in, source);
        if (in) MIDIPortDispose(in);
        if (out) MIDIPortDispose(out);
        if (client) MIDIClientDispose(client);
        in = out = 0;
        client = 0;
        outPortRef = 0;
        source = 0;
        dest = 0;
        port.clear();
        std::lock_guard lock(mutex);
        reassembler.reset();
        inbox.clear();
    }

    void onPackets(const MIDIPacketList* list) {
        const MIDIPacket* p = &list->packet[0];
        for (UInt32 i = 0; i < list->numPackets; ++i) {
            forwardChannelMessages({p->data, p->length});
            // The reassembler carries the partial message between packets, so
            // it is state shared with anything else delivering here. Feeding it
            // outside the lock is what turns two deliveries of the same packet
            // into one spliced message twice the length of any real block.
            std::lock_guard lock(mutex);
            auto msgs = reassembler.feed({p->data, p->length});
            for (auto& m : msgs) {
                if (listener) listener(m);
                inbox.emplace_back(++arrivals, std::move(m));
            }
            if (!msgs.empty()) cv.notify_all();
            p = MIDIPacketNext(p);
        }
    }

    /// Extracts channel messages and, if a thru destination is set, relays
    /// them. Runs on the CoreMIDI read thread, so it stays allocation-light
    /// and never blocks on the main mutex for the send itself.
    void forwardChannelMessages(std::span<const std::uint8_t> raw) {
        auto msgs = channels.feed(raw);
        if (msgs.empty()) return;

        MIDIEndpointRef dest{};
        int chan{-1};
        std::function<void(const Bytes&)> listener;
        {
            std::lock_guard lock(mutex);
            dest = thru;
            chan = thruChannel;
            listener = channelListener;
        }
        for (auto& m : msgs) {
            if (listener) listener(m);
            if (!dest || m.empty()) continue;
            Bytes out = m;
            if (chan >= 0 && out[0] < 0xF0)
                out[0] = std::uint8_t((out[0] & 0xF0) | (chan & 0x0F));
            std::byte storage[sizeof(MIDIPacketList) + 64];
            auto* pl = reinterpret_cast<MIDIPacketList*>(storage);
            MIDIPacket* pk = MIDIPacketListInit(pl);
            pk = MIDIPacketListAdd(pl, sizeof(storage), pk, 0, out.size(), out.data());
            if (pk) MIDISend(outPortRef, dest, pl);
        }
    }

    MIDIPortRef outPortRef{};

    /// Waits for a message satisfying `pred` that arrived after `after`,
    /// consuming it. Messages older than `after` cannot be a reply to a request
    /// sent at `after`, so they are ignored (see the note on `inbox`).
    std::optional<Bytes> await(const std::function<bool(const Bytes&)>& pred,
                               std::uint64_t after,
                               std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto scan = [&]() -> std::optional<Bytes> {
            for (auto it = inbox.begin(); it != inbox.end(); ++it) {
                if (it->first > after && pred(it->second)) {
                    Bytes found = std::move(it->second);
                    inbox.erase(it);
                    return found;
                }
            }
            return std::nullopt;
        };
        for (;;) {
            if (auto hit = scan()) return hit;
            if (cv.wait_until(lock, deadline) == std::cv_status::timeout)
                return scan();
        }
    }

    /// Drops buffered messages and returns the arrival counter, so a caller can
    /// require replies to be strictly newer than this moment.
    std::uint64_t mark() {
        std::lock_guard lock(mutex);
        inbox.clear();
        return arrivals;
    }
};

Device::Device() : impl_(std::make_unique<Impl>()) {}
Device::~Device() = default;
Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;

bool Device::isOpen() const { return impl_ && impl_->source && impl_->dest; }
const std::string& Device::portName() const { return impl_->port; }
void Device::setDeviceNumber(std::uint8_t n) { impl_->deviceNumber = n & 0x0F; }
std::uint8_t Device::deviceNumber() const { return impl_->deviceNumber; }

void Device::setSysExListener(std::function<void(const Bytes&)> fn) {
    std::lock_guard lock(impl_->mutex);
    impl_->listener = std::move(fn);
}

bool Device::open(const std::string& wanted, std::string* error,
                  const std::string& clientName) {
    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };

    // Opening an already-open device used to build a second CoreMIDI client and
    // connect the source again while the first connection was still live. Every
    // packet then arrived twice, and the rack lock did not catch it because a
    // lock already held by this process is granted again. The duplicate stream
    // is what shredded bulk transfers -- two deliveries of the same packet
    // feeding one reassembler produce messages spliced together at twice the
    // length of anything the rack sends.
    if (isOpen()) {
        if (wanted.empty() || wanted == impl_->port) return true;
        close();
    }
    impl_->teardown();

    // One client at a time. CoreMIDI merges every client's output to a
    // destination, so a second one's Parameter Requests land inside this one's
    // bulk transfers and the rack rejects them.
    std::string holder;
    if (!impl_->lock.acquire(clientName, &holder)) {
        if (error) *error = "the rack is already in use by " + holder;
        return false;
    }

    auto pick = [&](bool sources) -> std::pair<MIDIEndpointRef, std::string> {
        const ItemCount n = sources ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
        for (ItemCount i = 0; i < n; ++i) {
            MIDIEndpointRef e = sources ? MIDIGetSource(i) : MIDIGetDestination(i);
            const std::string name = endpointName(e);
            if (wanted.empty()) {
                // Only Port1 carries SysEx; prefer it explicitly.
                if (name.find("MOTIF") != std::string::npos &&
                    name.find("Port1") != std::string::npos)
                    return {e, name};
            } else if (name == wanted) {
                return {e, name};
            }
        }
        return {0, {}};
    };

    if (MIDIClientCreate(CFSTR("motifxs"), nullptr, nullptr, &impl_->client) != noErr) {
        impl_->lock.release();
        return fail("could not create a CoreMIDI client");
    }

    auto [src, srcName] = pick(true);
    auto [dst, dstName] = pick(false);
    if (!src || !dst) {
        impl_->lock.release();
        return fail(wanted.empty() ? "no MOTIF-RACK XS Port1 endpoint found"
                                   : "named MIDI port not found");
    }

    Impl* self = impl_.get();
    auto giveUp = [&](const char* m) {
        impl_->teardown();
        impl_->lock.release();
        return fail(m);
    };
    if (MIDIInputPortCreateWithBlock(
            impl_->client, CFSTR("in"), &impl_->in,
            ^(const MIDIPacketList* list, void*) { self->onPackets(list); }) != noErr)
        return giveUp("could not create the MIDI input port");
    if (MIDIOutputPortCreate(impl_->client, CFSTR("out"), &impl_->out) != noErr)
        return giveUp("could not create the MIDI output port");
    if (MIDIPortConnectSource(impl_->in, src, nullptr) != noErr)
        return giveUp("could not connect to the MIDI source");

    impl_->source = src;
    impl_->dest = dst;
    impl_->port = srcName;
    impl_->outPortRef = impl_->out;
    return true;
}

void Device::close() {
    if (!impl_) return;
    impl_->lock.release();
    impl_->teardown();
}

void Device::send(std::span<const std::uint8_t> raw) {
    if (!isOpen() || raw.empty()) return;
    // MIDIPacketList needs room for the data plus its own header.
    const std::size_t bytes = sizeof(MIDIPacketList) + raw.size() + 128;
    std::vector<std::byte> storage(bytes);
    auto* list = reinterpret_cast<MIDIPacketList*>(storage.data());
    MIDIPacket* p = MIDIPacketListInit(list);
    p = MIDIPacketListAdd(list, bytes, p, 0, raw.size(), raw.data());
    if (p) MIDISend(impl_->out, impl_->dest, list);
}

std::optional<DeviceInfo> Device::identify(std::chrono::milliseconds perDevice) {
    if (!isOpen()) return std::nullopt;
    // The documented omni form (7F) does not reply on real hardware, so sweep.
    for (std::uint8_t n = 0; n < 16; ++n) {
        const auto mark = impl_->mark();
        send(identityRequest(n));
        auto reply = impl_->await(
            [](const Bytes& m) { return parseIdentityReply(m).has_value(); }, mark, perDevice);
        if (!reply) continue;
        const auto id = parseIdentityReply(*reply);
        if (!id || !id->isMotifRackXs()) continue;
        impl_->deviceNumber = n;
        return DeviceInfo{n, id->version, impl_->port};
    }
    return std::nullopt;
}

std::optional<Bytes> Device::readAddress(Address a, std::chrono::milliseconds timeout) {
    if (!isOpen()) return std::nullopt;
    const auto mark = impl_->mark();
    send(parameterRequest(impl_->deviceNumber, a));
    auto reply = impl_->await(
        [a](const Bytes& m) {
            const auto pc = parseParameterChange(m);
            return pc && pc->address == a;
        },
        mark, timeout);
    if (!reply) return std::nullopt;  // reserved, mid-parameter, or wrong mode
    return parseParameterChange(*reply)->data;
}

std::optional<Bytes> Device::requestBulk(Address a, std::chrono::milliseconds timeout) {
    if (!isOpen()) return std::nullopt;
    const auto mark = impl_->mark();
    send(dumpRequest(impl_->deviceNumber, a));
    auto reply = impl_->await(
        [a](const Bytes& m) {
            const auto bulk = parseBulkDump(m);
            return bulk && bulk->address == a;
        },
        mark, timeout);
    if (!reply) return std::nullopt;
    const auto bulk = parseBulkDump(*reply);
    if (!bulk || !bulk->checksumOk) return std::nullopt;
    return bulk->data;
}

std::optional<std::int32_t> Device::readParameter(const Parameter& p, int index,
                                                  std::chrono::milliseconds timeout) {
    auto data = readAddress(p.addressFor(index), timeout);
    if (!data) return std::nullopt;
    return decodeValue(p, *data);
}

bool Device::writeParameter(const Parameter& p, std::int32_t value, int index) {
    if (!isOpen()) return false;
    const Bytes data = encodeValue(p, value);
    send(parameterChange(impl_->deviceNumber, p.addressFor(index), data));
    return true;  // Parameter Change is never acknowledged; see docs/state-sync.md
}

void Device::selectVoice(std::uint8_t msb, std::uint8_t lsb, std::uint8_t program,
                         std::uint8_t channel) {
    const std::uint8_t cc = std::uint8_t(0xB0 | (channel & 0x0F));
    const std::uint8_t pc = std::uint8_t(0xC0 | (channel & 0x0F));
    const Bytes m{cc, 0x00, std::uint8_t(msb & 0x7F),
                  cc, 0x20, std::uint8_t(lsb & 0x7F),
                  pc, std::uint8_t(program & 0x7F)};
    send(m);
}

void Device::quiesce(std::chrono::milliseconds quiet, std::chrono::milliseconds limit) {
    if (!isOpen()) return;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::uint64_t last = 0;
    {
        std::lock_guard lock(impl_->mutex);
        last = impl_->arrivals;
    }
    auto quietSince = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        std::uint64_t now = 0;
        {
            std::lock_guard lock(impl_->mutex);
            now = impl_->arrivals;
        }
        if (now != last) {
            last = now;
            quietSince = std::chrono::steady_clock::now();
            continue;
        }
        if (std::chrono::steady_clock::now() - quietSince >= quiet) break;
    }
    std::lock_guard lock(impl_->mutex);
    impl_->inbox.clear();
}

void Device::setChannelListener(std::function<void(const Bytes&)> fn) {
    std::lock_guard lock(impl_->mutex);
    impl_->channelListener = std::move(fn);
}

bool Device::setThru(const std::string& destinationName, std::string* error) {
    if (destinationName.empty()) {
        std::lock_guard lock(impl_->mutex);
        impl_->thru = 0;
        impl_->thruName.clear();
        return true;
    }
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i) {
        MIDIEndpointRef e = MIDIGetDestination(i);
        if (endpointName(e) == destinationName) {
            std::lock_guard lock(impl_->mutex);
            impl_->thru = e;
            impl_->thruName = destinationName;
            return true;
        }
    }
    if (error) *error = "destination not found: " + destinationName;
    return false;
}

std::string Device::thruName() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->thruName;
}

void Device::setThruChannel(int channel) {
    std::lock_guard lock(impl_->mutex);
    impl_->thruChannel = channel;
}

void Device::silenceThru() {
    MIDIEndpointRef dest{};
    {
        std::lock_guard lock(impl_->mutex);
        dest = impl_->thru;
    }
    if (!dest) return;
    Bytes cc;
    for (std::uint8_t ch = 0; ch < 16; ++ch) {
        const std::uint8_t st = std::uint8_t(0xB0 | ch);
        cc.insert(cc.end(), {st, 0x78, 0x00});
        cc.insert(cc.end(), {st, 0x7B, 0x00});
    }
    const std::size_t bytes = sizeof(MIDIPacketList) + cc.size() + 128;
    std::vector<std::byte> storage(bytes);
    auto* pl = reinterpret_cast<MIDIPacketList*>(storage.data());
    MIDIPacket* pk = MIDIPacketListInit(pl);
    pk = MIDIPacketListAdd(pl, bytes, pk, 0, cc.size(), cc.data());
    if (pk) MIDISend(impl_->out, dest, pl);
}

void Device::panic() {
    if (!isOpen()) return;

    // Notes first, so the sound stops as fast as possible.
    Bytes cc;
    for (std::uint8_t ch = 0; ch < 16; ++ch) {
        const std::uint8_t status = std::uint8_t(0xB0 | ch);
        cc.insert(cc.end(), {status, 0x78, 0x00});   // All Sound Off
        cc.insert(cc.end(), {status, 0x7B, 0x00});   // All Notes Off
    }
    send(cc);
    silenceThru();

    // Then stop the arpeggiators, or a held arp simply retriggers.
    // ARP Switch 38 pp 00 -> off(0); ARP Hold 38 pp 07 -> off(1), since the
    // encoding is 0 sync-off, 1 off, 2 on.
    for (std::uint8_t part = 0; part < 16; ++part) {
        const std::uint8_t off = 0x00;
        const std::uint8_t holdOff = 0x01;
        send(parameterChange(impl_->deviceNumber, {0x38, part, 0x00}, {&off, 1}));
        send(parameterChange(impl_->deviceNumber, {0x38, part, 0x07}, {&holdOff, 1}));
    }
}

}  // namespace motifxs
