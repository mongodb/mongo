// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>

namespace mongo {

class ServiceContext;

struct [[MONGO_MOD_PUBLIC]] GlobalLockStatsSnapshot {
    int64_t totalTimeMicros{0};
    int64_t activeReaders{0};
    int64_t activeWriters{0};
    int64_t queuedReaders{0};
    int64_t queuedWriters{0};
};

/**
 * Reads global atomic lock-state counters and returns a snapshot of active/queued reader and
 * writer counts. `startedAt` is subtracted from `Date_t::now()` to produce `totalTimeMicros`;
 * each caller owns its own reference point (e.g. captured at static initialization or at
 * install time).
 */
[[MONGO_MOD_PUBLIC]] GlobalLockStatsSnapshot collectGlobalLockStatsSnapshot(Date_t startedAt);

}  // namespace mongo
