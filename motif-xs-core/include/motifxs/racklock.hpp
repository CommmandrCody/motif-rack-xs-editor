// Exclusive ownership of the rack's MIDI port, across processes.
#pragma once

#include <string>

namespace motifxs {

/// Only one client may talk to the rack at a time.
///
/// The rack has a single MIDI port and no arbitration, and CoreMIDI merges the
/// output of every client that opens a destination. So a second client's
/// Parameter Requests land *inside* another's bulk transfer, splitting the
/// stream -- the rack then rejects the whole sequence with "illegal bulk data"
/// on its display, and the transfer is silently half-applied. That is not a
/// hypothetical: it is what happens when the standalone app is left open while
/// the plugin restores a project.
///
/// Held as an advisory lock on a file, so the operating system releases it if
/// the owning process dies. A crash cannot leave the rack permanently locked.
class RackLock {
public:
    RackLock() = default;
    ~RackLock();
    RackLock(const RackLock&) = delete;
    RackLock& operator=(const RackLock&) = delete;

    /// Takes exclusive ownership. On failure `holder` describes who has it.
    [[nodiscard]] bool acquire(const std::string& description, std::string* holder = nullptr);
    void release();
    [[nodiscard]] bool held() const { return fd_ >= 0; }

    /// Where the lock lives, for diagnostics.
    [[nodiscard]] static std::string path();

private:
    int fd_{-1};
};

}  // namespace motifxs
