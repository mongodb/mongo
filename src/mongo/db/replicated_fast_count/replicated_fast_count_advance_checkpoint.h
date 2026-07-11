// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_streaming_oplog_delta_accumulator.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"

namespace mongo::replicated_fast_count {
/**
 * Materialized checkpoint snapshot ready to persist.
 *
 * `updatedCollections` contains absolute size/count totals, not incremental deltas.
 */
struct SizeCountCheckpointSnapshot {
    SizeCountDeltas updatedCollections;
    Timestamp validAsOf;

    bool empty() const {
        return updatedCollections.empty();
    }
};

/**
 * Converts incremental deltas plus a known visible cutoff into a persistable snapshot by reading
 * the currently persisted totals and incrementing them in-place.
 */
SizeCountCheckpointSnapshot materializeCheckpointSnapshot(OperationContext* opCtx,
                                                          const SizeCountStore& sizeCountStore,
                                                          OplogScanResult scanResult,
                                                          Timestamp scanStartAfterTS);

/**
 * Persists a materialized checkpoint snapshot to the `sizeCountStore` and updates the valid-as-of
 * timestamp in `timestampStore`.
 *
 * Returns the number of writes to the `sizeCountStore`.
 */
size_t persistCheckpointSnapshot(OperationContext* opCtx,
                                 const SizeCountCheckpointSnapshot& checkpoint,
                                 SizeCountStore& sizeCountStore,
                                 SizeCountTimestampStore& timestampStore);


/**
 * Scans the oplog for size and count deltas since the last checkpoint, accumulates absolute totals
 * per collection, and persists the result to `sizeCountStore` and `timestampStore`.
 *
 * Returns the number of writes to the `sizeCountStore`.
 *
 * Guarantee: If new oplog entries were written since the last checkpoint, the `timestampStore`
 * advances its global valid-as-of timestamp, even in absence of oplog entries with size and count
 * deltas, to ensure forward progress through the oplog.
 */
size_t advanceCheckpoint(OperationContext* opCtx,
                         SizeCountStore& sizeCountStore,
                         SizeCountTimestampStore& timestampStore);
}  // namespace mongo::replicated_fast_count
