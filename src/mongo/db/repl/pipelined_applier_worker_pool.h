// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/repl/clang_checked/checked_mutex.h"
#include "mongo/db/repl/clang_checked/mutex.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/serial_processor.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_validator.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace mongo::repl {

/**
 * A set of persistent worker threads, each consuming its own queue of work items. Work
 * items enqueued to the same worker index are consumed serially, in FIFO order, by a single
 * thread; distinct workers consume independently. Each worker is backed by a single-threaded
 * SerialProcessor. _startWorkers gives every worker thread an internally authorized Client that
 * stepdown cannot kill. The set can be resized with resizeWorkers(); every time the dispatcher
 * recieves a batch, it resizes the set to the current `replWriterThreadCount`.
 *
 * The worker threads start at construction and run until shutdownAndJoin(), which consumes every
 * previously enqueued work item before joining the threads. Once a shutdown has been attempted,
 * new work is rejected.
 *
 * Note: The pool has no internal synchronization mechanisms. The public methods here *must* be
 * called exclusively from the dispatch thread to avoid data races. (The `_metricsCollector` only
 * reads from the `_workers` deque, and its lifetime is directly managed by the pool itself so that
 * it never holds pointers to freed memory.)
 */
class [[MONGO_MOD_PUBLIC]] PipelinedApplierWorkerPool {
public:
    /**
     * A unit of work consumed by a worker: the ops slice of a single dispatched batch routed to
     * that worker. `ops` holds ApplierOperations whose `op` pointers alias memory owned by the
     * dispatching batch.
     */
    struct WorkItem {
        std::vector<ApplierOperation> ops;
        std::shared_ptr<std::atomic<uint32_t>> batchRemainingWorkers;
        // Points to this worker's slot, owned by the in-flight batch until completion.
        WorkerMultikeyPathInfo* multikeyPathInfo = nullptr;
        // Records metadata deltas during application, owned by the in-flight batch until
        // completion.
        replicated_fast_count::ValidatorBuffers::value_type* validatorBuffer = nullptr;
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

    // Waits for all queued and active work items to finish. The caller must prevent concurrent
    // enqueueing.
    void waitForIdle();

    /**
     * Waits for all enqueued work items to be consumed, then shuts down and joins the worker
     * threads. Subsequent calls are no-ops.
     */
    void shutdownAndJoin();

    size_t numWorkers() const {
        clang_checked::lock_guard lk{_mutex};
        return _numWorkers;
    }

    /**
     * Drains every worker, then replaces the worker set with `newSize` fresh workers. Callers must
     * prevent concurrent enqueueing. The queue-depth collector is stopped while the workers are
     * replaced and restarted afterward. The new workers start with zeroed counters.
     * Returns the previous number of workers.
     */
    size_t resizeWorkers(size_t newSize);

    /**
     * Appends one subdocument per worker with its aggregate scheduled and executed work item
     * counts.
     */
    void report(BSONObjBuilder& bob) const;

private:
    /**
     * A worker: a serial processor and its work item counters.
     */
    struct Worker {
        std::unique_ptr<SerialProcessor<WorkItem>> processor;
        Atomic<uint64_t> scheduled{0};
        Atomic<uint64_t> executed{0};
    };

    [[nodiscard]] static std::deque<Worker> _startWorkers(size_t numWorkers, ConsumeFn consumeFn);
    [[nodiscard]] static PeriodicJobAnchor _makeMetricsCollector(const std::deque<Worker>& workers);

    Atomic<bool> _isShutdown{false};
    mutable clang_checked::CheckedMutex<std::mutex> _mutex;
    size_t _numWorkers MONGO_LOCKING_GUARDED_BY(_mutex);
    ConsumeFn _consumeFn;
    std::deque<Worker> _workers;
    PeriodicJobAnchor _metricsCollector;
};

}  // namespace mongo::repl
