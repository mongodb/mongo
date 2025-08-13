/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/lock_manager/lock_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/stats/counter_ops.h"

#include <memory>

namespace mongo {

template <typename CounterType>
void LockStats<CounterType>::report(BSONObjBuilder* builder) const {
    for (uint8_t i = 0; i < static_cast<uint8_t>(ResourceGlobalId::kNumIds); ++i) {
        _report(builder,
                resourceGlobalIdName(static_cast<ResourceGlobalId>(i)),
                _resourceGlobalStats[i]);
    }

    // Index starting from offset 2 because position 0 is a sentinel value for invalid resource/no
    // lock, and position 1 is the global resource which was already reported above.
    for (int i = 2; i < ResourceTypesCount; i++) {
        _report(builder, resourceTypeName(static_cast<ResourceType>(i)), _stats[i]);
    }

    _report(builder, "oplog", _oplogStats);
}

template <typename CounterType>
void LockStats<CounterType>::_report(BSONObjBuilder* builder,
                                     const char* resourceTypeName,
                                     const PerModeLockStatCounters& stat) const {
    std::unique_ptr<BSONObjBuilder> section;

    // All indexing below starts from offset 1, because we do not want to report/account
    // position 0, which is a sentinel value for invalid resource/no lock.

    // Num acquires
    {
        std::unique_ptr<BSONObjBuilder> numAcquires;
        for (int mode = 1; mode < LockModesCount; mode++) {
            long long value = counter_ops::get(stat.modeStats[mode].numAcquisitions);

            if (value > 0) {
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
            if (value > 0) {
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
            if (value > 0) {
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

// Ensures that there are instances compiled for LockStats for AtomicWord<long long> and int64_t
template class LockStats<int64_t>;
template class LockStats<AtomicWord<long long>>;

}  // namespace mongo
