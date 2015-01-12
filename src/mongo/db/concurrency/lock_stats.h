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

    class LockStats {
    public:

        /**
         * Locking statistics for the top level locks.
         */
        struct AtomicLockStats {
            AtomicLockStats() {
                reset();
            }

            void append(const AtomicLockStats& other);
            void reset();

            AtomicInt64 numAcquisitions;
            AtomicInt64 numWaits;
            AtomicInt64 combinedWaitTimeMicros;
            AtomicInt64 numDeadlocks;
        };

        // Keep the per-mode lock stats next to each other in case we want to do fancy operations
        // such as atomic operations on 128-bit values.
        struct PerModeAtomicLockStats {
            AtomicLockStats stats[LockModesCount];
        };


        LockStats();

        void recordAcquisition(ResourceId resId, LockMode mode);
        void recordWait(ResourceId resId, LockMode mode);
        void recordWaitTime(ResourceId resId, LockMode mode, uint64_t waitMicros);
        void recordDeadlock(ResourceId resId, LockMode mode);

        PerModeAtomicLockStats& get(ResourceId resId);

        void append(const LockStats& other);
        void report(BSONObjBuilder* builder) const;
        void reset();

    private:

        void _report(BSONObjBuilder* builder, const PerModeAtomicLockStats& stat) const;

        // Split the lock stats per resource type and special-case the oplog so we can collect
        // more detailed stats for it.
        PerModeAtomicLockStats _stats[ResourceTypesCount];
        PerModeAtomicLockStats _oplogStats;
    };


    /**
     * Reports instance-wide locking statistics, which can then be converted to BSON or logged.
     */
    void reportGlobalLockingStats(LockStats* outStats);

    /**
     * Currently used for testing only.
     */
    void resetGlobalLockStats();

} // namespace mongo
