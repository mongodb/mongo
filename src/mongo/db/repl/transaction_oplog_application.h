// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/util/modules.h"

#include <utility>
#include <vector>

namespace mongo {

/**
 * Apply `commitTransaction` oplog entry.
 */
Status applyCommitTransaction(OperationContext* opCtx,
                              const repl::ApplierOperation& op,
                              repl::OplogApplication::Mode mode);

/**
 * Apply `abortTransaction` oplog entry.
 */
Status applyAbortTransaction(OperationContext* opCtx,
                             const repl::ApplierOperation& op,
                             repl::OplogApplication::Mode mode);

/**
 * Follow an oplog chain and copy the operations to destination.  Operations will be copied in
 * forward oplog order (increasing optimes).
 */
std::vector<repl::OplogEntry> readTransactionOperationsFromOplogChain(
    OperationContext* opCtx,
    const repl::OplogEntry& entry,
    const std::vector<repl::OplogEntry*>& cachedOps) noexcept;

/**
 * Like readTransactionOperationsFromOplogChain, but also returns a boolean representing whether at
 * least one of the transaction operations is a command.
 */
std::pair<std::vector<repl::OplogEntry>, bool>
readTransactionOperationsFromOplogChainAndCheckForCommands(
    OperationContext* opCtx,
    const repl::OplogEntry& lastEntryInTxn,
    const std::vector<repl::OplogEntry*>& cachedOps) noexcept;

std::pair<std::vector<repl::OplogEntry>, bool> _readTransactionOperationsFromOplogChain(
    OperationContext* opCtx,
    const repl::OplogEntry& lastEntryInTxn,
    const std::vector<repl::OplogEntry*>& cachedOps,
    bool checkForCommands) noexcept;

/**
 * Apply `prepareTransaction` oplog entry.
 */
Status applyPrepareTransaction(OperationContext* opCtx,
                               const repl::ApplierOperation& op,
                               repl::OplogApplication::Mode mode);

/*
 * Reconstruct prepared transactions by iterating over the transactions table to see which
 * transactions should be in the prepared state, getting the corresponding oplog entry and applying
 * the operations. Called at the end of rollback, startup recovery and initial sync.
 */
[[MONGO_MOD_OPEN]] void reconstructPreparedTransactions(OperationContext* opCtx,
                                                        repl::OplogApplication::Mode mode);

/**
 * Recovers prepared transactions from a precise checkpoint by iterating over the transactions table
 * to find prepared transactions, cross referencing them with the prepared transactions found in
 * the checkpoint, then recreating the in-memory state for the transaction.
 */
[[MONGO_MOD_OPEN]] void recoverPreparedTransactionsFromPreciseCheckpoint(OperationContext* opCtx);
}  // namespace mongo
