// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_streaming_oplog_delta_accumulator.h"

namespace mongo::replicated_fast_count {

class SizeCountStore;
class SizeCountTimestampStore;

namespace flusher {

/**
 * Metadata about a successful flush.
 */
struct FlushResult {
    /**
     * The timestamp of the previous successful flush. If this is the first flush, then its
     * value is `Timestamp::min()`.
     */
    Timestamp previousValidAsOfTS;

    /**
     * The timestamp persisted to the fast count timestamp store during this flush.
     */
    Timestamp newValidAsOfTS;

    /**
     * The number of deltas in the checkpoint buffer when checked out.
     */
    size_t checkpointBufferSize;

    /**
     * The number of entries written to the fast count metadata store. This value is <=
     * `checkpointBufferSize` because some deltas can be skipped. See
     * `persistCheckpointSnapshot()`.
     */
    size_t entryWriteCount;

    /**
     * The number of attempts required to successfully flush the checkpoint to the fast count
     * metadata store and the fast count timestamp store. This value is 1 unless there was a
     * write conflict.
     */
    size_t flushAttempts;
};

/**
 * Persists the checkpoint metadata `batch` to the `SizeCountStore` and `SizeCountTimestampStore`,
 * retrying if a `WriteConflictException` occurs.
 *
 * Returns a `FlushResult` if the flush was successful, or `boost::none` otherwise.
 */
boost::optional<FlushResult> flush(OperationContext* opCtx,
                                   SizeCountStore& sizeCountStore,
                                   SizeCountTimestampStore& timestampStore,
                                   const OplogScanResult& batch);

}  // namespace flusher
}  // namespace mongo::replicated_fast_count
