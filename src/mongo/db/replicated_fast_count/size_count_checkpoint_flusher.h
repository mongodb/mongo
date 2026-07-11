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
     * Extracts the pending checkpoint from buffer and executes the flush. Returns the number of
     * collections updated. All errors managed internally. Returns 0 if the flush wasn't executed.
     */
    size_t _doFlush(OperationContext* opCtx, SizeCountCheckpointBuffer& buffer);

    SizeCountStore* _sizeCountStore;
    SizeCountTimestampStore* _timestampStore;

    mutable std::mutex _mutex;
    stdx::condition_variable _flushCv;
    bool _flushRequested{false};
};

}  // namespace mongo::replicated_fast_count
