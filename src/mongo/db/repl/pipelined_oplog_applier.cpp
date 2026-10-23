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
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo::repl {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforePipelinedApplierPublish);
MONGO_FAIL_POINT_DEFINE(hangBeforePipelinedApplierDrain);

/**
 * Configures 'opCtx' for oplog application: no constraint enforcement, prepare conflicts ignored.
 */
void setWorkerOpCtxStates(OperationContext* opCtx) {
    opCtx->setEnforceConstraints(false);
    shard_role_details::getRecoveryUnit(opCtx)->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource());
}

}  // namespace

/**
 * Returns if a batch containing the given ops must be applied serialized, i.e. with
 * no other batches in flight.
 */
bool batchMustBeAppliedSerialized(OplogApplication::Mode mode, const std::vector<OplogEntry>& ops) {
    invariant(!ops.empty());
    const BatchStats batchStats;
    const auto& firstEntryInBatch = ops.front();
    return std::any_of(ops.begin(), ops.end(), [&](const OplogEntry& entry) {
        const auto action = getBatchActionForEntry(mode, firstEntryInBatch, entry, batchStats);
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
                      consumeWorkItem(workerIdx, getOptions(), item, _batchTracker);
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

    while (true) {
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
            if (batch.termWhenExhausted()) {
                MONGO_UNIMPLEMENTED;
            }
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
        } catch (const DBException& ex) {
            LOGV2_FATAL(13322701,
                        "Pipelined oplog applier failed to dispatch a batch",
                        "error"_attr = redact(ex),
                        "firstOpTimeInBatch"_attr = firstOpTimeInBatch,
                        "numOperationsInBatch"_attr = numOpsInBatch);
        }
        lastDispatchedOpTime = lastOpTimeInBatch;
    }
}

StatusWith<OpTime> PipelinedOplogApplier::_applyOplogBatch(OperationContext* opCtx,
                                                           std::vector<OplogEntry> ops) {
    MONGO_UNIMPLEMENTED;
}

void PipelinedOplogApplier::_dispatchBatch(OperationContext* opCtx, std::vector<OplogEntry> ops) {
    invariant(!ops.empty());

    // A batch containing any entry that must apply alone — e.g. DDL, the new-primary noop, or a
    // truncate — is applied serialized. This is done through a "drain, dispatch, drain" sequence:
    // drain everything in flight, dispatch the batch, then drain and wait for it to be applied
    // and published before resuming.
    // If the batch doesn't require serialization, the batch is dispatched to the worker pool and
    // pipelined with other batches.
    if (batchMustBeAppliedSerialized(getOptions().mode, ops)) {
        if (!_drainWorkers()) {
            // TODO(SERVER-136053): Propagate shutdown failure to `_run`
            return;
        }
        _dispatchBatchToWorkers(opCtx, std::move(ops));
        if (!_drainWorkers()) {
            // TODO(SERVER-136053): Propagate shutdown failure to `_run`
            return;
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
    for (size_t workerIdx = 0; workerIdx < numWorkers; ++workerIdx) {
        if (writerVectors[workerIdx].empty())
            continue;
        ++participatingWorkers;
        opsDispatched += writerVectors[workerIdx].size();
        items[workerIdx].ops = std::move(writerVectors[workerIdx]);
    }

    // Register the batch and move "ops" and "derivedOps" into the batch tracker. This is to
    // guarantee that the oplog entries in the batch are alive for the duration the workers apply
    // the batch. The original batch boundary is registered even when the dispatching routes no
    // operations to any worker, so lastApplied still reaches it.
    auto multikeyMetadata = std::make_shared<std::vector<WorkerMultikeyPathInfo>>(numWorkers);
    auto remainingWorkers = _batchTracker.addBatch(
        lastOpTime, participatingWorkers, std::move(ops), std::move(derivedOps), multikeyMetadata);
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
    _replCoord->setMyLastAppliedOpTimeAndWallTimeForward(batch.lastOpTime);
    signalOplogWaiters();
}

Status applyWorkItem(OperationContext* opCtx,
                     const OplogApplier::Options& options,
                     const PipelinedApplierWorkerPool::WorkItem& item,
                     WorkerMultikeyPathInfo* multikeyPathInfo) {
    // Oplog application must not wait on execution tickets or flow control.
    ScopedAdmissionPriority<ExecutionAdmissionContext> skipTicketAcquisition(
        opCtx, AdmissionContext::Priority::kExempt);
    UnreplicatedWritesBlock uwb(opCtx);
    setWorkerOpCtxStates(opCtx);

    auto& tracker = MultikeyPathTracker::get(opCtx);
    if (multikeyPathInfo) {
        invariant(multikeyPathInfo->empty());
        invariant(tracker.isEmpty());
        tracker.startTrackingMultikeyPathInfo();
    }
    ON_BLOCK_EXIT([&] {
        if (multikeyPathInfo) {
            tracker.stopTrackingMultikeyPathInfo();
            tracker.clear();
        }
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
        if (multikeyPathInfo) {
            *multikeyPathInfo = tracker.getMultikeyPathInfo();
        }
        PipelinedApplyOtelMetrics::addOpsAppliedByWorkers(item.ops.size());
    }
    return status;
}

void consumeWorkItem(size_t workerIdx,
                     const OplogApplier::Options& options,
                     const PipelinedApplierWorkerPool::WorkItem& item,
                     PipelinedApplierBatchTracker& batchTracker,
                     bool deferMultikeyWrites) {
    invariant(!item.ops.empty());
    invariant(item.batchRemainingWorkers);
    invariant(item.multikeyPathInfo);
    Status status = Status::OK();
    try {
        auto opCtx = cc().makeOperationContext();
        status = opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
            // TODO(SERVER-135372): Remove deferMultikeyWrites and always defer once the advancer
            // flushes multikey writes before publishing lastApplied.
            return applyWorkItem(
                opCtx.get(), options, item, deferMultikeyWrites ? item.multikeyPathInfo : nullptr);
        });
    } catch (const DBException& ex) {
        status = ex.toStatus();
    }
    if (MONGO_likely(status.isOK())) {
        batchTracker.onWorkerCompletion(item.batchRemainingWorkers);
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
