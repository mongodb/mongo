// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"

#include <queue>
#include <string_view>

namespace mongo {

struct InFlightEntry {
    RangeDeletionTask task;
    OperationContext* opCtx = nullptr;
};
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
    void _completedRangeDeletion(mongo::UUID taskId);

    /*
     * Code executed by the dispatcher thread
     */
    void _dispatchRangeDeletions();

    /*
     * Takes the top element of the ready queue and creates its in-flight entry.
     */
    InFlightEntry _peekFrontAndAddToInFlight();

    /*
     * Code executed by the worker threads.
     */
    void _processRangeDeletion(InFlightEntry taskEntry);

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

    stdx::unordered_map<mongo::UUID, InFlightEntry, UUID::Hash> _inflightMap;

    /* Pointer to the (one and only) operation context used by the thread */
    ServiceContext::UniqueOperationContext _threadOpCtxHolder;

    SharedPromise<void> _beginProcessingSignal;

    /*
     * Maximum number of worker threads. Fixed at 1 until a server parameter can change it.
     * TODO SERVER-137792: Replace _maxThreads with the server parameter rangeDeleterThreadsCount.
     */
    size_t _maxThreads{1};

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
