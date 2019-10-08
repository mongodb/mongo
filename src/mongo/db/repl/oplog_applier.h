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
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"
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
              allowNamespaceNotFoundErrorsOnCrudOps(
                  inputMode == OplogApplication::Mode::kInitialSync ||
                  inputMode == OplogApplication::Mode::kRecovering),
              skipWritesToOplog(inputMode == OplogApplication::Mode::kRecovering) {}

        // Used to determine which operations should be applied. Only initial sync will set this to
        // be something other than the null optime.
        OpTime beginApplyingOpTime = OpTime();

        const OplogApplication::Mode mode;
        const bool allowNamespaceNotFoundErrorsOnCrudOps;
        const bool skipWritesToOplog;
    };

    /**
     * Controls what can popped from the oplog buffer into a single batch of operations that can be
     * applied using multiApply().
     */
    class BatchLimits {
    public:
        size_t bytes = 0;
        size_t ops = 0;

        // If provided, the batch will not include any operations with timestamps after this point.
        // This is intended for implementing slaveDelay, so it should be some number of seconds
        // before now.
        boost::optional<Date_t> slaveDelayLatestTimestamp = {};

        // If non-null, the batch will include operations with timestamps either
        // before-and-including this point or after it, not both.
        Timestamp forceBatchBoundaryAfter;
    };

    // Used to report oplog application progress.
    class Observer;

    using Operations = std::vector<OplogEntry>;

    // TODO (SERVER-43001): This potentially violates layering as OpQueueBatcher calls an
    // OplogApplier method.
    // Used to access batching logic.
    using GetNextApplierBatchFn = std::function<StatusWith<OplogApplier::Operations>(
        OperationContext* opCtx, const BatchLimits& batchLimits)>;

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
     * Returns this applier's buffer.
     */
    OplogBuffer* getBuffer() const;

    /**
     * Starts this OplogApplier.
     * Use the Future object to be notified when this OplogApplier has finished shutting down.
     */
    Future<void> startup();

    /**
     * Starts the shutdown process for this OplogApplier.
     * It is safe to call shutdown() multiplie times.
     */
    void shutdown();

    /**
     * Returns true if we are shutting down.
     */
    bool inShutdown() const;

    /**
     * Blocks until enough space is available.
     */
    void waitForSpace(OperationContext* opCtx, std::size_t size);

    /**
     * Pushes operations read into oplog buffer.
     * Accepts both Operations (OplogEntry) and OplogBuffer::Batch (BSONObj) iterators.
     * This supports current implementations of OplogFetcher and OplogBuffer which work in terms of
     * BSONObj.
     */
    void enqueue(OperationContext* opCtx,
                 Operations::const_iterator begin,
                 Operations::const_iterator end);
    void enqueue(OperationContext* opCtx,
                 OplogBuffer::Batch::const_iterator begin,
                 OplogBuffer::Batch::const_iterator end);

    /**
     * Returns a new batch of ops to apply.
     * A batch may consist of:
     *     at most "BatchLimits::ops" OplogEntries
     *     at most "BatchLimits::bytes" worth of OplogEntries
     *     only OplogEntries from before the "BatchLimits::slaveDelayLatestTimestamp" point
     *     a single command OplogEntry (excluding applyOps, which are grouped with CRUD ops)
     */
    StatusWith<Operations> getNextApplierBatch(OperationContext* opCtx,
                                               const BatchLimits& batchLimits);

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
     *
     * TODO: remove when enqueue() is implemented.
     */
    StatusWith<OpTime> multiApply(OperationContext* opCtx, Operations ops);

    const Options& getOptions() const;
    /**
     * Step-up
     * =======
     * On stepup, repl coord enters catch-up mode. It's the same as the secondary mode from
     * the perspective of producer and applier, so there's nothing to do with them.
     * When a node enters drain mode, producer state = Stopped, applier state = Draining.
     *
     * If the applier state is Draining, it will signal repl coord when there's nothing to apply.
     * The applier goes into Stopped state at the same time.
     *
     * The states go like the following:
     * - secondary and during catchup mode
     * (producer: Running, applier: Running)
     *      |
     *      | finish catch-up, enter drain mode
     *      V
     * - drain mode
     * (producer: Stopped, applier: Draining)
     *      |
     *      | applier signals drain is complete
     *      V
     * - primary is in master mode
     * (producer: Stopped, applier: Stopped)
     *
     *
     * Step-down
     * =========
     * The state transitions become:
     * - primary is in master mode
     * (producer: Stopped, applier: Stopped)
     *      |
     *      | step down
     *      V
     * - secondary mode, starting bgsync
     * (producer: Starting, applier: Running)
     *      |
     *      | bgsync runs start()
     *      V
     * - secondary mode, normal
     * (producer: Running, applier: Running)
     *
     * When a node steps down during draining mode, it's OK to change from (producer: Stopped,
     * applier: Draining) to (producer: Starting, applier: Running).
     *
     * When a node steps down during catchup mode, the states remain the same (producer: Running,
     * applier: Running).
     */
    enum class ApplierState { Running, Draining, Stopped };

    /**
     * In normal cases: Running -> Draining -> Stopped -> Running.
     * Draining -> Running is also possible if a node steps down during drain mode.
     *
     * Only the applier can make the transition from Draining to Stopped by calling
     * signalDrainComplete().
     */
    virtual ApplierState getApplierState() const;

    virtual void setApplierState(ApplierState st);

private:
    /**
     * Pops the operation at the front of the OplogBuffer.
     */
    void _consume(OperationContext* opCtx, OplogBuffer* oplogBuffer);

    /**
     * Called from startup() to run oplog application loop.
     * Currently applicable to steady state replication only.
     * Implemented in subclasses but not visible otherwise.
     */
    virtual void _run(OplogBuffer* oplogBuffer) = 0;

    /**
     * Called from multiApply() to apply a batch of operations in parallel.
     * Implemented in subclasses but not visible otherwise.
     */
    virtual StatusWith<OpTime> _multiApply(OperationContext* opCtx, Operations ops) = 0;

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

    ApplierState _applierState = ApplierState::Running;
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
    virtual void onBatchBegin(const OplogApplier::Operations& operations) = 0;

    /**
     * When the OplogApplier has completed applying a batch of operations, it will call this
     * function to report the last optime applied on success. Any errors during oplog application
     * will also be here.
     */
    virtual void onBatchEnd(const StatusWith<OpTime>& lastOpTimeApplied,
                            const OplogApplier::Operations& operations) = 0;
};

class NoopOplogApplierObserver : public repl::OplogApplier::Observer {
public:
    void onBatchBegin(const repl::OplogApplier::Operations&) final {}
    void onBatchEnd(const StatusWith<repl::OpTime>&, const repl::OplogApplier::Operations&) final {}
};

extern NoopOplogApplierObserver noopOplogApplierObserver;

/**
 * Creates thread pool for writer tasks.
 */
std::unique_ptr<ThreadPool> makeReplWriterPool();
std::unique_ptr<ThreadPool> makeReplWriterPool(int threadCount);

/**
 * Returns maximum number of operations in each batch that can be applied using multiApply().
 */
std::size_t getBatchLimitOplogEntries();

/**
 * Calculates batch limit size (in bytes) using the maximum capped collection size of the oplog
 * size.
 * Batches are limited to 10% of the oplog.
 */
std::size_t getBatchLimitOplogBytes(OperationContext* opCtx, StorageInterface* storageInterface);

}  // namespace repl
}  // namespace mongo
