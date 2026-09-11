#include "motifxs/worker.hpp"

namespace motifxs {

DeviceWorker::DeviceWorker() { thread_ = std::thread([this] { run(); }); }

DeviceWorker::~DeviceWorker() {
    quit_ = true;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

DeviceInfo DeviceWorker::info() const {
    std::lock_guard lock(mutex_);
    return info_;
}

void DeviceWorker::post(std::function<void(Device&)> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
}

void DeviceWorker::open(std::string portName,
                        std::function<void(bool, std::string, DeviceInfo)> done,
                        std::string clientName) {
    post([this, portName, done, clientName](Device& d) {
        std::string err;
        if (!d.open(portName, &err, clientName)) {
            open_ = false;
            done(false, err, {});
            return;
        }
        auto id = d.identify();
        if (!id) {
            d.close();
            open_ = false;
            done(false, "no reply from a MOTIF-RACK XS on this port", {});
            return;
        }
        {
            std::lock_guard lock(mutex_);
            info_ = *id;
        }
        open_ = true;
        done(true, {}, *id);
    });
}

void DeviceWorker::close() {
    post([this](Device& d) {
        d.close();
        open_ = false;
    });
}

void DeviceWorker::setParameter(const Parameter& p, int index, std::int32_t value) {
    const Address a = p.addressFor(index);
    {
        std::lock_guard lock(mutex_);
        pending_[a] = {&p, value};   // later writes replace earlier ones
    }
    cv_.notify_all();
}

void DeviceWorker::readParameter(const Parameter& p, int index,
                                 std::function<void(std::optional<std::int32_t>)> done) {
    post([this, &p, index, done](Device& d) {
        const auto v = d.readParameter(p, index);
        if (v) note(p.addressFor(index), *v);
        done(v);
    });
}

std::optional<std::int32_t> DeviceWorker::cached(Address a) const {
    std::lock_guard lock(mutex_);
    const auto it = cache_.find(a);
    if (it == cache_.end()) return std::nullopt;
    return it->second;
}

void DeviceWorker::setStateListener(std::function<void(Address, std::int32_t)> fn) {
    std::lock_guard lock(mutex_);
    listener_ = std::move(fn);
}

void DeviceWorker::note(Address a, std::int32_t v) {
    std::function<void(Address, std::int32_t)> fn;
    {
        std::lock_guard lock(mutex_);
        cache_[a] = v;
        fn = listener_;
    }
    if (fn) fn(a, v);
}

void DeviceWorker::flushWrites() {
    std::map<Address, std::pair<const Parameter*, std::int32_t>> batch;
    {
        std::lock_guard lock(mutex_);
        batch.swap(pending_);
    }
    if (!batch.empty()) changes_.fetch_add(1);
    for (auto& [addr, pv] : batch) {
        device_.writeParameter(*pv.first, pv.second,
                               // recover the index from the resolved address
                               pv.first->variable == AddressVariable::Part ||
                                       pv.first->variable == AddressVariable::Element
                                   ? addr.mid
                                   : 0);
        note(addr, pv.second);
    }
}

void DeviceWorker::run() {
    while (!quit_) {
        std::vector<std::function<void(Device&)>> jobs;
        {
            std::unique_lock lock(mutex_);
            if (jobs_.empty() && pending_.empty())
                cv_.wait_for(lock, flush_);
            jobs.swap(jobs_);
        }
        for (auto& j : jobs) {
            if (quit_) break;
            j(device_);
        }
        flushWrites();
    }
}

std::shared_ptr<DeviceWorker> sharedWorker() {
    static std::mutex mutex;
    static std::weak_ptr<DeviceWorker> weak;
    std::lock_guard lock(mutex);
    if (auto existing = weak.lock()) return existing;
    auto fresh = std::make_shared<DeviceWorker>();
    weak = fresh;
    return fresh;
}

}  // namespace motifxs
