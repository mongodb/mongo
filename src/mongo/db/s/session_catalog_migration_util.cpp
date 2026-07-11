// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/session_catalog_migration_util.h"

#include "mongo/db/session/session_catalog_mongod.h"

namespace mongo {
namespace session_catalog_migration_util {

boost::optional<SharedSemiFuture<void>> runWithSessionCheckedOutIfStatementNotExecuted(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumber txnNumber,
    boost::optional<StmtId> stmtId,
    unique_function<void()> callable,
    bool ignoreIncompleteHistory) {
    {
        auto lk = std::lock_guard(*opCtx->getClient());
        opCtx->setLogicalSessionId(std::move(lsid));
        opCtx->setTxnNumber(txnNumber);
    }

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

    auto txnParticipant = TransactionParticipant::get(opCtx);

    try {
        txnParticipant.beginOrContinue(opCtx,
                                       {txnNumber},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);

    } catch (const ExceptionFor<ErrorCodes::TransactionTooOld>&) {
        // txnNumber < txnParticipant.o().activeTxnNumber
        return boost::none;
    } catch (const ExceptionFor<ErrorCodes::IncompleteTransactionHistory>&) {
        // txnNumber == txnParticipant.o().activeTxnNumber &&
        // !txnParticipant.transactionIsInRetryableWriteMode()
        //
        // If the transaction chain is incomplete because the oplog was truncated, just ignore the
        // incoming write and don't attempt to "patch up" the missing pieces.
        //
        // This situation could also happen if the client reused the txnNumber for distinct
        // operations (which is a violation of the protocol). The client would receive an error if
        // they attempted to retry the retryable write they had reused the txnNumber with so it is
        // safe to leave config.transactions as-is.
        return boost::none;
    } catch (const ExceptionFor<ErrorCodes::PreparedTransactionInProgress>&) {
        // txnParticipant.transactionIsPrepared()
        return txnParticipant.onExitPrepare();
    } catch (const ExceptionFor<ErrorCodes::RetryableTransactionInProgress>&) {
        // This is a retryable write that was executed using an internal transaction and there is
        // a retry in progress.
        return txnParticipant.onConflictingInternalTransactionCompletion(opCtx);
    }

    if (stmtId) {
        try {
            if (txnParticipant.checkStatementExecuted(opCtx, *stmtId)) {
                // Skip the incoming statement because it has already been logged locally.
                return boost::none;
            }
        } catch (const ExceptionFor<ErrorCodes::IncompleteTransactionHistory>&) {
            // This indicates that the stmtId is absent from the activeTxnCommittedStatments &
            // hasIncompleteHistory set to true.
            // For post-fetch timestamp writes (already executed on the donor but processed by the
            // ReshardingOplogSessionApplication component on the recipient), a noop including the
            // executed statement must be inserted by the callable to preserve retryability during
            // resharding.
            if (ignoreIncompleteHistory)
                return boost::none;
        }
    }

    callable();
    return boost::none;
}

}  // namespace session_catalog_migration_util
}  // namespace mongo
