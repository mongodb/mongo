/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/mutex.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>

#include <boost/optional.hpp>

namespace mongo {

namespace observable_mutex_details {

/**
 * AcquisitionStats are the building blocks to keeping track of different sorts of acquisition
 * statistics, mainly exclusive and shared (as seen below).
 */
template <typename CounterType>
struct AcquisitionStats {
    CounterType total{0};        // Tracks the total number of acquisitions.
    CounterType contentions{0};  // Tracks the number of acquisitions that had to wait.
    CounterType waitCycles{0};   // Tracks the total wait time for contended acquisitions.
};

struct LockStats {
    AcquisitionStats<uint64_t> exclusiveAcquisitions{0, 0, 0};
    AcquisitionStats<uint64_t> sharedAcquisitions{0, 0, 0};
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
    using AppendStatsCallback = std::function<void(LockStats&)>;

    explicit ObservationToken(AppendStatsCallback callback) : _callback(std::move(callback)) {}

    void invalidate() {
        stdx::lock_guard lk(_mutex);
        _isValid = false;
    }

    bool isValid() const {
        stdx::lock_guard lk(_mutex);
        return _isValid;
    }

    explicit operator bool() const {
        return isValid();
    }

    boost::optional<LockStats> getStats() const {
        stdx::lock_guard lk(_mutex);
        if (!_isValid) {
            return boost::none;
        }
        LockStats stats;
        _callback(stats);
        return stats;
    }

private:
    AppendStatsCallback _callback;
    mutable stdx::mutex _mutex;
    bool _isValid = true;
};

template <typename MutexType>
concept ImplementsLockShared = requires(MutexType m) { m.lock_shared(); };

}  // namespace observable_mutex_details

/**
 * The wrapper for mutex objects, ObservableMutex, collects contention metrics for the wrapped mutex
 * type, including total acquisitions, number of contentions, and total wait time for contended
 * acquisitions, for shared and exclusive acquisitions. Lock functions take the fast path for
 * wait-free mutex acquisition and the slow path for waiting on the mutex. The wrapper exposes its
 * collected metrics through a token, which is initialized and created with the wrapper and
 * invalidated when the wrapper's destructor runs. The token is provided with a callback function
 * that collects contention stats for the wrapped mutex and returns it to the collector.
 */
#ifdef MONGO_CONFIG_MUTEX_OBSERVATION
template <typename MutexType>
class ObservableMutex {
public:
    ObservableMutex()
        : _token(std::make_shared<ObservationToken>(
              [this](observable_mutex_details::LockStats& stats) {
                  auto append = [](auto& agg, const auto& sample) {
                      agg.total += sample.total.loadRelaxed();
                      agg.contentions += sample.contentions.loadRelaxed();
                      agg.waitCycles += sample.waitCycles.loadRelaxed();
                  };
                  append(stats.exclusiveAcquisitions, _exclusiveAcquisitions);
                  append(stats.sharedAcquisitions, _sharedAcquisitions);
              })) {}

    ~ObservableMutex() {
        _token->invalidate();
    }

    const auto& token() const {
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

private:
    using ObservationToken = observable_mutex_details::ObservationToken;
    using AtomicAcquisitionStats = observable_mutex_details::AcquisitionStats<Atomic<uint64_t>>;

    // TODO SERVER-106769: Replace the following with the new low-overhead timer.
    MONGO_COMPILER_ALWAYS_INLINE uint64_t _getCurrentCPUCycle() const {
#if defined(__x86_64__)
        unsigned int lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        __asm__ __volatile__("lfence" ::: "memory");
        return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
        asm volatile("isb" : : : "memory");
        uint64_t tsc;
        asm volatile("mrs %0, cntvct_el0" : "=r"(tsc));
        return tsc;
#else
        return 0;
#endif
    }

    MONGO_COMPILER_NOINLINE void _onContendedAcquisition(AtomicAcquisitionStats& stats,
                                                         const std::function<void()>& lock) {
        stats.contentions.fetchAndAddRelaxed(1);
#ifndef _WIN32  // TODO SERVER-107856: Enable the waitCycles metric for Windows
        const auto t1 = _getCurrentCPUCycle();
#endif
        try {
            lock();
        } catch (...) {
            stats.contentions.fetchAndSubtractRelaxed(1);
            throw;
        }
#ifndef _WIN32
        stats.waitCycles.fetchAndAddRelaxed(_getCurrentCPUCycle() - t1);
#endif
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

using ObservableExclusiveMutex = ObservableMutex<stdx::mutex>;
using ObservableSharedMutex = ObservableMutex<std::shared_mutex>;  // NOLINT

}  // namespace mongo
