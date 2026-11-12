// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_oplog_applier.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier_batcher.h"
#include "mongo/db/repl/oplog_applier_dispatch.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/pipelined_applier_otel_metrics.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo::repl {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforePipelinedApplierPublish);
MONGO_FAIL_POINT_DEFINE(hangBeforePipelinedApplierDrain);
MONGO_FAIL_POINT_DEFINE(pausePipelinedApplierWorkerBeforeNextBatch);

/**
 * Blocks the calling worker while the 'pausePipelinedApplierWorkerBeforeNextBatch' fail point is
 * enabled. Called after the worker has signaled completion of its slice, it keeps the worker from
 * starting the next batch's slice, mirroring how 'pauseBatchApplicationBeforeCompletion' freezes
 * the whole classic applier. Fatals if the owning applier begins shutting down while blocked
 * ('applier' may be null in callers that cannot shut down, e.g. unit tests).
 */
void pauseWorkerBeforeNextBatch(const OplogApplier* applier, size_t workerIdx) {
    if (MONGO_unlikely(pausePipelinedApplierWorkerBeforeNextBatch.shouldFail())) {
        // Do not include "fail point enabled" in this message: tests that wait on
        // pauseBatchApplicationBeforeCompletion's log output grep for that text plus the fail
        // point name, and it must only match at the batch boundary in the advancer.
        LOGV2(13735800,
              "Holding pipelined oplog applier worker at "
              "pausePipelinedApplierWorkerBeforeNextBatch",
              "workerIdx"_attr = workerIdx);
        while (MONGO_unlikely(pausePipelinedApplierWorkerBeforeNextBatch.shouldFail())) {
            if (applier && applier->inShutdown()) {
                LOGV2_FATAL_NOTRACE(13735801,
                                    "Turn off pausePipelinedApplierWorkerBeforeNextBatch before "
                                    "attempting clean shutdown");
            }
            sleepmillis(100);
        }
    }
}

/**
 * Configures 'opCtx' for oplog application: no constraint enforcement, prepare conflicts ignored.
 */
void setWorkerOpCtxStates(OperationContext* opCtx) {
    opCtx->setEnforceConstraints(false);
    shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
        PrepareConflictBehavior::ignoreAndAllowWrites);
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource());
}

}  // namespace

/**
 * Returns if a batch containing the given ops must be applied serialized, i.e. with
 * no other batches in flight.
 */
bool batchMustBeAppliedSerialized(OplogApplication::Mode mode,
                                  Timestamp lastApplied,
                                  const std::vector<OplogEntry>& ops) {
    invariant(!ops.empty());
    const BatchStats batchStats;
    return std::any_of(ops.begin(), ops.end(), [&](const OplogEntry& entry) {
        if (mode == OplogApplication::Mode::kSecondary &&
            (entry.shouldPrepare() || entry.isPreparedCommitOrAbort())) {
            // A prepared-transaction boundary batch must apply without other batches in flight,
            // and its session update must be published before later batches dispatch.
            // TODO SERVER-138432: Pipeline eligible split prepared transaction batches.
            return true;
        }
        // Entries above lastApplied may belong to batches still in flight, so they may be applied
        // concurrently with this batch; entries below it have already been published.
        const auto action = getBatchActionForEntry(mode, lastApplied, entry, batchStats);
        return action == BatchAction::kProcessIndividually || action == BatchAction::kStartNewBatch;
    });
}

PipelinedOplogApplier::PipelinedOplogApplier(executor::TaskExecutor* executor,
                                             OplogBuffer* oplogBuffer,
                                             Observer* observer,
                                             ReplicationCoordinator* replCoord,
                                             StorageInterface* storageInterface,
                                             const Options& options,
                                             size_t numWorkers)
    : OplogApplier(executor, oplogBuffer, observer, options),
      _replCoord(replCoord),
      _storageInterface(storageInterface),
      _workerPool(numWorkers,
                  [this](size_t workerIdx, const PipelinedApplierWorkerPool::WorkItem& item) {
                      consumeWorkItem(workerIdx, getOptions(), item, _batchTracker, this);
                  }),
      _advancer(_batchTracker, [this](const auto& batch) { _publishBatch(batch); }) {
    _disableTransactionUpdateCoalescing =
        rss::ReplicatedStorageService::get(cc().getServiceContext())
            .getPersistenceProvider()
            .shouldDisableTransactionUpdateCoalescing();
}

PipelinedOplogApplier::~PipelinedOplogApplier() = default;

