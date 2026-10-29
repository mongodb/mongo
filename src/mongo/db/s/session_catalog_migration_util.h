// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace session_catalog_migration_util {

/**
 * Checks out the session with given session id and acts in one of the following ways depending on
 * the state of this shard's config.transactions table:
 *
 *   (a) If this shard already knows about a higher transaction than txnNumber,
 *       it skips calling the supplied lambda function and returns boost::none.
 *
 *   (b) If this shard already knows about the retryable write statement (txnNumber, *stmtId),
 *       it skips calling the supplied lambda function and returns boost::none.
 *
 *   (c) If this shard has a pending prepared transaction for a previous txnNumber or conflicting
 *       internal transaction for the incoming txnNumber, it skips calling the supplied
 *       lambda function and returns a future that becomes ready once the active prepared
 *       transaction on this shard commits or aborts. After waiting for the returned future to
 *       become ready, the caller should then invoke it with the same arguments a second time.
 *
 *   (d) Otherwise, it calls the lambda function and returns boost::none.
 */
boost::optional<SharedSemiFuture<void>> runWithSessionCheckedOutIfStatementNotExecuted(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumber txnNumber,
    boost::optional<StmtId> stmtId,
    unique_function<void()> callable,
    bool ignoreIncompleteHistory = true);

}  // namespace session_catalog_migration_util
}  // namespace mongo
