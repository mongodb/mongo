/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_batch.h"
#include "mongo/db/repl/oplog_batcher.h"

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
    bool _waitForData(OperationContext* opCtx, Seconds maxWaitTime);

    /**
     * If secondaryDelaySecs is enabled, this function calculates the most recent timestamp of any
     * oplog entries that can be be returned in a batch.
     */
    boost::optional<Date_t> _calculateSecondaryDelaySecsLatestTimestamp();

    /**
     * Get a batch from either the _stashedBatch or _oplogBuffer. If there is an entry in the batch
     * not passing delaySecsLatestTimestamp, keep the whole batch in _stashedBatch and revisit next
     * time.
     */
    bool _pollFromBuffer(OperationContext* opCtx,
                         OplogWriterBatch* batch,
                         boost::optional<Date_t>& delaySecsLatestTimestamp);

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
