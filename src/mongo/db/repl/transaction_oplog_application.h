/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"

namespace mongo {

/**
 * Apply `commitTransaction` oplog entry.
 */
Status applyCommitTransaction(OperationContext* opCtx,
                              const repl::OplogEntry& entry,
                              repl::OplogApplication::Mode mode);

/**
 * Apply `abortTransaction` oplog entry.
 */
Status applyAbortTransaction(OperationContext* opCtx,
                             const repl::OplogEntry& entry,
                             repl::OplogApplication::Mode mode);

/**
 * Follow an oplog chain and copy the operations to destination.  Operations will be copied in
 * forward oplog order (increasing optimes).
 */
repl::MultiApplier::Operations readTransactionOperationsFromOplogChain(
    OperationContext* opCtx,
    const repl::OplogEntry& entry,
    const std::vector<repl::OplogEntry*>& cachedOps);

/**
 * Apply `prepareTransaction` oplog entry.
 */
Status applyPrepareTransaction(OperationContext* opCtx,
                               const repl::OplogEntry& entry,
                               repl::OplogApplication::Mode mode);

/*
 * Reconstruct prepared transactions by iterating over the transactions table to see which
 * transactions should be in the prepared state, getting the corresponding oplog entry and applying
 * the operations. Called at the end of rollback, startup recovery and initial sync.
 */
void reconstructPreparedTransactions(OperationContext* opCtx, repl::OplogApplication::Mode mode);
}  // namespace mongo
