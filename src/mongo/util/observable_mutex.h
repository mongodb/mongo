// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"
#include "mongo/util/scopeguard.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <absl/base/internal/cycleclock.h>
#include <boost/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

namespace observable_mutex_details {
template <typename MutexType>
concept ImplementsLockShared = requires(MutexType m) { m.lock_shared(); };

/**
 * AcquisitionStats are the building blocks to keeping track of different sorts of acquisition
 * statistics, mainly exclusive and shared (as seen below).
 */
template <typename CounterType>
struct AcquisitionStats {
    CounterType total{0};        // Tracks the total number of acquisitions.
    CounterType contentions{0};  // Tracks the number of acquisitions that had to wait.
    CounterType waitCycles{0};   // Tracks the total wait time for contended acquisitions.

    AcquisitionStats& operator+=(const AcquisitionStats rhs) {
        this->total += rhs.total;
        this->contentions += rhs.contentions;
        this->waitCycles += rhs.waitCycles;
        return *this;
    }

    AcquisitionStats operator+(const AcquisitionStats rhs) const {
        return {this->total + rhs.total,
                this->contentions + rhs.contentions,
                this->waitCycles + rhs.waitCycles};
    }
};

struct Timer {
    // TODO [SERVER-106769]: Replace `Timer` with the new low-overhead timer.
    MONGO_COMPILER_ALWAYS_INLINE int64_t getTime() const {
        // TODO [SERVER-90115]: This absl functionality is internal-only and may be removed,
        // replace this once we've created our own cycle clock reader.
        return absl::base_internal::CycleClock::Now();
    }
};
}  // namespace observable_mutex_details

using MutexAcquisitionStats = observable_mutex_details::AcquisitionStats<uint64_t>;

/**
 * Converts a raw wait-cycle count into microseconds using the CycleClock frequency. The
 * frequency is architecture-specific but known at runtime, so dividing by it yields comparable
 * time units across a mixed fleet.
 */
inline int64_t waitCyclesToMicros(uint64_t waitCycles) {
    const double freq = absl::base_internal::CycleClock::Frequency();
    if (MONGO_unlikely(freq <= 0.0)) {
        return 0;
    }
    return static_cast<int64_t>(static_cast<double>(waitCycles) * 1e6 / freq);
}

struct MutexStats {
    MutexAcquisitionStats exclusiveAcquisitions{0, 0, 0};
    MutexAcquisitionStats sharedAcquisitions{0, 0, 0};

    MutexStats& operator+=(const MutexStats rhs) {
        this->exclusiveAcquisitions += rhs.exclusiveAcquisitions;
        this->sharedAcquisitions += rhs.sharedAcquisitions;
        return *this;
    }

    MutexStats operator+(const MutexStats rhs) const {
        return {this->exclusiveAcquisitions + rhs.exclusiveAcquisitions,
                this->sharedAcquisitions + rhs.sharedAcquisitions};
    }
};

/**
 * Tokens are shared between the wrapper ObservableMutex and the ObservableMutexRegistry. A token is
 * used to enable tracking instances of ObservableMutex without changing their lifetime semantics.
 * The validity of the token tracks the liveness of its corresponding mutex object. The token also
 * enables collectors to safely acquire the latest contention statistics for each ObservableMutex
 * through the registry.
 */
class ObservationToken {
public:
    using AppendStatsCallback = std::function<void(MutexStats&)>;

    explicit ObservationToken(AppendStatsCallback callback) : _callback(std::move(callback)) {}

    /**
     * Updates _lastKnownStats using _callback and marks the token as invalid.
     */
    void invalidate() {
        std::lock_guard lk(_mutex);
        if (!_isValid) {
            return;
        }
        _callback(_lastKnownStats);
        _isValid = false;
    }

    bool isValid() const {
        std::lock_guard lk(_mutex);
        return _isValid;
    }

    explicit operator bool() const {
        return isValid();
    }

    MutexStats getStats() {
        std::lock_guard lk(_mutex);
        if (_isValid) {
            _callback(_lastKnownStats);
        }
        return _lastKnownStats;
    }

private:
    AppendStatsCallback _callback;
    mutable std::mutex _mutex;
    MutexStats _lastKnownStats;
    bool _isValid = true;
};

