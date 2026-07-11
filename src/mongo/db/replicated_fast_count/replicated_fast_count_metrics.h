// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ServiceContext;

/**
 * Free functions wrapping the ReplicatedFastCountManager OpenTelemetry metric instruments. All
 * metrics are reported via OTel and serverStatus.
 */
void setIsRunning(bool running);

void incrementFlushFailureCount();

void incrementInsertCount();

void incrementUpdateCount();

/**
 * Records metrics for a successful flush. Updates flush timing, success count, and
 * flushed-document counters.
 */
void recordFlush(Date_t startTime, size_t batchSize);

/**
 * Free functions for recording checkpoint oplog scan metrics. These functions are safe to call from
 * any code in the checkpoint scan path.
 */
void recordCheckpointOplogEntryProcessed();
void recordCheckpointOplogEntrySkipped();
void recordCheckpointSizeCountEntryProcessed(int count = 1);

/**
 * Records the timestamp of the most recently applied oplog entry observed from the replication
 * coordinator and refreshes the `oplog_lag_secs` gauge. Intended to be called from an
 * OpTimeObserver registered with ReplicationCoordinator::addAppliedOpTimeObserver.
 */
void recordAppliedOpTime(const Timestamp& ts);

/**
 * Records the timestamp of the most recently persisted fastcount checkpoint and refreshes the
 * `oplog_lag_secs` gauge. Called from `ReplicatedFastCountManager::initializeMetadata()` to seed
 * from disk at startup, and from `ReplicatedFastCountOpObserver`'s on-commit hook on every write
 * to `config.fast_count_metadata_store_timestamps` (primary path and secondary oplog-apply path).
 */
void recordCheckpointAdvanced(const Timestamp& ts);

/**
 * Registers an OpTimeObserver with the replication coordinator that feeds the `oplog_lag_secs`
 * gauge. Should be called once per ServiceContext; calling more than once just registers extra
 * (idempotent) observer instances.
 */
void registerAppliedOpTimeObserver(ServiceContext* svcCtx);

/**
 * Clears the cached applied/checkpoint timestamps that back the `oplog_lag_secs` gauge. Tests that
 * need to exercise the pre-first-checkpoint state must call this — the underlying atomics live in
 * a file-scope namespace and persist across tests.
 */
void resetOplogLagState_ForTest();

}  // namespace mongo
