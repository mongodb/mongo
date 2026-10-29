// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_insert_listener.h"

#include "mongo/db/curop.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::insert_listener {
namespace {
MONGO_FAIL_POINT_DEFINE(planExecutorHangWhileYieldedInWaitForInserts);
}

bool shouldListenForInserts(OperationContext* opCtx, CanonicalQuery* cq) {
    return cq && cq->getFindCommandRequest().getTailable() &&
        cq->getFindCommandRequest().getAwaitData() && awaitDataState(opCtx).shouldWaitForInserts &&
        opCtx->checkForInterruptNoAssert().isOK() &&
        awaitDataState(opCtx).waitForInsertsDeadline >
        opCtx->getServiceContext()->getPreciseClockSource()->now();
}

bool shouldWaitForInserts(OperationContext* opCtx,
                          CanonicalQuery* cq,
                          PlanYieldPolicy* yieldPolicy) {
    // If this is an awaitData-respecting operation and we have time left and we're not interrupted,
    // we should wait for inserts.
    if (shouldListenForInserts(opCtx, cq)) {
        // We expect awaitData cursors to be yielding.
        tassert(
            11321502,
            fmt::format("Cannot create notifier with non-yielding PlanYieldPolicy::YieldPolicy {}",
                        static_cast<int>(yieldPolicy->getPolicy())),
            yieldPolicy->canReleaseLocksDuringExecution());

        // For operations with a last committed opTime, we are fetching oplog entries and should not
        // wait if the replication coordinator's lastCommittedOpTime has progressed past the
        // client's lastCommittedOpTime. In that case, we will return early so that we can inform
        // the client of the new lastCommittedOpTime immediately.
        if (clientsLastKnownCommittedOpTime(opCtx)) {
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            return !replCoord->shouldUseEmptyOplogBatchToPropagateCommitPoint(
                clientsLastKnownCommittedOpTime(opCtx).value());
        }
        return true;
    }
    return false;
}

std::unique_ptr<Notifier> getCappedInsertNotifier(
    OperationContext* opCtx,
    const boost::optional<CollectionAcquisition>& collection,
    PlanYieldPolicy* yieldPolicy) {
    // We don't expect to need a capped insert notifier for non-yielding plans.
    tassert(11321503,
            fmt::format("Cannot create notifier with non-yielding PlanYieldPolicy::YieldPolicy {}",
                        static_cast<int>(yieldPolicy->getPolicy())),
            yieldPolicy->canReleaseLocksDuringExecution());

    // In case of the read concern majority, return a majority committed point notifier, otherwise,
    // a notifier associated with that capped collection
    //
    // We can only wait on the capped collection insert notifier if the collection is present,
    // otherwise we should retry immediately when we hit EOF.
    if (shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource() ==
        RecoveryUnit::kMajorityCommitted) {
        return std::make_unique<MajorityCommittedPointNotifier>();
    } else {
        tassert(11321504, "collection must not be null", collection && collection->exists());

        return std::make_unique<LocalCappedInsertNotifier>(
            collection->getCollectionPtr()->getRecordStore()->capped()->getInsertNotifier());
    }
}

void waitForInserts(OperationContext* opCtx,
                    PlanYieldPolicy* yieldPolicy,
                    std::unique_ptr<Notifier>& notifier) {
    // The notifier wait() method will not wait unless the version passed to it matches the
    // current version of the notifier.  Since the version passed to it is the current version
    // of the notifier at the time of the previous EOF, we require two EOFs in a row with no
    // notifier version change in order to wait.  This is sufficient to ensure we never wait
    // when data is available.
    notifier->prepareForWait(opCtx);
    auto yieldResult = yieldPolicy->yieldOrInterrupt(
        opCtx,
        [opCtx, &notifier] {
            const auto deadline = awaitDataState(opCtx).waitForInsertsDeadline;
            auto curOp = CurOp::get(opCtx);
            curOp->pauseTimer();
            ON_BLOCK_EXIT([curOp] { curOp->resumeTimer(); });
            notifier->waitUntil(opCtx, deadline);
            if (MONGO_unlikely(planExecutorHangWhileYieldedInWaitForInserts.shouldFail())) {
                LOGV2(4452903,
                      "PlanExecutor - planExecutorHangWhileYieldedInWaitForInserts fail point "
                      "enabled. "
                      "Blocking until fail point is disabled");
                planExecutorHangWhileYieldedInWaitForInserts.pauseWhileSet();
            }
        },
        RestoreContext::RestoreType::kYield);
    notifier->doneWaiting(opCtx);

    uassertStatusOK(yieldResult);
}
}  // namespace mongo::insert_listener
