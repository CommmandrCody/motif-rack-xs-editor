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
    [[nodiscard]] bool open(const std::string& portName, std::string* error = nullptr);
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

    [[nodiscard]] std::optional<std::int32_t> readParameter(
        const Parameter&, int index = 0,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{150});

    bool writeParameter(const Parameter&, std::int32_t value, int index = 0);

    /// Bank Select MSB/LSB + Program Change on the given channel (0-based).
    void selectVoice(std::uint8_t msb, std::uint8_t lsb, std::uint8_t program, std::uint8_t channel = 0);

    /// Called for every inbound SysEx message, on the MIDI read thread.
    void setSysExListener(std::function<void(const Bytes&)>);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace motifxs
