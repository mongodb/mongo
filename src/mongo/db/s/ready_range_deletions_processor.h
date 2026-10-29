// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/executor/task_executor.h"
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
     * Remove a range deletion from the head of the queue. Supposed to be called only once a
     * range deletion successfully finishes.
     */
    void _completedRangeDeletion();

    /*
     * Code executed by the internal thread
     */
    void _runRangeDeletions();

    /**
     * Returns a copy of the task at the front of the queue.
     */
    RangeDeletionTask _peekFront() const;


    bool _shouldDeferRangeDeletionForResharding(NamespaceString nss, OperationContext* opCtx);

    /**
     * Pops the current task from the queue and re-enqueues it after the given delay. If the
     * executor is shutting down, the task is not re-enqueued (it will be recovered from disk on
     * the next step-up).
     */
    void _rescheduleRangeDeletion(const RangeDeletionTask& task,
                                  Seconds delay,
                                  std::string_view reason);

    ServiceContext* const _service;

    mutable std::mutex _mutex;

    State _state{kInitializing};

    /*
     * Condition variable notified when:
     * - The component has been initialized (the operation context has been instantiated)
     * - The instance is shutting down (the operation context has been marked killed)
     * - A new range deletion is scheduled (the queue size has increased by one)
     */
    stdx::condition_variable _condVar;

    /* Queue containing scheduled range deletions */
    std::queue<RangeDeletionTask> _queue;

    /* Pointer to the (one and only) operation context used by the thread */
    ServiceContext::UniqueOperationContext _threadOpCtxHolder;

    /*
     * Raw pointer to the operation context of the alternate client ("range-deleter-batch")
     * currently executing a batch deletion, or nullptr when no batch is in flight. Owned by the
     * range-deleter thread; only ever set/cleared by it under `_mutex`. Tracked so that shutdown()
     * can interrupt the in-flight batch directly rather than relying on cancellation delivered via
     * the executor, which may already be shutting down on stepdown.
     */
    OperationContext* _batchOpCtx{nullptr};

    SharedPromise<void> _beginProcessingSignal;

    /* Thread consuming the range deletions queue */
    stdx::thread _thread;

    /*
     * An executor that is managed (startup & shutdown) by the RangeDeleterService. An example
     * use of this is to schedule a retry of task that errored at a later time.
     */
    std::shared_ptr<executor::TaskExecutor> _executor;
};

}  // namespace mongo
