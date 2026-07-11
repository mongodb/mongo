// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/sharding_api_d_params_gen.h"

namespace mongo {

namespace refresh_util {

namespace {
template <typename SignalT>
void waitForSignalToComplete(OperationContext* opCtx, const SignalT& future) {
    // Must not block while holding a lock
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    // If we are in a transaction, limit the time we can wait for the future. This is needed in
    // order to prevent distributed deadlocks in situations where a DDL operation holds locks in a
    // different shard that prevent the future from completing. Limiting the wait will ensure that
    // the transaction will eventually get aborted.
    if (opCtx->inMultiDocumentTransaction()) {
        opCtx->runWithDeadline(opCtx->fastClockSource().now() +
                                   Milliseconds(metadataRefreshInTransactionMaxWaitMS.load()),
                               ErrorCodes::ExceededTimeLimit,
                               [&] { future.get(opCtx); });
    } else {
        future.get(opCtx);
    }
}
}  // namespace

void waitForRefreshToComplete(OperationContext* opCtx, const SharedSemiFuture<void>& future) {
    waitForSignalToComplete(opCtx, future);
}

Status waitForCriticalSectionToComplete(OperationContext* opCtx,
                                        const CriticalSectionSignal& critSecSignal) noexcept {
    try {
        waitForSignalToComplete(opCtx, critSecSignal);
    } catch (const DBException& ex) {
        // This is a best-effort attempt to wait for the critical section to complete, so no
        // need to handle any exceptions
        return ex.toStatus();
    }
    return Status::OK();
}

}  // namespace refresh_util

}  // namespace mongo
