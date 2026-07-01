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
#include "mongo/db/operation_context_group.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_flusher.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_oplog_tailer.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <mutex>
#include <string_view>

namespace mongo::replicated_fast_count {

/**
 * Central point for checkpointing replicated size and count. Manages the coordination between
 * tailing the oplog for new size and count deltas, materializing the deltas into a logical size and
 * count checkpoint, and flushing the checkpoint upon request.
 *
 * Intended for single lifecycle use - once shutdown() is called, startup() is effectively a no-op.
 */
class SizeCountCheckpointCoordinator {
public:
    SizeCountCheckpointCoordinator(SizeCountStore& sizeCountStore,
                                   SizeCountTimestampStore& timestampStore,
                                   UUID oplogUuid,
                                   Timestamp startCheckpointingAfterTS);

    ~SizeCountCheckpointCoordinator();

    SizeCountCheckpointCoordinator(const SizeCountCheckpointCoordinator&) = delete;
    SizeCountCheckpointCoordinator& operator=(const SizeCountCheckpointCoordinator&) = delete;

    /**
     * Spawns background threads for tailing and flushing.
     */
    void startup(ServiceContext* service);
    void shutdown();

    /**
     * Asynchronous request to snapshot and flush the newest size and count checkpoint.
     */
    void requestFlush();

    /**
     * Performs a synchronous oplog tailing iteration then flush iteration.
     */
    void flushSync_ForTest(OperationContext* opCtx);
    bool isRunning_ForTest() const;
    bool isFlushRequested_ForTest() const;

private:
    /**
     * Continuously tails new oplog entries and populates the buffer with their accumulated size and
     * count deltas.
     */
    void _runTailerThread(ServiceContext* service);

    /**
     * When signaled, snapshots the accumulated size and count deltas in the buffer and flushes them
     * to disk.
     */
    void _runFlushThread(ServiceContext* service);

    void _handleWorkerFailure(Status status, std::string_view message);

    std::unique_ptr<SizeCountCheckpointFlusher> _flusher;

    /**
     * Written to by the tailing thread and read / cleared by the flushing thread, holds the
     * accumulated size and count deltas for the upcoming size and count checkpoint.
     *
     * Snapshotting logic allows for flushes to do I/O while tailing continues in the background.
     */
    std::unique_ptr<SizeCountCheckpointBuffer> _buffer;
    SizeCountStore& _sizeCountStore;
    SizeCountTimestampStore& _timestampStore;

    mutable std::mutex _mutex;

    /**
     * Indicates the background threads were started. Once set to `true` in `startup()`, is never
     * reset. This ensures idempotency for `startup()`.
     */
    bool _started{false};

    /**
     * Indicates that shutdown has been requested. Once set, never reset. Ensures idempotency with
     * `shutdown()`.
     */
    bool _shutdownRequested{false};

    stdx::thread _tailerThread;
    stdx::thread _flushThread;

    OperationContextGroup _opCtxGroup;
};

}  // namespace mongo::replicated_fast_count
