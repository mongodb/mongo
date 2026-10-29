// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/lock_manager/lock_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/stats/counter_ops.h"
#include "mongo/platform/atomic.h"

#include <memory>

namespace mongo {

template <typename CounterType>
LockStats<CounterType>::LockStats() = default;

template <typename CounterType>
void LockStats<CounterType>::report(BSONObjBuilder* builder, bool reportZeroMetrics) const {
    for (uint8_t i = 0; i < static_cast<uint8_t>(ResourceGlobalId::kNumIds); ++i) {
        _report(builder,
                resourceGlobalIdName(static_cast<ResourceGlobalId>(i)),
                _resourceGlobalStats[i],
                reportZeroMetrics);
    }

    // Index starting from offset 2 because position 0 is a sentinel value for invalid resource/no
    // lock, and position 1 is the global resource which was already reported above.
    for (int i = 2; i < ResourceTypesCount; i++) {
        _report(
            builder, resourceTypeName(static_cast<ResourceType>(i)), _stats[i], reportZeroMetrics);
    }

    _report(builder, "oplog", _oplogStats, reportZeroMetrics);
}

template <typename CounterType>
void LockStats<CounterType>::_report(BSONObjBuilder* builder,
                                     const char* resourceTypeName,
                                     const PerModeLockStatCounters& stat,
                                     bool reportZeroMetrics) const {
    std::unique_ptr<BSONObjBuilder> section;

    // All indexing below starts from offset 1, because we do not want to report/account
    // position 0, which is a sentinel value for invalid resource/no lock.

    // Num acquires
    {
        std::unique_ptr<BSONObjBuilder> numAcquires;
        for (int mode = 1; mode < LockModesCount; mode++) {
            long long value = counter_ops::get(stat.modeStats[mode].numAcquisitions);
            if (value > 0 || reportZeroMetrics) {
                if (!numAcquires) {
                    if (!section) {
                        section.reset(new BSONObjBuilder(builder->subobjStart(resourceTypeName)));
                    }

                    numAcquires.reset(new BSONObjBuilder(section->subobjStart("acquireCount")));
                }
                numAcquires->append(legacyModeName(static_cast<LockMode>(mode)), value);
            }
        }
    }

    // Num waits
    {
        std::unique_ptr<BSONObjBuilder> numWaits;
        for (int mode = 1; mode < LockModesCount; mode++) {
            long long value = counter_ops::get(stat.modeStats[mode].numWaits);
            if (value > 0 || reportZeroMetrics) {
                if (!numWaits) {
                    if (!section) {
                        section.reset(new BSONObjBuilder(builder->subobjStart(resourceTypeName)));
                    }

                    numWaits.reset(new BSONObjBuilder(section->subobjStart("acquireWaitCount")));
                }
                numWaits->append(legacyModeName(static_cast<LockMode>(mode)), value);
            }
        }
    }

    // Total time waiting
    {
        std::unique_ptr<BSONObjBuilder> timeAcquiring;
        for (int mode = 1; mode < LockModesCount; mode++) {
            long long value = counter_ops::get(stat.modeStats[mode].combinedWaitTimeMicros);
            if (value > 0 || reportZeroMetrics) {
                if (!timeAcquiring) {
                    if (!section) {
                        section.reset(new BSONObjBuilder(builder->subobjStart(resourceTypeName)));
                    }

                    timeAcquiring.reset(
                        new BSONObjBuilder(section->subobjStart("timeAcquiringMicros")));
                }
                timeAcquiring->append(legacyModeName(static_cast<LockMode>(mode)), value);
            }
        }
    }
}

template <typename CounterType>
void LockStats<CounterType>::reset() {
    for (uint8_t i = 0; i < static_cast<uint8_t>(ResourceGlobalId::kNumIds); ++i) {
        for (uint8_t mode = 0; mode < LockModesCount; ++mode) {
            _resourceGlobalStats[i].modeStats[mode].reset();
        }
    }

    for (int i = 0; i < ResourceTypesCount; i++) {
        for (int mode = 0; mode < LockModesCount; mode++) {
            _stats[i].modeStats[mode].reset();
        }
    }

    for (int mode = 0; mode < LockModesCount; mode++) {
        _oplogStats.modeStats[mode].reset();
    }
}

template <typename CounterType>
int64_t LockStats<CounterType>::getCumulativeWaitTimeMicros() const {
    int64_t totalWaitTime = 0;
    for (uint8_t i = 0; i < static_cast<uint8_t>(ResourceGlobalId::kNumIds); ++i) {
        totalWaitTime += _getWaitTime(_resourceGlobalStats[i]);
    }

    // Index starting from offset 2 because position 0 is a sentinel value for invalid resource/no
    // lock, and position 1 is the global resource which was already accounted for above.
    for (int i = RESOURCE_GLOBAL + 1; i < ResourceTypesCount; i++) {
        totalWaitTime += _getWaitTime(_stats[i]);
    }

    totalWaitTime += _getWaitTime(_oplogStats);
    return totalWaitTime;
}

template <typename CounterType>
int64_t LockStats<CounterType>::_getWaitTime(const PerModeLockStatCounters& stat) const {
    int64_t timeAcquiringLocks = 0;
    for (int mode = 1; mode < LockModesCount; mode++) {
        timeAcquiringLocks += counter_ops::get(stat.modeStats[mode].combinedWaitTimeMicros);
    }
    return timeAcquiringLocks;
}

// Ensures that these are the only instances compiled for LockStats for Atomic<long long> and
// int64_t
template class LockStats<int64_t>;
template class LockStats<Atomic<long long>>;

}  // namespace mongo
