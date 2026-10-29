// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_applier_worker_pool.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/pipelined_applier_otel_metrics.h"
#include "mongo/db/repl/pipelined_oplog_applier.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/periodic_runner.h"

#include <limits>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo::repl {

std::deque<PipelinedApplierWorkerPool::Worker> PipelinedApplierWorkerPool::_startWorkers(
    size_t numWorkers) {
    LOGV2(13315500, "Starting pipelined oplog applier workers", "numWorkers"_attr = numWorkers);
    std::deque<Worker> workers;
    for (size_t i = 0; i < numWorkers; ++i) {
        ThreadPool::Options options;
        options.poolName = fmt::format("PipelinedApplierWorker-{}", i);
        options.threadNamePrefix = fmt::format("PipelinedApplierWorker-{}-", i);
        options.minThreads = 1;
        options.maxThreads = 1;
        options.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName,
                               getGlobalServiceContext()->getService(),
                               Client::noSession(),
                               ClientOperationKillableByStepdown{false});
            AuthorizationSession::get(cc())->grantInternalAuthorization();
        };
        workers.emplace_back(std::move(options));
        workers.back().pool.startup();
    }
    return workers;
}

PeriodicJobAnchor PipelinedApplierWorkerPool::_makeMetricsCollector(
    const std::deque<Worker>& workers) {
    auto* runner = getGlobalServiceContext()->getPeriodicRunner();
    if (!runner) {
        return {};
    }
    return runner->makeJob({
        "PipelinedApplierWorkerPool queue depth metrics",
        // Capturing `workers` by reference is safe because `_metricsCollector` is declared after
        // `_workers` in the class, so the periodic job will be created after and destroyed before
        // the deque.
        [&workers](auto*) {
            if (workers.empty()) {
                return;
            }
            uint64_t minQueueDepth = std::numeric_limits<uint64_t>::max();
            uint64_t maxQueueDepth = 0;
            for (const auto& worker : workers) {
                // Load `executed` before `scheduled` so the calculated depth can never be negative.
                uint64_t executed = worker.executed.loadRelaxed();
                uint64_t scheduled = worker.scheduled.loadRelaxed();
                invariant(scheduled >= executed);
                uint64_t queueDepth = scheduled - executed;
                minQueueDepth = std::min(minQueueDepth, queueDepth);
                maxQueueDepth = std::max(maxQueueDepth, queueDepth);
            }
            // This would only occur if we didn't check any workers, in which case we should have
            // early-returned from this callback already.
            invariant(minQueueDepth != std::numeric_limits<uint64_t>::max());
            PipelinedApplyOtelMetrics::setQueueDepthMin(minQueueDepth);
            PipelinedApplyOtelMetrics::setQueueDepthMax(maxQueueDepth);
        },
        Seconds(1),
        true /*isKillableByStepdown*/
    });
}

PipelinedApplierWorkerPool::PipelinedApplierWorkerPool(size_t numWorkers, ConsumeFn consumeFn)
    : _consumeFn(std::move(consumeFn)),
      _numWorkers(numWorkers),
      _workers(_startWorkers(numWorkers)),
      _metricsCollector(_makeMetricsCollector(_workers)) {
    if (_metricsCollector.isValid()) {
        _metricsCollector.start();
    }
}

PipelinedApplierWorkerPool::~PipelinedApplierWorkerPool() {
    // Idempotent: drains and joins here only if the owner never shut the pool down explicitly.
    shutdownAndJoin();
}

void PipelinedApplierWorkerPool::enqueue(size_t workerIdx, WorkItem item) {
    iassert(ErrorCodes::ShutdownInProgress,
            "Pipelined oplog applier workers are shutting down",
            !_isShutdown.load());
    invariant(workerIdx < _numWorkers);
    PipelinedApplyOtelMetrics::addOpsQueued(item.ops.size());
    auto& worker = _workers[workerIdx];
    // Counted before scheduling so a report never shows more items executed than scheduled.
    worker.scheduled.fetchAndAddRelaxed(1);
    worker.pool.schedule([this, workerIdx, &worker, item = std::move(item)](Status status) {
        // A non-OK status means the item was scheduled after shutdown, which the shutdown
        // contract forbids.
        invariant(status);
        _consumeFn(workerIdx, item);
        worker.executed.fetchAndAddRelaxed(1);
    });
}

void PipelinedApplierWorkerPool::waitForIdle() {
    for (auto& worker : _workers) {
        worker.pool.waitForIdle();
    }
}

void PipelinedApplierWorkerPool::shutdownAndJoin() {
    if (MONGO_unlikely(_isShutdown.swap(true))) {
        return;
    }
    LOGV2(
        13315501, "Shutting down pipelined oplog applier workers", "numWorkers"_attr = _numWorkers);
    // Wait for each worker thread to consume everything before signaling shutdown. A ThreadPool's
    // worker stops consuming as soon as shutdown() is called, and join() drains any tasks still
    // pending on an extra cleanup thread, which can run concurrently with the worker's in-flight
    // task. Waiting for idle first keeps every work item's consumption on its owning worker
    // thread, preserving per-worker serialization through shutdown. _isShutdown rejects any
    // further enqueues, so the queues stay empty afterwards.
    waitForIdle();
    for (auto& worker : _workers) {
        worker.pool.shutdown();
    }
    for (auto& worker : _workers) {
        worker.pool.join();
    }
    LOGV2(13315502, "Pipelined oplog applier workers shut down");
}

void PipelinedApplierWorkerPool::report(BSONObjBuilder& bob) const {
    for (size_t i = 0; i < _numWorkers; ++i) {
        BSONObjBuilder sub(bob.subobjStart(fmt::format("worker{}", i)));
        sub.append("scheduled", static_cast<long long>(_workers[i].scheduled.loadRelaxed()));
        sub.append("executed", static_cast<long long>(_workers[i].executed.loadRelaxed()));
    }
}

}  // namespace mongo::repl
