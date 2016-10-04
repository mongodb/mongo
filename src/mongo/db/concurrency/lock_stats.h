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

#pragma once

#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

class BSONObjBuilder;


/**
 * Operations for manipulating the lock statistics abstracting whether they are atomic or not.
 */
struct CounterOps {
    static int64_t get(const int64_t& counter) {
        return counter;
    }

    static int64_t get(const AtomicInt64& counter) {
        return counter.load();
    }

    static void set(int64_t& counter, int64_t value) {
        counter = value;
    }

    static void set(AtomicInt64& counter, int64_t value) {
        counter.store(value);
    }

    static void add(int64_t& counter, int64_t value) {
        counter += value;
    }

    static void add(int64_t& counter, const AtomicInt64& value) {
        counter += value.load();
    }

    static void add(AtomicInt64& counter, int64_t value) {
        counter.addAndFetch(value);
    }
};


/**
 * Bundle of locking statistics values.
 */
template <typename CounterType>
struct LockStatCounters {
    template <typename OtherType>
    void append(const LockStatCounters<OtherType>& other) {
        CounterOps::add(numAcquisitions, other.numAcquisitions);
        CounterOps::add(numWaits, other.numWaits);
        CounterOps::add(combinedWaitTimeMicros, other.combinedWaitTimeMicros);
        CounterOps::add(numDeadlocks, other.numDeadlocks);
    }

    void reset() {
        CounterOps::set(numAcquisitions, 0);
        CounterOps::set(numWaits, 0);
        CounterOps::set(combinedWaitTimeMicros, 0);
        CounterOps::set(numDeadlocks, 0);
    }


    CounterType numAcquisitions;
    CounterType numWaits;
    CounterType combinedWaitTimeMicros;
    CounterType numDeadlocks;
};


/**
 * Templatized lock statistics management class, which can be specialized with atomic integers
 * for the global stats and with regular integers for the per-locker stats.
 */
template <typename CounterType>
class LockStats {
public:
    // Declare the type for the lock counters bundle
    typedef LockStatCounters<CounterType> LockStatCountersType;

    /**
     * Initializes the locking statistics with zeroes (calls reset).
     */
    LockStats();

    void recordAcquisition(ResourceId resId, LockMode mode) {
        CounterOps::add(get(resId, mode).numAcquisitions, 1);
    }

    void recordWait(ResourceId resId, LockMode mode) {
        CounterOps::add(get(resId, mode).numWaits, 1);
    }

    void recordWaitTime(ResourceId resId, LockMode mode, int64_t waitMicros) {
        CounterOps::add(get(resId, mode).combinedWaitTimeMicros, waitMicros);
    }

    void recordDeadlock(ResourceId resId, LockMode mode) {
        CounterOps::add(get(resId, mode).numDeadlocks, 1);
    }

    LockStatCountersType& get(ResourceId resId, LockMode mode) {
        if (resId == resourceIdOplog) {
            return _oplogStats.modeStats[mode];
        }

        return _stats[resId.getType()].modeStats[mode];
    }

    template <typename OtherType>
    void append(const LockStats<OtherType>& other) {
        typedef LockStatCounters<OtherType> OtherLockStatCountersType;

        // Append all lock stats
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

    void report(BSONObjBuilder* builder) const;
    void reset();

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
                 const char* sectionName,
                 const PerModeLockStatCounters& stat) const;


    // Split the lock stats per resource type and special-case the oplog so we can collect
    // more detailed stats for it.
    PerModeLockStatCounters _stats[ResourceTypesCount];
    PerModeLockStatCounters _oplogStats;
};

typedef LockStats<int64_t> SingleThreadedLockStats;
typedef LockStats<AtomicInt64> AtomicLockStats;


/**
 * Reports instance-wide locking statistics, which can then be converted to BSON or logged.
 */
void reportGlobalLockingStats(SingleThreadedLockStats* outStats);

/**
 * Currently used for testing only.
 */
void resetGlobalLockStats();

}  // namespace mongo
