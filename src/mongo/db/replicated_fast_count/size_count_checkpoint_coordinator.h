// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

namespace mongo::replicated_fast_count {

/**
 * Central point for checkpointing replicated size and count. Manages the coordination between
 * tailing the oplog for new size and count deltas, materializing the deltas into a logical size and
 * count checkpoint, and flushing the checkpoint upon request.
 *
 * Intended for single lifecycle use.
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

    std::unique_ptr<SizeCountCheckpointFlusher> _flusher;

    /**
     * Written to by the tailing thread and read / cleared by the flushing thread, holds the
     * accumulated size and count deltas for the upcoming size and count checkpoint.
     *
     * Snapshotting logic allows for flushes to do I/O while tailing continues in the background.
     */
    std::unique_ptr<SizeCountCheckpointBuffer> _buffer;

    stdx::thread _tailerThread;
    stdx::thread _flushThread;

    OperationContextGroup _opCtxGroup;

    mutable std::mutex _mutex;
    /**
     * Indicates that the destructor has begun. Once set, never reset. Prevents worker threads from
     * creating new opCtxs after the destructor has interrupted the opCtx group.
     */
    bool _shutdownRequested{false};
};

}  // namespace mongo::replicated_fast_count