void PipelinedOplogApplier::_run(OplogBuffer* oplogBuffer) {
    // The batcher may stop producing work before the workers shut down, so worker shutdown should
    // be last.
    ON_BLOCK_EXIT([this] { _workerPool.shutdownAndJoin(); });

    _oplogBatcher->startup(_storageInterface);
    ON_BLOCK_EXIT([this] { _oplogBatcher->shutdown(); });

    _advancer.startup();
    ON_BLOCK_EXIT([this] {
        // Abandoned work permits teardown, but cannot satisfy a successful drain barrier.
        static_cast<void>(_drainWorkers());
        _advancer.shutdownAndJoin();
    });

    // Every batch must start after the last op dispatched before it. lastApplied sets the initial
    // bound: it trails dispatch, since batches still on the workers have not been published yet.
    OpTime lastDispatchedOpTime = _replCoord->getMyLastAppliedOpTime();

    // Set when the loop observes the tracker reporting an abandoned batch. The batcher thread
    // blocks until every batch it produced has been taken, so the remaining batches are discarded
    // without applying them until the batcher hands over its empty shutdown sentinel batch.
    bool dispatchStopped = false;

    while (true) {
        // Workers abandon the batch queue if they are interrupted by shutdown. Once any worker
        // signals abandonment in the batch tracker, the front batch never completes and no later
        // batch can publish, so the remaining batches are discarded instead of applied.
        if (!dispatchStopped && _batchTracker.hasAbandonedBatch()) {
            // Shutdown ordering never interrupts the applier's work, so this is not
            // expected to be reached.
            tassertNoThrow(
                13605302, "Pipelined oplog applier observed a shutdown-related abandonment", false);
            LOGV2_ERROR(13605300,
                        "Pipelined oplog applier stopped dispatching after shutdown-related "
                        "abandonment",
                        "lastAppliedOpTime"_attr = _replCoord->getMyLastAppliedOpTime());
            dispatchStopped = true;
        }

        // An operation context is allocated per batch so that catalog lookups use the current
        // snapshot.
        const auto opCtxPtr = cc().getServiceContext()->makeKillOpsExemptOperationContext(&cc());
        auto* opCtx = opCtxPtr.get();
        ScopedAdmissionPriority<ExecutionAdmissionContext> priority(
            opCtx, AdmissionContext::Priority::kExempt);

        auto batch = _oplogBatcher->getNextBatch(Seconds(1));
        if (batch.empty()) {
            if (batch.mustShutdown()) {
                return;
            }
            if (batch.termWhenExhausted() && !dispatchStopped) {
                // We need to wait for all in-flight work to finish before signaling that the
                // applier has drained. Once dispatch stopped, a later term-exhausted batch must
                // neither drain again nor signal drain completion.
                if (_drainWorkers()) {
                    _replCoord->signalApplierDrainComplete(opCtx, *batch.termWhenExhausted());
                }
            }
            continue;
        }

        // A batch produced after dispatch stopped is dropped unapplied. The oplog batcher's
        // contract requires that all queued ops are drained before the pipelined oplog applier can
        // see the sentinal value that signals shutdown.
        if (dispatchStopped) {
            continue;
        }

        const auto firstOpTimeInBatch = batch.front().getOpTime();
        const auto lastOpTimeInBatch = batch.back().getOpTime();
        if (firstOpTimeInBatch <= lastDispatchedOpTime) {
            LOGV2_FATAL(13322700,
                        "Pipelined oplog applier received a batch that does not follow the last "
                        "dispatched op",
                        "firstOpTimeInBatch"_attr = firstOpTimeInBatch,
                        "lastDispatchedOpTime"_attr = lastDispatchedOpTime,
                        "lastAppliedOpTime"_attr = _replCoord->getMyLastAppliedOpTime(),
                        "firstOperation"_attr = redact(batch.front().toBSONForLogging()));
        }

        const auto numOpsInBatch = batch.count();
        try {
            _dispatchBatch(opCtx, batch.releaseBatch());
            lastDispatchedOpTime = lastOpTimeInBatch;
        } catch (const DBException& ex) {
            if (ErrorCodes::isShutdownError(ex.code())) {
                LOGV2_ERROR(13605301,
                            "Pipelined oplog applier interrupted by shutdown while dispatching a "
                            "batch",
                            "error"_attr = redact(ex),
                            "firstOpTimeInBatch"_attr = firstOpTimeInBatch,
                            "numOperationsInBatch"_attr = numOpsInBatch,
                            "lastAppliedOpTime"_attr = _replCoord->getMyLastAppliedOpTime());
                dispatchStopped = true;
            } else {
                LOGV2_FATAL(13322701,
                            "Pipelined oplog applier failed to dispatch a batch",
                            "error"_attr = redact(ex),
                            "firstOpTimeInBatch"_attr = firstOpTimeInBatch,
                            "numOperationsInBatch"_attr = numOpsInBatch);
            }
        }
    }
}

