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

    SysExReassembler reassembler;
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

    ~Impl() {
        if (in) MIDIPortDispose(in);
        if (out) MIDIPortDispose(out);
        if (client) MIDIClientDispose(client);
    }

    void onPackets(const MIDIPacketList* list) {
        const MIDIPacket* p = &list->packet[0];
        std::vector<Bytes> msgs;
        for (UInt32 i = 0; i < list->numPackets; ++i) {
            msgs = reassembler.feed({p->data, p->length});
            if (!msgs.empty()) {
                std::lock_guard lock(mutex);
                for (auto& m : msgs) {
                    if (listener) listener(m);
                    inbox.emplace_back(++arrivals, std::move(m));
                }
                cv.notify_all();
            }
            p = MIDIPacketNext(p);
        }
    }

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

bool Device::open(const std::string& wanted, std::string* error) {
    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };

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

    if (MIDIClientCreate(CFSTR("motifxs"), nullptr, nullptr, &impl_->client) != noErr)
        return fail("could not create a CoreMIDI client");

    auto [src, srcName] = pick(true);
    auto [dst, dstName] = pick(false);
    if (!src || !dst)
        return fail(wanted.empty() ? "no MOTIF-RACK XS Port1 endpoint found"
                                   : "named MIDI port not found");

    Impl* self = impl_.get();
    if (MIDIInputPortCreateWithBlock(
            impl_->client, CFSTR("in"), &impl_->in,
            ^(const MIDIPacketList* list, void*) { self->onPackets(list); }) != noErr)
        return fail("could not create the MIDI input port");
    if (MIDIOutputPortCreate(impl_->client, CFSTR("out"), &impl_->out) != noErr)
        return fail("could not create the MIDI output port");
    if (MIDIPortConnectSource(impl_->in, src, nullptr) != noErr)
        return fail("could not connect to the MIDI source");

    impl_->source = src;
    impl_->dest = dst;
    impl_->port = srcName;
    return true;
}

void Device::close() {
    if (!impl_) return;
    if (impl_->in && impl_->source) MIDIPortDisconnectSource(impl_->in, impl_->source);
    impl_->source = 0;
    impl_->dest = 0;
    impl_->port.clear();
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

}  // namespace motifxs
