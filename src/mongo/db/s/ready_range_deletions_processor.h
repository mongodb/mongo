/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"

#include <queue>

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

    ServiceContext* const _service;

    mutable stdx::mutex _mutex;

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