StatusWith<OpTime> PipelinedOplogApplier::_applyOplogBatch(OperationContext* opCtx,
                                                           std::vector<OplogEntry> ops) {
    MONGO_UNIMPLEMENTED;
}

void PipelinedOplogApplier::_dispatchBatch(OperationContext* opCtx, std::vector<OplogEntry> ops) {
    invariant(!ops.empty());

    pauseBatchApplicationAfterWritingOplogEntriesAndBeforeApplication(opCtx);

    // A batch containing any entry that must apply alone — e.g. DDL, the new-primary noop, or a
    // truncate — is applied serialized. This is done through a "drain, dispatch, drain" sequence:
    // drain everything in flight, dispatch the batch, then drain and wait for it to be applied
    // and published before resuming.
    // If the batch doesn't require serialization, the batch is dispatched to the worker pool and
    // pipelined with other batches.
    // We pass in the current lastApplied opTime as a bound on the smallest opTime that can be run
    // by a worker while this batch is executing. Since this is the LOWER bound, it's alright and
    // expected if the lastApplied moves forward and the value passed in is stale.
    if (batchMustBeAppliedSerialized(
            getOptions().mode, _replCoord->getMyLastAppliedOpTime().getTimestamp(), ops)) {
        // A failed drain returns without dispatching the batch; the run loop observes the
        // abandonment in the batch tracker and stops dispatching.
        if (!_drainWorkers()) {
            return;
        }
        // Prepared commit and abort oplog entries resolve the split sessions their prepare entry
        // created. The splits are returned to the pool once every worker of the resolving batch has
        // finished, mirroring the classic applier's end-of-batch release, so a drained pipeline
        // implies all resolved splits are released.
        auto* splitSessManager = _replCoord->getSplitPrepareSessionManager();
        std::vector<std::pair<LogicalSessionId, TxnNumber>> splitReleases;
        if (getOptions().mode == OplogApplication::Mode::kSecondary) {
            for (const auto& op : ops) {
                if (op.isPreparedCommitOrAbort() &&
                    splitSessManager->isSessionSplit(*op.getSessionId(), *op.getTxnNumber())) {
                    splitReleases.emplace_back(*op.getSessionId(), *op.getTxnNumber());
                }
            }
        }
        _dispatchBatchToWorkers(opCtx, std::move(ops));
        if (!_drainWorkers()) {
            return;
        }
        for (auto& [lsid, txnNumber] : splitReleases) {
            splitSessManager->releaseSplitSessions(lsid, txnNumber);
        }
    } else {
        _dispatchBatchToWorkers(opCtx, std::move(ops));
    }
}

