// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/stats/counter_ops.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {

/**
 * Counts numAcquisitions, numWaits and combinedWaitTimeMicros values.
 *
 * Additionally supports appending or subtracting other LockStatCounters' values to or from its own;
 * and can reset its own values to 0.
 */
template <typename CounterType>
struct [[MONGO_MOD_PUBLIC]] LockStatCounters {
    template <typename OtherType>
    void set(const LockStatCounters<OtherType>& other) {
        counter_ops::set(numAcquisitions, other.numAcquisitions);
        counter_ops::set(numWaits, other.numWaits);
        counter_ops::set(combinedWaitTimeMicros, other.combinedWaitTimeMicros);
    }

    template <typename OtherType>
    void append(const LockStatCounters<OtherType>& other) {
        counter_ops::add(numAcquisitions, other.numAcquisitions);
        counter_ops::add(numWaits, other.numWaits);
        counter_ops::add(combinedWaitTimeMicros, other.combinedWaitTimeMicros);
    }

    template <typename OtherType>
    void subtract(const LockStatCounters<OtherType>& other) {
        counter_ops::add(numAcquisitions, -other.numAcquisitions);
        counter_ops::add(numWaits, -other.numWaits);
        counter_ops::add(combinedWaitTimeMicros, -other.combinedWaitTimeMicros);
    }

    void reset() {
        counter_ops::set(numAcquisitions, 0);
        counter_ops::set(numWaits, 0);
        counter_ops::set(combinedWaitTimeMicros, 0);
    }

    // The lock statistics we track.
    CounterType numAcquisitions{0};
    CounterType numWaits{0};
    CounterType combinedWaitTimeMicros{0};
};

/**
 * Templatized lock statistics management class, which can be specialized with atomic integers
 * for the global stats and with regular integers for the per-locker stats.
 *
 * CounterType allows the code to operate on both int64_t and Atomic<long long>
 */
template <typename CounterType>
class [[MONGO_MOD_PUBLIC]] LockStats {
public:
    // Declare the type for the lock counters bundle
    typedef LockStatCounters<CounterType> LockStatCountersType;

    LockStats();
    LockStats(const LockStats<CounterType>& other) = default;

    template <typename OtherType>
    explicit LockStats(const LockStats<OtherType>& other) {
        set(other);
    }

    LockStats<CounterType>& operator=(const LockStats<CounterType>& other) = default;

    template <typename OtherType>
    LockStats<CounterType>& operator=(const LockStats<OtherType>& other) {
        set(other);
        return *this;
    }

    void recordAcquisition(ResourceId resId, LockMode mode) {
        counter_ops::add(get(resId, mode).numAcquisitions, 1);
    }

    void recordWait(ResourceId resId, LockMode mode) {
        counter_ops::add(get(resId, mode).numWaits, 1);
    }

    void recordWaitTime(ResourceId resId, LockMode mode, int64_t waitMicros) {
        counter_ops::add(get(resId, mode).combinedWaitTimeMicros, waitMicros);
    }

    LockStatCountersType& get(ResourceId resId, LockMode mode) {
        if (resId.getType() == RESOURCE_GLOBAL) {
            return _resourceGlobalStats[resId.getHashId()].modeStats[mode];
        }

        static const ResourceId resourceIdRsOplog(RESOURCE_COLLECTION,
                                                  NamespaceString::kRsOplogNamespace);
        if (resId == resourceIdRsOplog) {
            return _oplogStats.modeStats[mode];
        }

        return _stats[resId.getType()].modeStats[mode];
    }

    template <typename OtherType>
    void set(const LockStats<OtherType>& other) {
        typedef LockStatCounters<OtherType> OtherLockStatCountersType;

        // Set global lock stats.
        for (uint8_t i = 0; i < static_cast<uint8_t>(ResourceGlobalId::kNumIds); ++i) {
            for (uint8_t mode = 0; mode < LockModesCount; ++mode) {
                _resourceGlobalStats[i].modeStats[mode].set(
                    other._resourceGlobalStats[i].modeStats[mode]);
            }
        }

        // Set all non-global, non-oplog lock stats.
        for (int i = 0; i < ResourceTypesCount; i++) {
            for (int mode = 0; mode < LockModesCount; mode++) {
                const OtherLockStatCountersType& otherStats = other._stats[i].modeStats[mode];
                LockStatCountersType& thisStats = _stats[i].modeStats[mode];
                thisStats.set(otherStats);
            }
        }

        // Set the oplog stats
        for (int mode = 0; mode < LockModesCount; mode++) {
            const OtherLockStatCountersType& otherStats = other._oplogStats.modeStats[mode];
            LockStatCountersType& thisStats = _oplogStats.modeStats[mode];
            thisStats.set(otherStats);
        }
    }

