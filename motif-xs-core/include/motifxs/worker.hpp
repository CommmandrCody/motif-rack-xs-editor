// Off-thread device access.
//
// MIDI I/O must never happen on a UI or audio thread (docs/vst-architecture.md).
// This owns the Device and a single worker thread; callers post work and get
// results back through callbacks. Both the standalone app and the plugin use
// it, so the threading policy lives in one place.
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "motifxs/device.hpp"
#include "motifxs/parameters.hpp"

namespace motifxs {

class DeviceWorker {
public:
    DeviceWorker();
    ~DeviceWorker();

    DeviceWorker(const DeviceWorker&) = delete;
    DeviceWorker& operator=(const DeviceWorker&) = delete;

    /// Opens a port and identifies the rack. `done` runs on the worker thread.
    void open(std::string portName, std::function<void(bool, std::string, DeviceInfo)> done,
              std::string clientName = "Motif Rack XS");
    void close();
    [[nodiscard]] bool isOpen() const { return open_.load(); }
    [[nodiscard]] DeviceInfo info() const;

    /// Queues a parameter write. Repeated writes to the same address before the
    /// next flush collapse to the latest value, so dragging a knob cannot
    /// outrun the rack's input buffer.
    void setParameter(const Parameter&, int index, std::int32_t value);

    /// Reads a parameter; `done` runs on the worker thread with nullopt if the
    /// rack stayed silent (reserved, mid-parameter, or wrong mode).
    void readParameter(const Parameter&, int index,
                       std::function<void(std::optional<std::int32_t>)> done);

    /// Runs arbitrary work against the device on the worker thread.
    void post(std::function<void(Device&)> job);

    /// Last value seen for an address, from either a read or a write.
    [[nodiscard]] std::optional<std::int32_t> cached(Address) const;
    void setStateListener(std::function<void(Address, std::int32_t)>);

    /// Milliseconds between coalesced write flushes.
    void setFlushInterval(std::chrono::milliseconds ms) { flush_ = ms; }

    /// Counts changes made *to* the rack. Reads never bump it, so a caller can
    /// tell "the user edited something" from "we polled the device", which is
    /// what decides whether a saved project is stale.
    [[nodiscard]] std::uint64_t changeCount() const { return changes_.load(); }

    /// For changes that bypass setParameter -- applying a custom patch, or a
    /// raw Bank Select and Program Change.
    void noteExternalChange() { changes_.fetch_add(1); }

private:
    void run();
    void flushWrites();
    void note(Address, std::int32_t);

    Device device_;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<std::uint64_t> changes_{0};
    std::atomic<bool> open_{false};
    /// An open takes a moment and runs on the worker thread, so isOpen() is
    /// still false while one is in flight. Without this, several instances
    /// asking at once each queue their own.
    std::atomic<bool> opening_{false};
    std::chrono::milliseconds flush_{15};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::function<void(Device&)>> jobs_;
    std::map<Address, std::pair<const Parameter*, std::int32_t>> pending_;
    std::map<Address, std::int32_t> cache_;
    std::function<void(Address, std::int32_t)> listener_;
    DeviceInfo info_;
};

/// The one connection every plugin instance in a host process shares.
///
/// CoreMIDI happily lets several clients write to the same destination, so two
/// plugin instances each opening their own would interleave SysEx mid-message
/// and corrupt each other -- and both would race on the reply. One connection,
/// reference counted, released when the last instance goes.
///
/// Two instances addressing different Parts is legitimate and useful; they
/// simply have to share the wire.
[[nodiscard]] std::shared_ptr<DeviceWorker> sharedWorker();

}  // namespace motifxs
