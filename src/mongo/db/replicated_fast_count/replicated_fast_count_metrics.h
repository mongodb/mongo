/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

namespace MONGO_MOD_PUBLIC mongo {

class ServiceContext;

/**
 * Wrapper for the ReplicatedFastCountManager OpenTelemetry metric instruments. All metrics are
 * reported directly via OTel and serverStatus.
 */
class ReplicatedFastCountMetrics {
public:
    void setIsRunning(bool running);

    /**
     * Records metrics for a successful flush. Updates flush timing and flushed-document
     * counters and gauges.
     */
    void recordFlush(Date_t startTime, size_t batchSize);

    void incrementFlushFailureCount();

    void incrementInsertCount();

    void incrementUpdateCount();

    void addWriteTimeMsTotal(int64_t ms);
};

/**
 * Free functions for recording checkpoint oplog scan metrics. These functions are safe to call from
 * any code in the checkpoint scan path without needing a ReplicatedFastCountMetrics instance.
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

}  // namespace MONGO_MOD_PUBLIC mongo
