/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <cstddef>
#include <memory>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_batcher.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

/**
 * Applies oplog entries.
 * Reads from an OplogBuffer batches of operations that may be applied in parallel.
 */
class OplogApplier {
    OplogApplier(const OplogApplier&) = delete;
    OplogApplier& operator=(const OplogApplier&) = delete;

public:
    /**
     * Used to configure behavior of this OplogApplier.
     **/
    class Options {
    public:
        Options() = delete;

        explicit Options(OplogApplication::Mode inputMode)
            : mode(inputMode),
              allowNamespaceNotFoundErrorsOnCrudOps(inputMode ==
                                                        OplogApplication::Mode::kInitialSync ||
                                                    OplogApplication::inRecovering(inputMode)),
              skipWritesToOplog(OplogApplication::inRecovering(inputMode)),
              skipWritesToChangeCollection(false) {}

        Options(OplogApplication::Mode inputMode,
                bool skipWritesToOplog,
                bool skipWritesToChangeCollection)
            : mode(inputMode),
              allowNamespaceNotFoundErrorsOnCrudOps(inputMode ==
                                                        OplogApplication::Mode::kInitialSync ||
                                                    OplogApplication::inRecovering(inputMode)),
              skipWritesToOplog(skipWritesToOplog),
              skipWritesToChangeCollection(skipWritesToChangeCollection) {}

        Options(OplogApplication::Mode mode,
                bool allowNamespaceNotFoundErrorsOnCrudOps,
                bool skipWritesToOplog,
                bool skipWritesToChangeCollection)
            : mode(mode),
              allowNamespaceNotFoundErrorsOnCrudOps(allowNamespaceNotFoundErrorsOnCrudOps),
              skipWritesToOplog(skipWritesToOplog),
              skipWritesToChangeCollection(skipWritesToChangeCollection) {}

        // Used to determine which operations should be applied. Only initial sync will set this to
        // be something other than the null optime.
        OpTime beginApplyingOpTime = OpTime();

        const OplogApplication::Mode mode;
        const bool allowNamespaceNotFoundErrorsOnCrudOps;
        const bool skipWritesToOplog;
        const bool skipWritesToChangeCollection;
    };

    // Used to report oplog application progress.
    class Observer;

    /**
     * OplogBatcher is an implementation detail that should be abstracted from all levels above
     * the OplogApplier. Parts of the system that need to modify BatchLimits can do so through the
     * OplogApplier.
     */
    using BatchLimits = OplogBatcher::BatchLimits;

    /**
     * Constructs this OplogApplier with specific options.
     * Obtains batches of operations from the OplogBuffer to apply.
     * Reports oplog application progress using the Observer.
     */
    OplogApplier(executor::TaskExecutor* executor,
                 OplogBuffer* oplogBuffer,
                 Observer* observer,
                 const Options& options);

    virtual ~OplogApplier() = default;

    /**
     * Returns this applier's input buffer.
     */
    OplogBuffer* getBuffer() const;

    /**
     * Starts this OplogApplier.
     * Use the Future object to be notified when this OplogApplier has finished shutting down.
     */
    Future<void> startup();

    /**
     * Starts the shutdown process for this OplogApplier.
     * It is safe to call shutdown() multiple times.
     */
    void shutdown();

    /**
     * Returns true if this OplogApplier is shutting down.
     */
    bool inShutdown() const;

    /**
     * Blocks until enough space is available.
     */
    void waitForSpace(OperationContext* opCtx, std::size_t size);