// TODO(SERVER-110898): Remove once TSAN works with ObservableMutex.
#if !__has_feature(thread_sanitizer)
/**
 * The wrapper for mutex objects, ObservableMutex, collects contention metrics for the wrapped mutex
 * type, including total acquisitions, number of contentions, and total wait time for contended
 * acquisitions, for shared and exclusive acquisitions. Lock functions take the fast path for
 * wait-free mutex acquisition and the slow path for waiting on the mutex. The wrapper exposes its
 * collected metrics through a token, which is initialized and created with the wrapper and
 * invalidated when the wrapper's destructor runs. The token is provided with a callback function
 * that collects contention stats for the wrapped mutex and returns it to the collector.
 */
template <typename MutexType>
class ObservableMutex {
public:
    ObservableMutex()
        : _token(std::make_shared<ObservationToken>([this](MutexStats& stats) {
              auto assign = [](auto& target, const auto& source) {
                  target.total = source.total.loadRelaxed();
                  target.contentions = source.contentions.loadRelaxed();
                  target.waitCycles = source.waitCycles.loadRelaxed();
              };
              assign(stats.exclusiveAcquisitions, _exclusiveAcquisitions);
              assign(stats.sharedAcquisitions, _sharedAcquisitions);
          })) {}

    ~ObservableMutex() {
        _token->invalidate();
    }

    const std::shared_ptr<ObservationToken>& token() const {
        return _token;
    }

    void lock() {
        if (MONGO_unlikely(!_mutex.try_lock())) {
            _onContendedAcquisition(_exclusiveAcquisitions, [&]() { _mutex.lock(); });
        }
        _exclusiveAcquisitions.total.fetchAndAddRelaxed(1);
    }

    void unlock() {
        _mutex.unlock();
    }

    void lock_shared()
    requires observable_mutex_details::ImplementsLockShared<MutexType>
    {
        if (MONGO_unlikely(!_mutex.try_lock_shared())) {
            _onContendedAcquisition(_sharedAcquisitions, [&]() { _mutex.lock_shared(); });
        }
        _sharedAcquisitions.total.fetchAndAddRelaxed(1);
    }

    void unlock_shared()
    requires observable_mutex_details::ImplementsLockShared<MutexType>
    {
        _mutex.unlock_shared();
    }

    [[MONGO_MOD_NEEDS_REPLACEMENT]] void setExclusiveAcquisitions_forTest(
        MutexAcquisitionStats stat) {
        _exclusiveAcquisitions.contentions.store(stat.contentions);
        _exclusiveAcquisitions.total.store(stat.total);
        _exclusiveAcquisitions.waitCycles.store(stat.waitCycles);
    }

    [[MONGO_MOD_NEEDS_REPLACEMENT]] void setSharedAcquisitions_forTest(MutexAcquisitionStats stat) {
        _sharedAcquisitions.contentions.store(stat.contentions);
        _sharedAcquisitions.total.store(stat.total);
        _sharedAcquisitions.waitCycles.store(stat.waitCycles);
    }

private:
    using AtomicAcquisitionStats = observable_mutex_details::AcquisitionStats<Atomic<uint64_t>>;

    template <typename LockerFunc>
    MONGO_COMPILER_NOINLINE void _onContendedAcquisition(AtomicAcquisitionStats& stats,
                                                         LockerFunc locker) {
        // TODO SERVER-106769: Replace `Timer` with the new low-overhead timer.
        observable_mutex_details::Timer timer;
        stats.contentions.fetchAndAddRelaxed(1);
        const auto t1 = timer.getTime();

        // This callback is only run when the call to locker() throws. This is the preferred way to
        // handle exceptions since it can help produce better code when locker() can't throw.
        ScopeGuard cleanupGuard([&]() { stats.contentions.fetchAndSubtractRelaxed(1); });
        locker();
        cleanupGuard.dismiss();

        const auto t2 = timer.getTime();
        // Safety check: drop the sample if the timer ran backwards to avoid underflowing
        // waitCycles.
        if (MONGO_likely(t2 >= t1)) {
            stats.waitCycles.fetchAndAddRelaxed(t2 - t1);
        }
    }

    mutable MutexType _mutex;

    AtomicAcquisitionStats _exclusiveAcquisitions;

    // The following tracks shared lock acquisitions, if supported by `MutexType`.
    AtomicAcquisitionStats _sharedAcquisitions;

    std::shared_ptr<ObservationToken> _token;
};
#else
template <typename MutexType>
using ObservableMutex = MutexType;
#endif

using ObservableExclusiveMutex = ObservableMutex<std::mutex>;
using ObservableSharedMutex = ObservableMutex<std::shared_mutex>;  // NOLINT

}  // namespace mongo
