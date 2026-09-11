// CoreMIDI device abstraction.
// MIDI I/O never happens on an audio thread; see docs/vst-architecture.md.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "motifxs/parameters.hpp"
#include "motifxs/racklock.hpp"
#include "motifxs/sysex.hpp"

namespace motifxs {

struct MidiEndpoint {
    std::string name;
    int index{};
};

/// Enumerates CoreMIDI endpoints.
[[nodiscard]] std::vector<MidiEndpoint> listSources();
[[nodiscard]] std::vector<MidiEndpoint> listDestinations();

struct DeviceInfo {
    std::uint8_t deviceNumber{};
    float firmwareVersion{};
    std::string portName;
};

/// An open connection to one MOTIF-RACK XS port.
///
/// Only Port1 carries SysEx on real hardware; ports 2-4 are note-only.
class Device {
public:
    Device();
    ~Device();
    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;

    /// Opens the named port for both directions. Empty name auto-selects the
    /// first endpoint containing "MOTIF".
    ///
    /// Takes exclusive ownership of the rack first; if another client already
    /// has it this fails and `error` names the holder. One client at a time is
    /// a hard requirement, not a preference -- see RackLock.
    ///
    /// `clientName` appears in that message to whoever is refused next.
    [[nodiscard]] bool open(const std::string& portName, std::string* error = nullptr,
                            const std::string& clientName = "Motif Rack XS");
    void close();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] const std::string& portName() const;

    /// Sweeps device numbers 0..15 with an Identity Request. The documented
    /// omni form does not reply on real hardware, so this is a sweep, not a
    /// broadcast. See docs/protocol.md.
    [[nodiscard]] std::optional<DeviceInfo> identify(
        std::chrono::milliseconds perDevice = std::chrono::milliseconds{120});

    void setDeviceNumber(std::uint8_t n);
    [[nodiscard]] std::uint8_t deviceNumber() const;

    void send(std::span<const std::uint8_t> raw);

    /// Sends a Parameter Request and waits for the matching Parameter Change.
    /// nullopt means the unit stayed silent, which is ambiguous: reserved,
    /// mid-parameter, or wrong mode. See docs/state-sync.md.
    [[nodiscard]] std::optional<Bytes> readAddress(
        Address, std::chrono::milliseconds timeout = std::chrono::milliseconds{150});

    /// Requests a whole block by bulk dump. Far cheaper than reading a block
    /// one parameter at a time: a 61-byte Multi Part arrives in one message.
    /// Returns nullopt on timeout or checksum failure -- a corrupted block
    /// silently poisoning the model is worse than a failed read.
    [[nodiscard]] std::optional<Bytes> requestBulk(
        Address, std::chrono::milliseconds timeout = std::chrono::milliseconds{400});

    [[nodiscard]] std::optional<std::int32_t> readParameter(
        const Parameter&, int index = 0,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{150});

    bool writeParameter(const Parameter&, std::int32_t value, int index = 0);

    /// Bank Select MSB/LSB + Program Change on the given channel (0-based).
    void selectVoice(std::uint8_t msb, std::uint8_t lsb, std::uint8_t program, std::uint8_t channel = 0);

    /// Silences everything: All Sound Off and All Notes Off on all 16 channels,
    /// then ARP Switch off and ARP Hold off on all 16 Multi parts.
    ///
    /// An arpeggio with Hold on latches: one note starts it and releasing the
    /// key does not stop it, so notes-off alone is not enough. Any controller
    /// for this rack needs this within reach.
    void panic();

    /// Called for every inbound SysEx message, on the MIDI read thread.
    void setSysExListener(std::function<void(const Bytes&)>);

    /// Called for every inbound channel message, on the MIDI read thread.
    /// This is where arpeggiator output arrives when ARP MIDI Out is on.
    void setChannelListener(std::function<void(const Bytes&)>);

    /// Forwards the rack's channel messages to another CoreMIDI destination,
    /// so an arpeggio playing on the Motif can drive a second instrument.
    ///
    /// SysEx and realtime are never forwarded: the editor's own parameter
    /// traffic must not reach the other device, and clock is the host's job.
    /// Pass an empty name to stop forwarding.
    [[nodiscard]] bool setThru(const std::string& destinationName, std::string* error = nullptr);
    [[nodiscard]] std::string thruName() const;

    /// Rewrites the channel of forwarded messages. -1 keeps the original.
    void setThruChannel(int channel);

    /// All Notes Off + All Sound Off on the thru destination, all 16 channels.
    void silenceThru();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace motifxs
