// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"
#include "mongo/stdx/condition_variable.h"

#include <mutex>

namespace mongo::replicated_fast_count {

class SizeCountStore;
class SizeCountTimestampStore;

/**
 * Persists size and count checkpoints when signaled.
 */
class SizeCountCheckpointFlusher {
public:
    SizeCountCheckpointFlusher(SizeCountStore* sizeCountStore,
                               SizeCountTimestampStore* timestampStore);

    /**
     * Runs the flush loop until opCtx is interrupted (shutdown signal).
     */
    void run(OperationContext* opCtx, SizeCountCheckpointBuffer& buffer);

    /**
     * Signals that a flush should be performed on the next loop iteration.
     */
    void requestFlush();

    bool isFlushRequested_ForTest() const;
    void runOneFlushCycle_ForTest(OperationContext* opCtx, SizeCountCheckpointBuffer& buffer);

private:
    /**
     * Wrapper of a single flush cycle. Updates top-level flush metrics.
     */
    void _runOneFlushCycle(OperationContext* opCtx, SizeCountCheckpointBuffer& buffer);

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
     * Extracts the pending checkpoint from buffer and executes the flush. Returns a `FlushResult`
     * with metadata about the flush if data was persisted, or `boost::none` if the flush was a
     * no-op.
     */
    boost::optional<FlushResult> _doFlush(OperationContext* opCtx,
                                          SizeCountCheckpointBuffer& buffer);

    SizeCountStore* _sizeCountStore;
    SizeCountTimestampStore* _timestampStore;

    mutable std::mutex _mutex;
    stdx::condition_variable _flushCv;
    bool _flushRequested{false};
};

}  // namespace mongo::replicated_fast_count
