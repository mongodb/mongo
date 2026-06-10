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
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"
#include "mongo/db/storage/record_store.h"

#include <memory>

namespace mongo::replicated_fast_count {
/**
 * Tails the oplog to buffer size/count deltas incrementally, enabling faster checkpoints.
 */
class SizeCountCheckpointOplogTailer {
public:
    /**
     * Reuses a cursor across iterations to avoid open/close overhead.
     * Tracks lastBufferedRid so the main loop can always use seekExact (faster than seek).
     */
    struct TailerState {
        std::unique_ptr<SeekableRecordCursor> cursor;
        RecordId lastBufferedRid;
    };

    /**
     * Tails the oplog for size/count deltas since startAfter (exclusive), appending to buffer.
     */
    void run(OperationContext* opCtx, Timestamp startAfter, SizeCountCheckpointBuffer& buffer);

    /**
     * Returns boost::none if the oplog is empty or doesn't exist.
     */
    boost::optional<TailerState> bootstrap_ForTest(OperationContext* opCtx,
                                                   Timestamp startAfter,
                                                   SizeCountCheckpointBuffer& buffer) {
        return _bootstrap(opCtx, startAfter, buffer);
    }

    /**
     * Non-blocking single iteration: drains entries up to the no-holes point.
     * Returns true if buffer was updated.
     */
    bool runOneIteration_ForTest(OperationContext* opCtx,
                                 boost::optional<TailerState>& state,
                                 SizeCountCheckpointBuffer& buffer);

private:
    /**
     * Returns none if the oplog is empty. Otherwise, `lastBufferedRid` refers to an
     * already processed record so the main loop can always seekExact to it.
     *
     * Timestamp::min() means start from the beginning: since it never refers to a real entry,
     * the first record is processed immediately to establish the invariant.
     * Otherwise, size and count is checkpointed up to the oplog entry with timestamp `startAfter`,
     * and it should not be double processed.
     */
    boost::optional<TailerState> _bootstrap(OperationContext* opCtx,
                                            Timestamp startAfter,
                                            SizeCountCheckpointBuffer& buffer);
};
}  // namespace mongo::replicated_fast_count