    /**
     * Pushes operations read into oplog buffer.
     * Accepts both std::vector<OplogEntry> and OplogBuffer::Batch (BSONObj) iterators.
     * This supports current implementations of OplogFetcher and OplogBuffer which work in terms of
     * BSONObj.
     */
    void enqueue(OperationContext* opCtx,
                 std::vector<OplogEntry>::const_iterator begin,
                 std::vector<OplogEntry>::const_iterator end,
                 boost::optional<std::size_t> bytes = boost::none);
    void enqueue(OperationContext* opCtx,
                 OplogBuffer::Batch::const_iterator begin,
                 OplogBuffer::Batch::const_iterator end,
                 boost::optional<std::size_t> bytes = boost::none);
    /**
     * Applies a batch of oplog entries by writing the oplog entries to the local oplog and then
     * using a set of threads to apply the operations.
     *
     * If the batch application is successful, returns the optime of the last op applied, which
     * should be the last op in the batch.
     * Returns ErrorCodes::CannotApplyOplogWhilePrimary if the node has become primary.
     *
     * To provide crash resilience, this function will advance the persistent value of 'minValid'
     * to at least the last optime of the batch. If 'minValid' is already greater than or equal
     * to the last optime of this batch, it will not be updated.
     */
    StatusWith<OpTime> applyOplogBatch(OperationContext* opCtx, std::vector<OplogEntry> ops);

    virtual void scheduleWritesToOplogAndChangeCollection(OperationContext* opCtx,
                                                          StorageInterface* storageInterface,
                                                          ThreadPool* writerPool,
                                                          const std::vector<OplogEntry>& ops,
                                                          bool skipWritesToOplog) = 0;

    /**
     * Calls the OplogBatcher's getNextApplierBatch.
     */
    StatusWith<OplogApplierBatch> getNextApplierBatch(
        OperationContext* opCtx,
        const BatchLimits& batchLimits,
        Milliseconds waitToFillBatch = Milliseconds(0));

    const Options& getOptions() const;

private:
    /**
     * Called from startup() to run oplog application loop.
     * Currently applicable to steady state replication only.
     * Implemented in subclasses but not visible otherwise.
     */
    virtual void _run(OplogBuffer* oplogBuffer) = 0;

    /**
     * Called from applyOplogBatch() to apply a batch of operations in parallel.
     * Implemented in subclasses but not visible otherwise.
     */
    virtual StatusWith<OpTime> _applyOplogBatch(OperationContext* opCtx,
                                                std::vector<OplogEntry> ops) = 0;

    // Used to schedule task for oplog application loop.
    // Not owned by us.
    executor::TaskExecutor* const _executor;

    // Not owned by us.
    OplogBuffer* const _oplogBuffer;

    // Not owned by us.
    Observer* const _observer;

    // Protects member data of OplogApplier.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("OplogApplier::_mutex");

    // Set to true if shutdown() has been called.
    bool _inShutdown = false;

    // Configures this OplogApplier.
    const Options _options;

protected:
    // Handles consuming oplog entries from the OplogBuffer for oplog application.
    std::unique_ptr<OplogBatcher> _oplogBatcher;
};

/**
 * The OplogApplier reports its progress using the Observer interface.
 */
class OplogApplier::Observer {
public:
    virtual ~Observer() = default;

    /**
     * Called when the OplogApplier is ready to start applying a batch of operations read from the
     * OplogBuffer.
     **/
    virtual void onBatchBegin(const std::vector<OplogEntry>& operations) = 0;

    /**
     * When the OplogApplier has completed applying a batch of operations, it will call this
     * function to report the last optime applied on success. Any errors during oplog application
     * will also be here.
     */
    virtual void onBatchEnd(const StatusWith<OpTime>& lastOpTimeApplied,
                            const std::vector<OplogEntry>& operations) = 0;
};

class NoopOplogApplierObserver : public repl::OplogApplier::Observer {
public:
    void onBatchBegin(const std::vector<OplogEntry>&) final {}
    void onBatchEnd(const StatusWith<repl::OpTime>&, const std::vector<OplogEntry>&) final {}
};

extern NoopOplogApplierObserver noopOplogApplierObserver;

/**
 * Creates the default thread pool for writer tasks.
 */
std::unique_ptr<ThreadPool> makeReplWriterPool();
std::unique_ptr<ThreadPool> makeReplWriterPool(int threadCount);

/**
 * Creates a thread pool suitable for writer tasks, with the specified name
 */
std::unique_ptr<ThreadPool> makeReplWriterPool(int threadCount,
                                               StringData name,
                                               bool isKillableByStepdown = false);

}  // namespace repl
}  // namespace mongo
