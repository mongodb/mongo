/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/lock_stats.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

template <typename CounterType>
LockStats<CounterType>::LockStats() {
    reset();
}

template <typename CounterType>
void LockStats<CounterType>::report(BSONObjBuilder* builder) const {
    // All indexing below starts from offset 1, because we do not want to report/account
    // position 0, which is a sentinel value for invalid resource/no lock.
    for (int i = 1; i < ResourceTypesCount; i++) {
        _report(builder, resourceTypeName(static_cast<ResourceType>(i)), _stats[i]);
    }

    _report(builder, "oplog", _oplogStats);
}

template <typename CounterType>
void LockStats<CounterType>::_report(BSONObjBuilder* builder,
                                     const char* sectionName,
                                     const PerModeLockStatCounters& stat) const {
    std::unique_ptr<BSONObjBuilder> section;

    // All indexing below starts from offset 1, because we do not want to report/account
    // position 0, which is a sentinel value for invalid resource/no lock.

    // Num acquires
    {
        std::unique_ptr<BSONObjBuilder> numAcquires;
        for (int mode = 1; mode < LockModesCount; mode++) {
            const long long value = CounterOps::get(stat.modeStats[mode].numAcquisitions);
            if (value > 0) {
                if (!numAcquires) {
                    if (!section) {
                        section.reset(new BSONObjBuilder(builder->subobjStart(sectionName)));
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
            const long long value = CounterOps::get(stat.modeStats[mode].numWaits);
            if (value > 0) {
                if (!numWaits) {
                    if (!section) {
                        section.reset(new BSONObjBuilder(builder->subobjStart(sectionName)));
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
            const long long value = CounterOps::get(stat.modeStats[mode].combinedWaitTimeMicros);
            if (value > 0) {
                if (!timeAcquiring) {
                    if (!section) {
                        section.reset(new BSONObjBuilder(builder->subobjStart(sectionName)));
                    }

                    timeAcquiring.reset(
                        new BSONObjBuilder(section->subobjStart("timeAcquiringMicros")));
                }
                timeAcquiring->append(legacyModeName(static_cast<LockMode>(mode)), value);
            }
        }
    }

    // Deadlocks
    {
        std::unique_ptr<BSONObjBuilder> deadlockCount;
        for (int mode = 1; mode < LockModesCount; mode++) {
            const long long value = CounterOps::get(stat.modeStats[mode].numDeadlocks);
            if (value > 0) {
                if (!deadlockCount) {
                    if (!section) {
                        section.reset(new BSONObjBuilder(builder->subobjStart(sectionName)));
                    }

                    deadlockCount.reset(new BSONObjBuilder(section->subobjStart("deadlockCount")));
                }
                deadlockCount->append(legacyModeName(static_cast<LockMode>(mode)), value);
            }
        }
    }
}

template <typename CounterType>
void LockStats<CounterType>::reset() {
    for (int i = 0; i < ResourceTypesCount; i++) {
        for (int mode = 0; mode < LockModesCount; mode++) {
            _stats[i].modeStats[mode].reset();
        }
    }

    for (int mode = 0; mode < LockModesCount; mode++) {
        _oplogStats.modeStats[mode].reset();
    }
}


// Ensures that there are instances compiled for LockStats for AtomicInt64 and int64_t
template class LockStats<int64_t>;
template class LockStats<AtomicInt64>;

}  // namespace mongo
