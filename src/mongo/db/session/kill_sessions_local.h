// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/util/modules.h"

/**
 * Mongod local kill session / transaction functionality library.
 */
namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Kills all cursors, ops, and transactions on mongod for sessions matching 'matcher'.
 */
SessionKiller::Result killSessionsLocal(OperationContext* opCtx,
                                        const SessionKiller::Matcher& matcher,
                                        SessionKiller::UniformRandomBitGenerator* urbg);

/**
 * Matches only sessions which have unprepared multi-statement transactions, kills the sessions and
 * aborts the transactions.
 */
void killSessionsAbortUnpreparedTransactions(OperationContext* opCtx,
                                             const SessionKiller::Matcher& matcher,
                                             ErrorCodes::Error reason = ErrorCodes::Interrupted,
                                             Date_t deadline = Date_t::max());

/**
 * Aborts unprepared transactions whose sessions are associated with any of the given lockerIds.
 * This can be used to kill transactions that hold locks conflicting with a pending lock request,
 * identified by querying the lock manager for conflicting lock holders.
 */
void killSessionsAbortUnpreparedTransactionsForLockerIds(OperationContext* opCtx,
                                                         const std::vector<LockerId>& lockerIds,
                                                         ErrorCodes::Error reason,
                                                         Date_t deadline = Date_t::max());

/**
 * Aborts any expired transactions.
 */
void killAllExpiredTransactions(OperationContext* opCtx,
                                Milliseconds timeout,
                                int64_t* numKills,
                                int64_t* numTimeOuts);


/**
 * Aborts the oldest transaction when under cache pressure. We filter out prepared, or internal
 * transactions.
 */
void killOldestTransaction(OperationContext* opCtx,
                           Milliseconds timeout,
                           int64_t* numKills,
                           int64_t* numSkips,
                           int64_t* numTimeOuts);

/**
 * Run during shutdown to kill all in-progress transactions, including those in prepare.
 */
void killSessionsLocalShutdownAllTransactions(OperationContext* opCtx);

/**
 * Run during rollback to abort all in-progress prepared transactions.
 */
void killSessionsAbortAllPreparedTransactions(OperationContext* opCtx);

/**
 * Yields locks of prepared transactions.
 */
void yieldLocksForPreparedTransactions(OperationContext* opCtx);

/**
 * Invalidates sessions that do not have prepared transactions, since txnNumbers for transactions
 * that were aborted in-memory may be reused on the new primary.
 */
void invalidateSessionsForStepdown(OperationContext* opCtx);

}  // namespace mongo
