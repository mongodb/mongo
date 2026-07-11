// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {
namespace initial_sync_common_stats {

// The number of initial sync attempts that have failed since server startup. Each instance of
// InitialSyncer may run multiple attempts to fulfill an initial sync request that is triggered
// when InitialSyncer::startup() is called.
extern otel::metrics::Counter<int64_t>& initialSyncFailedAttempts;

// The number of initial sync requests that have been requested and failed. Each instance of
// InitialSyncer (upon successful startup()) corresponds to a single initial sync request.
// This value does not include the number of times where a InitialSyncer is created successfully
// but failed in startup().
extern otel::metrics::Counter<int64_t>& initialSyncFailures;

// The number of initial sync requests that have been requested and completed successfully. Each
// instance of InitialSyncer corresponds to a single initial sync request.
extern otel::metrics::Counter<int64_t>& initialSyncCompletes;

void LogInitialSyncAttemptStats(const StatusWith<OpTimeAndWallTime>& attemptResult,
                                bool hasRetries,
                                const BSONObj& stats);

}  // namespace initial_sync_common_stats
}  // namespace repl
}  // namespace mongo
