// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_oplog_applier.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

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
    ON_BLOCK_EXIT([this] { _workerPool.shutdownAndJoin(); });
    MONGO_UNIMPLEMENTED;
}

StatusWith<OpTime> PipelinedOplogApplier::_applyOplogBatch(OperationContext* opCtx,
                                                           std::vector<OplogEntry> ops) {
    MONGO_UNIMPLEMENTED;
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
