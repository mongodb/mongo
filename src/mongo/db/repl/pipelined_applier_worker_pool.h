// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/serial_processor.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/functional.h"
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
 * A fixed set of persistent worker threads, each consuming its own queue of work items. Work
 * items enqueued to the same worker index are consumed serially, in FIFO order, by a single
 * thread; distinct workers consume independently. Each worker is backed by a single-threaded
 * SerialProcessor. _startWorkers gives every worker thread an internally authorized Client that
 * stepdown cannot kill.
 *
 * The worker threads start at construction and run until shutdownAndJoin(), which consumes every
 * previously enqueued work item before joining the threads. Once a shutdown has been attempted,
 * new work is rejected.
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
        return _numWorkers;
    }

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
    const size_t _numWorkers;
    // The set of workers is fixed for the object's lifetime.
    std::deque<Worker> _workers;
    PeriodicJobAnchor _metricsCollector;
};

}  // namespace mongo::repl