void PipelinedOplogApplier::_dispatchBatchToWorkers(OperationContext* opCtx,
                                                    std::vector<OplogEntry> ops) {
    using WorkItem = PipelinedApplierWorkerPool::WorkItem;
    const size_t numWorkers = _workerPool.numWorkers();
    invariant(!ops.empty());

    // Take the validation lock (MODE_IX) for the entire duration of this batch to prevent
    // validate from running while the batch is in-flight.
    auto validateLock = std::make_unique<collection_validation::BatchValidationLock>(
        cc().getServiceContext(), opCtx);

    // Capture the last op time to publish lastApplied after all workers finish.
    const OpTimeAndWallTime lastOpTime{ops.back().getOpTime(), ops.back().getWallClockTime()};

    // Holds pseudo-operations (expanded applyOps, config.transactions session updates, expanded
    // container ops, etc).
    std::vector<std::vector<OplogEntry>> derivedOps;
    // Note that writerVectors holds ApplierOperations, which simply has pointers to the OplogEntry
    // objects in ops and derivedOps. The ApplierOperations do not own the OplogEntry objects; the
    // inflight batch in the batch tracker owns them.
    std::vector<std::vector<ApplierOperation>> writerVectors(numWorkers);
    // Continuous internode checkpoint validation is only implemented for the classic applier, so
    // no validator is passed here.
    fillWriterVectors(getOptions(),
                      _disableTransactionUpdateCoalescing,
                      opCtx,
                      &ops,
                      &writerVectors,
                      &derivedOps,
                      /*checkpointValidator=*/nullptr);

    std::vector<WorkItem> items(numWorkers);
    uint32_t participatingWorkers = 0;
    int64_t opsDispatched = 0;
    // Tracks the total serialized byte size of the ops dispatched in this batch, used by the batch
    // tracker to apply byte-based backpressure.
    uint64_t batchBytes = 0;
    for (size_t workerIdx = 0; workerIdx < numWorkers; ++workerIdx) {
        if (writerVectors[workerIdx].empty())
            continue;
        ++participatingWorkers;
        opsDispatched += writerVectors[workerIdx].size();
        for (const auto& applierOp : writerVectors[workerIdx]) {
            batchBytes += applierOp->getRawObjSizeBytes();
        }
        items[workerIdx].ops = std::move(writerVectors[workerIdx]);
    }

    // Register the batch and move "ops" and "derivedOps" into the batch tracker. This is to
    // guarantee that the oplog entries in the batch are alive for the duration the workers apply
    // the batch. The original batch boundary is registered even when the dispatching routes no
    // operations to any worker, so lastApplied still reaches it. addBatch may also block here if
    // the tracker is applying backpressure due to the inflight-batches or inflight-bytes caps.
    auto multikeyMetadata = std::make_shared<std::vector<WorkerMultikeyPathInfo>>(numWorkers);
    auto remainingWorkers = _batchTracker.addBatch(lastOpTime,
                                                   participatingWorkers,
                                                   std::move(ops),
                                                   std::move(derivedOps),
                                                   multikeyMetadata,
                                                   batchBytes,
                                                   std::move(validateLock));
    if (!remainingWorkers) {
        // Batch tracker shut down while we were waiting for capacity.
        return;
    }
    PipelinedApplyOtelMetrics::addOpsDispatched(opsDispatched);
    for (size_t workerIdx = 0; workerIdx < numWorkers; ++workerIdx) {
        if (!items[workerIdx].ops.empty()) {
            items[workerIdx].batchRemainingWorkers = remainingWorkers;
            items[workerIdx].multikeyPathInfo = &(*multikeyMetadata)[workerIdx];
            _workerPool.enqueue(workerIdx, std::move(items[workerIdx]));
        }
    }
}

bool PipelinedOplogApplier::_drainWorkers() {
    hangBeforePipelinedApplierDrain.pauseWhileSet();
    LOGV2_DEBUG(13469900, 1, "Draining pipelined oplog applier workers");
    _workerPool.waitForIdle();
    // Publication may include deferred multikey writes after the batch is popped from the FIFO.
    // Wait for all publication side effects to finish.
    return _waitForAdvancerIdle();
}

bool PipelinedOplogApplier::_waitForAdvancerIdle() {
    return _batchTracker.waitUntilIdle();
}

void PipelinedOplogApplier::_publishBatch(
    const PipelinedApplierBatchTracker::InflightBatch& batch) {
    hangBeforePipelinedApplierPublish.pauseWhileSet();
    // The advancer reaches a batch only after all of its workers have applied their slices, so
    // pausing here holds the batch fully applied but unpublished, with lastApplied frozen: the
    // classic applier's fail point position. This is where the log line tests wait on is emitted.
    // The worker fail point is passed so the helper can require, via invariant, that tests
    // pausing here have also paused the workers.
    pauseBatchApplicationBeforeCompletionAndLastAppliedAdvancement(
        this, &pausePipelinedApplierWorkerBeforeNextBatch);

    invariant(batch.multikeyMetadata);
    invariant(!batch.ops.empty());

    // Use the advancer's Client so the dispatcher can create its next batch's operation context.
    const auto opCtx = cc().getServiceContext()->makeKillOpsExemptOperationContext(&cc());
    ScopedAdmissionPriority<ExecutionAdmissionContext> priority(
        opCtx.get(), AdmissionContext::Priority::kExempt);
    UnreplicatedWritesBlock noRep(opCtx.get());
    setWorkerOpCtxStates(opCtx.get());
    const Timestamp lastApplied = _replCoord->getMyLastAppliedOpTime().getTimestamp();
    for (const auto& infoVector : *batch.multikeyMetadata) {
        for (const auto& info : infoVector) {
            const Timestamp ts = info.earliestTimestamp.isNull() ? batch.ops.front().getTimestamp()
                                                                 : info.earliestTimestamp;
            invariant(ts > lastApplied,
                      "Deferred multikey writes must commit above the published timestamp");
        }
    }
    uassertStatusOK(flushMultikeyPathInfo(opCtx.get(),
                                          _storageInterface,
                                          *batch.multikeyMetadata,
                                          batch.ops.front().getTimestamp(),
                                          batch.lastOpTime.opTime.getTimestamp()));
    _replCoord->setMyLastAppliedOpTimeAndWallTimeForward(batch.lastOpTime);
    signalOplogWaiters();
}

