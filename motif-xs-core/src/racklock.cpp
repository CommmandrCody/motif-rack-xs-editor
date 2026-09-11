#include "motifxs/racklock.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace motifxs {
namespace {

std::string lockDirectory() {
    const char* home = std::getenv("HOME");
    std::string dir = home ? std::string(home) : std::string("/tmp");
    dir += "/Library/Application Support/MotifRackXS";
    return dir;
}

}  // namespace

std::string RackLock::path() { return lockDirectory() + "/rack.lock"; }

RackLock::~RackLock() { release(); }

bool RackLock::acquire(const std::string& description, std::string* holder) {
    if (fd_ >= 0) return true;

    ::mkdir(lockDirectory().c_str(), 0755);           // harmless if it exists
    const std::string file = path();

    const int fd = ::open(file.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        if (holder) *holder = "could not open " + file;
        return false;
    }

    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        // Someone else owns it. The file says who, for a message worth reading.
        if (holder) {
            std::vector<char> buf(256, 0);
            const auto n = ::pread(fd, buf.data(), buf.size() - 1, 0);
            *holder = n > 0 ? std::string(buf.data()) : std::string("another client");
        }
        ::close(fd);
        return false;
    }

    ::ftruncate(fd, 0);
    const std::string who = description + " (pid " + std::to_string(::getpid()) + ")";
    const auto written = ::pwrite(fd, who.data(), who.size(), 0);
    (void)written;
    fd_ = fd;
    return true;
}

void RackLock::release() {
    if (fd_ < 0) return;
    ::ftruncate(fd_, 0);
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
}

}  // namespace motifxs
