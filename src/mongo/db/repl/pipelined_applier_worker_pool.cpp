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
    size_t numWorkers, ConsumeFn consumeFn) {
    LOGV2(13315500, "Starting pipelined oplog applier workers", "numWorkers"_attr = numWorkers);
    std::deque<Worker> workers;
    workers.resize(numWorkers);
    for (size_t workerIdx = 0; workerIdx < numWorkers; ++workerIdx) {
        auto& worker = workers[workerIdx];
        SerialProcessor<WorkItem>::Options opts{
            .process = [workerIdx, &worker, consumeFn](WorkItem item) -> Status {
                consumeFn(workerIdx, item);
                worker.executed.fetchAndAddRelaxed(1);
                return Status::OK();
            },
            .init =
                [workerIdx] {
                    Client::initThread(fmt::format("PipelinedApplierWorker-{}", workerIdx),
                                       getGlobalServiceContext()->getService(),
                                       Client::noSession(),
                                       ClientOperationKillableByStepdown{false});
                    AuthorizationSession::get(cc())->grantInternalAuthorization();
                    return Status::OK();
                },
        };
        worker.processor = std::make_unique<SerialProcessor<WorkItem>>(std::move(opts));
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
    : _numWorkers(numWorkers),
      _workers(_startWorkers(numWorkers, std::move(consumeFn))),
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
    invariant(worker.processor);
    worker.processor->push(std::move(item));
}

void PipelinedApplierWorkerPool::waitForIdle() {
    for (auto& worker : _workers) {
        invariant(worker.processor);
        worker.processor->drain();
    }
}

void PipelinedApplierWorkerPool::shutdownAndJoin() {
    if (MONGO_unlikely(_isShutdown.swap(true))) {
        return;
    }
    LOGV2(
        13315501, "Shutting down pipelined oplog applier workers", "numWorkers"_attr = _numWorkers);
    // Wait for each worker thread to consume everything before signaling shutdown.
    // SerialProcessor::shutdown() forces the processor's worker to stop consuming, joins the
    // thread, and returns whatever was left in the worker's queue. Waiting for idle first ensures
    // every worker drains its queue, so no work items will be dropped, and
    // PipelinedApplierWorkerPool::_isShutdown ensures we don't enqueue any work items during this
    // draining process.
    waitForIdle();
    for (auto& worker : _workers) {
        auto dropped = worker.processor->shutdown();
        invariant(dropped.empty());
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