Status applyWorkItem(OperationContext* opCtx,
                     const OplogApplier::Options& options,
                     const PipelinedApplierWorkerPool::WorkItem& item,
                     WorkerMultikeyPathInfo* multikeyPathInfo) {
    invariant(multikeyPathInfo);
    // Oplog application must not wait on execution tickets or flow control.
    ScopedAdmissionPriority<ExecutionAdmissionContext> skipTicketAcquisition(
        opCtx, AdmissionContext::Priority::kExempt);
    UnreplicatedWritesBlock uwb(opCtx);
    setWorkerOpCtxStates(opCtx);

    auto& tracker = MultikeyPathTracker::get(opCtx);
    invariant(multikeyPathInfo->empty());
    invariant(tracker.isEmpty());
    tracker.startTrackingMultikeyPathInfo();
    ON_BLOCK_EXIT([&] {
        tracker.stopTrackingMultikeyPathInfo();
        tracker.clear();
    });

    std::vector<ApplierOperation> ops = item.ops;
    // A DSC node always resumes oplog application from a consistent point: it recovers to the
    // stable checkpoint and replays from there, so there is never a window where its data is
    // inconsistent with the ops being applied. As a result, we hardcode isDataConsistent to true.
    const bool isDataConsistent = true;
    const auto status =
        OplogApplierUtils::applyOplogBatchCommon(opCtx,
                                                 ops,
                                                 options.mode,
                                                 options.allowNamespaceNotFoundErrorsOnCrudOps,
                                                 isDataConsistent,
                                                 &applyOplogEntryOrGroupedInserts);
    if (status.isOK()) {
        *multikeyPathInfo = tracker.getMultikeyPathInfo();
        PipelinedApplyOtelMetrics::addOpsAppliedByWorkers(item.ops.size());
    }
    return status;
}

void consumeWorkItem(size_t workerIdx,
                     const OplogApplier::Options& options,
                     const PipelinedApplierWorkerPool::WorkItem& item,
                     PipelinedApplierBatchTracker& batchTracker,
                     const OplogApplier* applier) {
    invariant(!item.ops.empty());
    invariant(item.batchRemainingWorkers);
    invariant(item.multikeyPathInfo);
    Status status = Status::OK();
    try {
        auto opCtx = cc().makeOperationContext();
        status = opCtx->runWithoutInterruptionExceptAtGlobalShutdown(
            [&] { return applyWorkItem(opCtx.get(), options, item, item.multikeyPathInfo); });
    } catch (const DBException& ex) {
        status = ex.toStatus();
    }
    if (MONGO_likely(status.isOK())) {
        batchTracker.onWorkerCompletion(item.batchRemainingWorkers);
        // Pause after signaling completion, so the batch still completes and the advancer reaches
        // it, but this worker cannot start the next batch's slice. This is the pipelined
        // counterpart of the classic applier's full stop under
        // pauseBatchApplicationBeforeCompletion; tests enable both fail points together via the
        // jstests helper configurePauseBatchApplicationBeforeCompletion().
        pauseWorkerBeforeNextBatch(applier, workerIdx);
        return;
    }
    // A global-shutdown interrupt abandons the item unapplied; lastApplied stays below this
    // batch and restart recovery re-applies it.
    if (ErrorCodes::isShutdownError(status.code())) {
        batchTracker.onWorkerAbandonment();
        LOGV2(13322101,
              "Pipelined oplog applier worker interrupted by shutdown",
              "workerIdx"_attr = workerIdx,
              "error"_attr = redact(status));
        return;
    }
    LOGV2_FATAL(13322100,
                "Pipelined oplog applier worker failed to apply a work item",
                "workerIdx"_attr = workerIdx,
                "numOperationsInBatch"_attr = item.ops.size(),
                "firstOperation"_attr = redact(item.ops.front()->toBSONForLogging()),
                "firstOperationOpTime"_attr = item.ops.front()->getOpTime(),
                "lastOperationOpTime"_attr = item.ops.back()->getOpTime(),
                "error"_attr = redact(status));
}

}  // namespace mongo::repl
