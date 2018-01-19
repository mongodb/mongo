/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/platform/atomic_word.h"

namespace mongo {

class BSONObjBuilder;
class OperationContext;
class ServiceContext;

/**
 * Encapsulates per-process statistics for the sharding subsystem.
 */
struct ShardingStatistics {
    // Counts how many times threads hit stale config exception (which is what triggers metadata
    // refreshes)
    AtomicInt64 countStaleConfigErrors{0};

    // Cumulative, always-increasing counter of how many chunks did this node start donating
    // (whether they succeeded or not)
    AtomicInt64 countDonorMoveChunkStarted{0};

    // Cumulative, always-increasing counter of how much time the entire move chunk operation took
    // (excluding range deletion)
    AtomicInt64 totalDonorMoveChunkTimeMillis{0};

    // Cumulative, always-increasing counter of how much time the clone phase took on the donor
    // node, before it was appropriate to enter the critical section
    AtomicInt64 totalDonorChunkCloneTimeMillis{0};

    // Cumulative, always-increasing counter of how much time the critical section's commit phase
    // took (this is the period of time when all operations on the collection are blocked, not just
    // the reads)
    AtomicInt64 totalCriticalSectionCommitTimeMillis{0};

    // Cumulative, always-increasing counter of how much time the entire critical section took. It
    // includes the time the recipient took to fetch the latest modifications from the donor and
    // persist them plus the critical section commit time.
    //
    // The value of totalCriticalSectionTimeMillis - totalCriticalSectionCommitTimeMillis gives the
    // duration of the catch-up phase of the critical section (where the last mods are transferred
    // from the donor to the recipient).
    AtomicInt64 totalCriticalSectionTimeMillis{0};

    /**
     * Obtains the per-process instance of the sharding statistics object.
     */
    static ShardingStatistics& get(ServiceContext* serviceContext);
    static ShardingStatistics& get(OperationContext* opCtx);

    /**
     * Reports the accumulated statistics for serverStatus.
     */
    void report(BSONObjBuilder* builder) const;
};

}  // namespace mongo
