// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/db/repl/oplog_batch.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {

class OplogWriterBatcher {
    OplogWriterBatcher(const OplogWriterBatcher&) = delete;
    OplogWriterBatcher& operator=(const OplogWriterBatcher&) = delete;

public:
    class BatchLimits {
    public:
        size_t minBytes;
        size_t maxBytes;
        size_t maxCount;
    };

    OplogWriterBatcher(OplogBuffer* oplogBuffer);
    ~OplogWriterBatcher();

    // Get a batch from the underlying OplogBuffer for the OplogWriter to write to disk. If the
    // buffer is empty, the batcher will wait for the next batch until the maxWaitTime timeout is
    // hit, so this function can return an empty batch.
    OplogWriterBatch getNextBatch(OperationContext* opCtx, Seconds maxWaitTime);
    OplogWriterBatch getNextBatch(OperationContext* opCtx,
                                  Seconds maxWaitTime,
                                  const BatchLimits& batchLimits);

private:
    /**
     * Merge all OplogWriterBatches into one OplogWriterBatch.
     */
    OplogWriterBatch _mergeBatches(std::vector<OplogWriterBatch>& batches,
                                   size_t totalBytes,
                                   size_t totalOps);

    /**
     * Wait until the buffer is not empty or deadline arrives then return true if we should process
     * next batch or false if we should stop processing.
     */
    bool _waitForData(OperationContext* opCtx, Date_t waitDeadline);

    /**
     * If secondaryDelaySecs is enabled, this function calculates the most recent timestamp of any
     * oplog entries that can be be returned in a batch.
     */
    boost::optional<Date_t> _calculateSecondaryDelaySecsLatestTimestamp(OperationContext* opCtx,
                                                                        Date_t now);

    /**
     * Get a batch from either the _stashedBatch or _oplogBuffer. If there is an entry in the batch
     * not passing delaySecsLatestTimestamp, keep the whole batch in _stashedBatch and revisit next
     * time.
     */
    bool _pollFromBuffer(OperationContext* opCtx,
                         OplogWriterBatch* batch,
                         boost::optional<Date_t>& delaySecsLatestTimestamp);

    /**
     * Return the current term if the writer is in drain mode and the buffer is exhausted.
     */
    boost::optional<long long> _isBufferExhausted(OperationContext* opCtx);

private:
    // This should be a OplogBuffer that supports batch operations.
    // Not owned by us.
    OplogBuffer* const _oplogBuffer;

    // Keep the last batch from buffer when the batch is not passing secondaryDelaySecs. This will
    // be reset to null once that batch passes secondaryDelaySecs and return to the caller.
    boost::optional<OplogWriterBatch> _stashedBatch = boost::none;
};

}  // namespace repl
}  // namespace mongo
