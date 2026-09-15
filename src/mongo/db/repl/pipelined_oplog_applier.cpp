// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_oplog_applier.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo::repl {
namespace {

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
                      consumeWorkItem(workerIdx, getOptions(), item);
                  }),
      _router(numWorkers) {}

PipelinedOplogApplier::~PipelinedOplogApplier() = default;

void PipelinedOplogApplier::_run(OplogBuffer* oplogBuffer) {
    // The batcher may stop producing work before the workers shut down, so worker shutdown should
    // be last.
    ON_BLOCK_EXIT([this] { _workerPool.shutdownAndJoin(); });

    _oplogBatcher->startup(_storageInterface);
    ON_BLOCK_EXIT([this] { _oplogBatcher->shutdown(); });

    // Every batch must start after the last op dispatched before it. lastApplied sets the initial
    // bound: it trails dispatch, since batches still on the workers have not been published yet.
    OpTime lastDispatchedOpTime = _replCoord->getMyLastAppliedOpTime();

    while (true) {
        // An operation context is allocated per batch so that catalog lookups made while routing
        // use the current snapshot. The collection properties cached by _router persist across
        // batches; the router clears and refetches them itself when it classifies an op as
        // requiring inline application, as the op may invalidate the cached properties.
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
            _dispatchOps(opCtx, batch.releaseBatch());
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

void PipelinedOplogApplier::_dispatchOps(OperationContext* opCtx, std::vector<OplogEntry> ops) {
    using OpClass = PipelinedOpRouter::OpClass;
    using WorkItem = PipelinedApplierWorkerPool::WorkItem;
    const size_t numWorkers = _workerPool.numWorkers();

    // Packed container ops write several keys in one entry; hashing the whole entry would not
    // agree with the hash of any single key, so a packed op and a later op on one of its keys
    // could land on different workers and be applied concurrently. Split them into single-key ops
    // first so each key is hashed and routed independently.
    OplogApplierUtils::expandBatchedContainerOps(ops);

    // The ops to dispatch, in oplog order, each paired with its worker. Inner ops extracted from an
    // applyOps entry are owned by 'derivedOps'; everything else points into 'ops'.
    std::vector<std::pair<size_t, OplogEntry*>> routedOps;
    routedOps.reserve(ops.size());
    std::vector<std::vector<OplogEntry>> derivedOps;
    std::vector<size_t> opsPerWorker(numWorkers, 0);
    auto route = [&](OplogEntry* op) {
        auto workerIdx = _router.selectWorker(opCtx, op);
        routedOps.emplace_back(workerIdx, op);
        ++opsPerWorker[workerIdx];
    };

    for (auto& op : ops) {
        switch (_router.classify(op)) {
            case OpClass::kPipelined:
                route(&op);
                break;
            case OpClass::kPipelinedApplyOps: {
                auto& innerOps = derivedOps.emplace_back(ApplyOps::extractOperations(op));
                OplogApplierUtils::expandBatchedContainerOps(innerOps);
                for (auto& innerOp : innerOps) {
                    route(&innerOp);
                }
                break;
            }
            case OpClass::kRequiresInline:
                MONGO_UNIMPLEMENTED;
        }
    }

    // Each worker slice of ops is sized before any op is moved into it, so it never reallocates
    // upon insertion.
    std::vector<WorkItem> items(numWorkers);
    for (size_t workerIdx = 0; workerIdx < numWorkers; ++workerIdx) {
        items[workerIdx].ops.reserve(opsPerWorker[workerIdx]);
    }
    for (auto& [workerIdx, op] : routedOps) {
        items[workerIdx].ops.push_back(std::move(*op));
    }

    for (size_t workerIdx = 0; workerIdx < numWorkers; ++workerIdx) {
        if (!items[workerIdx].ops.empty()) {
            _workerPool.enqueue(workerIdx, std::move(items[workerIdx]));
        }
    }
}

Status applyWorkItem(OperationContext* opCtx,
                     const OplogApplier::Options& options,
                     const PipelinedApplierWorkerPool::WorkItem& item) {
    // Oplog application must not wait on execution tickets or flow control.
    ScopedAdmissionPriority<ExecutionAdmissionContext> skipTicketAcquisition(
        opCtx, AdmissionContext::Priority::kExempt);
    UnreplicatedWritesBlock uwb(opCtx);
    setWorkerOpCtxStates(opCtx);

    std::vector<ApplierOperation> ops;
    ops.reserve(item.ops.size());
    for (const auto& op : item.ops) {
        ops.emplace_back(&op);
    }
    // A DSC node always resumes oplog application from a consistent point: it recovers to the
    // stable checkpoint and replays from there, so there is never a window where its data is
    // inconsistent with the ops being applied. As a result, we hardcode isDataConsistent to true.
    const bool isDataConsistent = true;
    return OplogApplierUtils::applyOplogBatchCommon(opCtx,
                                                    ops,
                                                    options.mode,
                                                    options.allowNamespaceNotFoundErrorsOnCrudOps,
                                                    isDataConsistent,
                                                    &applyOplogEntryOrGroupedInserts);
}

void consumeWorkItem(size_t workerIdx,
                     const OplogApplier::Options& options,
                     const PipelinedApplierWorkerPool::WorkItem& item) {
    invariant(!item.ops.empty());
    Status status = Status::OK();
    try {
        auto opCtx = cc().makeOperationContext();
        status = opCtx->runWithoutInterruptionExceptAtGlobalShutdown(
            [&] { return applyWorkItem(opCtx.get(), options, item); });
    } catch (const DBException& ex) {
        status = ex.toStatus();
    }
    if (MONGO_likely(status.isOK())) {
        return;
    }
    // A global-shutdown interrupt abandons the item unapplied; lastApplied stays below this
    // batch and restart recovery re-applies it.
    if (ErrorCodes::isShutdownError(status.code())) {
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
                "firstOperation"_attr = redact(item.ops.front().toBSONForLogging()),
                "firstOperationOpTime"_attr = item.ops.front().getOpTime(),
                "lastOperationOpTime"_attr = item.ops.back().getOpTime(),
                "error"_attr = redact(status));
}

}  // namespace mongo::repl
