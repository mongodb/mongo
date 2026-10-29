// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_writer_batcher.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] repl {

/**
 * Writes oplog entries to the oplog.
 */
class [[MONGO_MOD_PUBLIC]] OplogWriter {
    OplogWriter(const OplogWriter&) = delete;
    OplogWriter& operator=(const OplogWriter&) = delete;

public:
    /**
     * Used to configure the behavior of this OplogWriter.
     */
    class Options {
    public:
        Options() = delete;
        explicit Options(bool skipWritesToOplogColl)
            : skipWritesToOplogColl(skipWritesToOplogColl) {}

        const bool skipWritesToOplogColl;
    };

    /**
     * Constructs this OplogWriter with specific options.
     */
    OplogWriter(executor::TaskExecutor* executor, OplogBuffer* writeBuffer, const Options& options);

    virtual ~OplogWriter() = default;

    /**
     * Returns this writer's input buffer.
     */
    OplogBuffer* getBuffer() const;

    /**
     * Starts this OplogWriter.
     * Use the Future object to be notified when this OplogWriter has finished shutting down.
     */
    Future<void> startup();

    /**
     * Starts the shutdown process for this OplogWriter.
     * It is safe to call shutdown() multiple times.
     */
    void shutdown();

    /**
     * Returns true if this OplogWriter is shutting down.
     */
    bool inShutdown() const;

    /**
     * Blocks until enough space is available.
     */
    void waitForSpace(OperationContext* opCtx, const OplogBuffer::Cost& cost);

    /**
     * Pushes operations read into oplog buffer.
     */
    void enqueue(OperationContext* opCtx,
                 OplogBuffer::Batch::const_iterator begin,
                 OplogBuffer::Batch::const_iterator end,
                 const OplogBuffer::Cost& cost);

    /**
     * Writes a batch of oplog entries to the oplog.
     *
     * Returns false if nothing is written, true otherwise.
     *
     * External states such as oplog visibility, replication opTimes and journaling
     * should be handled by the caller.
     */
    virtual bool writeOplogBatch(OperationContext* opCtx, const std::vector<BSONObj>& ops) = 0;

    /**
     * Schedules the writes of the oplog batch to the oplog using the thread pool. Use
     * waitForScheduledWrites() after calling this function to wait for the writes to complete.
     *
     * Returns false if no write is scheduled, true otherwise.
     *
     * External states such as oplog visibility, replication opTimes and journaling
     * should be handled by the caller.
     */
    virtual bool scheduleWriteOplogBatch(OperationContext* opCtx,
                                         const std::vector<OplogEntry>& ops) = 0;

    /**
     * Wait for all scheduled writes to completed. This should be used in conjunction
     * with scheduleWriteOplogBatch().
     */
    virtual void waitForScheduledWrites(OperationContext* opCtx) = 0;

    /**
     * Returns the options used to configure the behavior of this OplogWriter.
     */
    const Options& getOptions() const;

private:
    /**
     * Called from startup() to run oplog write loop.
     * Currently applicable to steady state replication only.
     * Implemented in subclasses but not visible otherwise.
     */
    virtual void _run() = 0;

protected:
    OplogWriterBatcher _batcher;

private:
    // Protects member data of this OplogWriter.
    mutable ObservableMutex<std::mutex> _mutex;

    // Used to schedule task for oplog write loop.
    // Not owned by us.
    executor::TaskExecutor* const _executor;

    // Not owned by us.
    OplogBuffer* const _writeBuffer;

    // Set to true if shutdown() has been called.
    bool _inShutdown = false;

    // Configures this OplogWriter.
    const Options _options;
};
}  // namespace repl
}  // namespace mongo
