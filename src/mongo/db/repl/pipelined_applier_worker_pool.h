// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <deque>
#include <functional>
#include <vector>

namespace mongo::repl {

/**
 * A fixed set of persistent worker threads, each consuming its own queue of work items. Work
 * items enqueued to the same worker index are consumed serially, in FIFO order, by a single
 * thread; distinct workers consume independently. Each worker is backed by a single-threaded
 * ThreadPool. _startWorkers gives every worker thread an internally authorized Client that stepdown
 * cannot kill.
 *
 * The worker threads start at construction and run until shutdownAndJoin(), which consumes every
 * previously enqueued work item before joining the threads. Once a shutdown has been attempted,
 * new work is rejected.
 */
class [[MONGO_MOD_PUBLIC]] PipelinedApplierWorkerPool {
public:
    /**
     * A unit of work consumed by a worker: the ops slice of a single dispatched batch routed to
     * that worker.
     */
    struct WorkItem {
        std::vector<OplogEntry> ops;
    };

    /**
     * Invoked on the owning worker's thread, once per work item, in per-worker FIFO order.
     */
    using ConsumeFn = std::function<void(size_t workerIdx, const WorkItem& item)>;

    PipelinedApplierWorkerPool(size_t numWorkers, ConsumeFn consumeFn);

    ~PipelinedApplierWorkerPool();

    /**
     * Schedules a work item for consumption by the given worker. Throws ShutdownInProgress if a
     * shutdown has been attempted. The shutdown check is best-effort for callers sequenced with
     * shutdownAndJoin (there is a single enqueuing thread, which is also the one that shuts the
     * pool down); an enqueue racing shutdownAndJoin from another thread is a contract violation.
     */
    void enqueue(size_t workerIdx, WorkItem item);

    /**
     * Waits for all enqueued work items to be consumed, then shuts down and joins the worker
     * threads. Subsequent calls are no-ops.
     */
    void shutdownAndJoin();

    size_t numWorkers() const {
        return _numWorkers;
    }

    /**
     * Appends one subdocument per worker with its aggregate scheduled and executed work item
     * counts.
     */
    void report(BSONObjBuilder& bob) const;

private:
    /**
     * A worker: a thread pool and its work item counters. _startWorkers configures each pool
     * with a single thread.
     */
    struct Worker {
        explicit Worker(ThreadPool::Options options) : pool(std::move(options)) {}

        ThreadPool pool;
        Atomic<uint64_t> scheduled{0};
        Atomic<uint64_t> executed{0};
    };

    static std::deque<Worker> _startWorkers(size_t numWorkers);

    ConsumeFn _consumeFn;
    Atomic<bool> _isShutdown{false};
    const size_t _numWorkers;
    // The set of workers is fixed for the object's lifetime.
    std::deque<Worker> _workers;
};

}  // namespace mongo::repl
