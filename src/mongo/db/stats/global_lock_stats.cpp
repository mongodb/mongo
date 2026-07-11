// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/global_lock_stats.h"

#include "mongo/db/shard_role/lock_manager/locker.h"

namespace mongo {

GlobalLockStatsSnapshot collectGlobalLockStatsSnapshot(Date_t startedAt) {
    const auto counts = Locker::getGlobalClientStateCounts();

    GlobalLockStatsSnapshot snap;
    snap.activeReaders = counts.activeReader;
    snap.activeWriters = counts.activeWriter;
    snap.queuedReaders = counts.queuedReader;
    snap.queuedWriters = counts.queuedWriter;

    const auto elapsed = Date_t::now() - startedAt;
    snap.totalTimeMicros = durationCount<Microseconds>(elapsed);
    return snap;
}

}  // namespace mongo
