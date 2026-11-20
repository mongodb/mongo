// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_group.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"

#include <queue>
#include <string_view>

namespace mongo {

/*
 * Class enclosing a thread continuously processing "ready" range deletions, meaning tasks
 * that are allowed to be processed (already drained ongoing queries and already waited for
 * `orphanCleanupDelaySecs`).
 */
class ReadyRangeDeletionsProcessor {
public:
    ReadyRangeDeletionsProcessor(OperationContext* opCtx,
                                 std::shared_ptr<executor::TaskExecutor> executor);
    ~ReadyRangeDeletionsProcessor();

    void beginProcessing();

    /*
     * Interrupt ongoing range deletions
     */
    void shutdown();

    /*
     * Schedule a range deletion at the end of the queue
     */
    void emplaceRangeDeletion(const RangeDeletionTask& rdt);

    /*
     * Notify the dispatcher that the max number of threads has changed
     */
    void updateWorkerThreadCount();

private:
    enum State { kInitializing, kRunning, kStopped };

    /*
     * Return true if this processor have been shutted down
     */
    bool _stopRequested() const;

    void _transitionState(WithLock, State newState);
    bool _validateStateTransition(State oldState, State newState) const;
    bool _isStateTransitionValid(State oldState, State newState) const;

    /*
     * Releases one in-flight slot and wakes the dispatcher. Called when
     * _processRangeDeletion returns, including completion, reschedule, stepdown, and throws.
     */
    void _dropInFlightTask();

    /*
     * Code executed by the dispatcher thread
     */
    void _dispatchRangeDeletions();

    /*
     * Pops the front of the ready queue and counts the task as in flight.
     */
    RangeDeletionTask _peekFrontAndAddToInFlight();

    /*
     * Code executed by the worker threads.
     */
    void _processRangeDeletion(RangeDeletionTask task);

    bool _shouldDeferRangeDeletionForResharding(NamespaceString nss);

    /**
     * Enqueues the task again after the given delay. The in-flight slot is released when
     * _processRangeDeletion returns. If the executor is shutting down, the task is not enqueued
     * (it will be recovered from disk on the next step-up).
     */
    void _rescheduleRangeDeletion(const RangeDeletionTask& task,
                                  Seconds delay,
                                  std::string_view reason);

    ServiceContext* const _service;

    mutable std::mutex _mutex;

    State _state{kInitializing};

    /*
     * Condition variable notified when:
     * - A new range deletion is scheduled (the queue size has increased by one)
     * - An inflight task has been dropped
     * - The rangeDeleterThreadsCount server parameter has been changed
     */
    stdx::condition_variable _condVar;

    /* Queue containing scheduled range deletions */
    std::queue<RangeDeletionTask> _queue;

    /*
     * Operation contexts of the dispatcher and the workers, so shutdown can interrupt them
     * together.
     */
    OperationContextGroup _opCtxGroup;

    /* Counter that keeps track of how many tasks are currently in-flight*/
    size_t _inflightCounter{0};

    SharedPromise<void> _beginProcessingSignal;

    /* Thread pool with the worker threads that will perform the range deletions */
    std::unique_ptr<ThreadPool> _workerPool;

    /* Thread consuming the range deletions queue */
    stdx::thread _thread;

    /*
     * An executor that is managed (startup & shutdown) by the RangeDeleterService. An example
     * use of this is to schedule a retry of task that errored at a later time.
     */
    std::shared_ptr<executor::TaskExecutor> _executor;
};

}  // namespace mongo
