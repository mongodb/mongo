/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/sharding_api_d_params_gen.h"

namespace mongo {

namespace refresh_util {

void waitForRefreshToComplete(OperationContext* opCtx, SharedSemiFuture<void> future) {
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

Status waitForCriticalSectionToComplete(OperationContext* opCtx,
                                        SharedSemiFuture<void> critSecSignal) noexcept {
    try {
        waitForRefreshToComplete(opCtx, critSecSignal);
    } catch (const DBException& ex) {
        // This is a best-effort attempt to wait for the critical section to complete, so no
        // need to handle any exceptions
        return ex.toStatus();
    }
    return Status::OK();
}

}  // namespace refresh_util

}  // namespace mongo
