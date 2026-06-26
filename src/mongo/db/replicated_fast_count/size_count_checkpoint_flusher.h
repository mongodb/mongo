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
