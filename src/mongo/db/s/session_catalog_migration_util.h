/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/future.h"

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
    unique_function<void()> callable);

}  // namespace session_catalog_migration_util
}  // namespace mongo