    template <typename OtherType>
    void append(const LockStats<OtherType>& other) {
        typedef LockStatCounters<OtherType> OtherLockStatCountersType;

        // Append global lock stats.
        for (uint8_t i = 0; i < static_cast<uint8_t>(ResourceGlobalId::kNumIds); ++i) {
            for (uint8_t mode = 0; mode < LockModesCount; ++mode) {
                _resourceGlobalStats[i].modeStats[mode].append(
                    other._resourceGlobalStats[i].modeStats[mode]);
            }
        }

        // Append all non-global, non-oplog lock stats.
        for (int i = 0; i < ResourceTypesCount; i++) {
            for (int mode = 0; mode < LockModesCount; mode++) {
                const OtherLockStatCountersType& otherStats = other._stats[i].modeStats[mode];
                LockStatCountersType& thisStats = _stats[i].modeStats[mode];
                thisStats.append(otherStats);
            }
        }

        // Append the oplog stats
        for (int mode = 0; mode < LockModesCount; mode++) {
            const OtherLockStatCountersType& otherStats = other._oplogStats.modeStats[mode];
            LockStatCountersType& thisStats = _oplogStats.modeStats[mode];
            thisStats.append(otherStats);
        }
    }

    template <typename OtherType>
    void subtract(const LockStats<OtherType>& other) {
        typedef LockStatCounters<OtherType> OtherLockStatCountersType;

        for (uint8_t i = 0; i < static_cast<uint8_t>(ResourceGlobalId::kNumIds); ++i) {
            for (uint8_t mode = 0; mode < LockModesCount; ++mode) {
                _resourceGlobalStats[i].modeStats[mode].subtract(
                    other._resourceGlobalStats[i].modeStats[mode]);
            }
        }

        for (int i = 0; i < ResourceTypesCount; i++) {
            for (int mode = 0; mode < LockModesCount; mode++) {
                const OtherLockStatCountersType& otherStats = other._stats[i].modeStats[mode];
                LockStatCountersType& thisStats = _stats[i].modeStats[mode];
                thisStats.subtract(otherStats);
            }
        }

        for (int mode = 0; mode < LockModesCount; mode++) {
            const OtherLockStatCountersType& otherStats = other._oplogStats.modeStats[mode];
            LockStatCountersType& thisStats = _oplogStats.modeStats[mode];
            thisStats.subtract(otherStats);
        }
    }

    void report(BSONObjBuilder* builder, bool reportZeroMetrics = false) const;
    void reset();
    int64_t getCumulativeWaitTimeMicros() const;

private:
    // Necessary for the append call, which accepts argument of type different than our
    // template parameter.
    template <typename T>
    friend class LockStats;

    // Keep the per-mode lock stats next to each other in case we want to do fancy operations
    // such as atomic operations on 128-bit values.
    struct PerModeLockStatCounters {
        LockStatCountersType modeStats[LockModesCount];
    };

    void _report(BSONObjBuilder* builder,
                 const char* resourceTypeName,
                 const PerModeLockStatCounters& stat,
                 bool reportZeroMetrics = false) const;

    int64_t _getWaitTime(const PerModeLockStatCounters& stat) const;

    // For the global resource, split the lock stats per ID since each one should be reported
    // separately. For the remaining resources, split the lock stats per resource type. Special-case
    // the oplog so we can collect more detailed stats for it.
    PerModeLockStatCounters _resourceGlobalStats[static_cast<uint8_t>(ResourceGlobalId::kNumIds)];
    PerModeLockStatCounters _stats[ResourceTypesCount];
    PerModeLockStatCounters _oplogStats;
};

typedef LockStats<int64_t> SingleThreadedLockStats [[MONGO_MOD_PUBLIC]];

/**
 * Reports instance-wide locking statistics, which can then be converted to BSON or logged.
 */
[[MONGO_MOD_PUBLIC]] void reportGlobalLockingStats(SingleThreadedLockStats* outStats);

/**
 * Zeroes out all the global locking statistics, for testing purposes.
 */
[[MONGO_MOD_PUBLIC]] void resetGlobalLockStats_forTest();

}  // namespace mongo
