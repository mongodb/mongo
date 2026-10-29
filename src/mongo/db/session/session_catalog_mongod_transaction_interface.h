// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * This interface provides methods for the MongoDSessionCatalog implementation to access
 * multi-document transaction features, specifically functionality provided by the
 * TransactionParticipant class in the db/transaction library.
 */
class MongoDSessionCatalogTransactionInterface {
public:
    using ScanSessionsCallbackFn = SessionCatalog::ScanSessionsCallbackFn;
    using ScanSessionsReadOnlyCallbackFn = SessionCatalog::ScanSessionsReadOnlyCallbackFn;
    using KillSessionsPredicateFn = SessionCatalog::KillSessionsPredicateFn;

    virtual ~MongoDSessionCatalogTransactionInterface() = default;

    /**
     * Returns true if this session contains a prepared transaction.
     *
     * Accepts an observable session because this is called inside a session worker function.
     */
    virtual bool isTransactionPrepared(const ObservableSession& session) = 0;

    /**
     * Returns true if we have a transaction that is in-progress.
     *
     * See TransactionParticipant::TransactionState.
     */
    virtual bool isTransactionInProgress(OperationContext* opCtx) = 0;

    /**
     * Invalidates the TransactionParticipant on check-in for committed internal prepared
     * transactions recovered from a precise checkpoint, ensuring subsequent retries reload
     * operation history from storage.
     */
    virtual void invalidateTransactionOnCheckInIfNeeded(OperationContext* opCtx) = 0;

    /**
     * Gets a description of the current transaction state.
     */
    virtual std::string transactionStateDescriptor(OperationContext* opCtx) = 0;

    /**
     * Blocking method, which loads the transaction state from storage if it has been marked as
     * needing refresh.
     */
    virtual void refreshTransactionFromStorageIfNeeded(OperationContext* opCtx) = 0;

    /**
     * Same as above, but does not retrieve full transaction history and should be called
     * only when oplog reads are not possible.
     */
    virtual void refreshTransactionFromStorageIfNeededNoOplogEntryFetch(
        OperationContext* opCtx) = 0;

    /**
     * Used only by the secondary oplog application logic.
     * Similar to 'TransactionParticipant::beginOrContinue' without performing any checks for
     * whether the new txnNumber will start a transaction number in the past.
     */
    virtual void beginOrContinueTransactionUnconditionally(
        OperationContext* opCtx, TxnNumberAndRetryCounter txnNumberAndRetryCounter) = 0;

    /**
     * Aborts the transaction, releasing transaction resources.
     */
    virtual void abortTransaction(OperationContext* opCtx, const SessionTxnRecord& txnRecord) = 0;

    /**
     * Yield or reacquire locks for prepared transactions, used on replication state transition.
     */
    virtual void refreshLocksForPreparedTransaction(OperationContext* opCtx,
                                                    const OperationSessionInfo& sessionInfo) = 0;

    /**
     * Marks the session as requiring refresh. Used when the session state has been modified
     * externally, such as through a direct write to the transactions table.
     */
    virtual void invalidateSessionToKill(OperationContext* opCtx, const SessionToKill& session) = 0;

    /**
     * Returns a 'parentSessionWorkerFn' that can be passed to
     * SessionCatalog::scanSessionsForReap().
     *
     * Accepts an output parameter for the parent session's TxnNumber.
     */
    virtual ScanSessionsCallbackFn makeParentSessionWorkerFnForReap(
        TxnNumber* parentSessionActiveTxnNumber) = 0;

    /**
     * Returns a 'childSessionWorkerFn' that can be passed to SessionCatalog::scanSessionsForReap().
     *
     * Accepts a reference to the parent session's TxnNumber.
     */
    virtual ScanSessionsCallbackFn makeChildSessionWorkerFnForReap(
        const TxnNumber& parentSessionActiveTxnNumber) = 0;

    /**
     * Returns a predicate to determine which sessions should be killed on step-up.
     * Sessions where the predicate returns true will be killed.
     */
    virtual KillSessionsPredicateFn makeKillPredicateForStepUp() = 0;

    /**
     * Returns a read-only scan function for step-up that collects prepared transaction sessions
     * into 'sessionsToReacquireLocks'.
     */
    virtual ScanSessionsReadOnlyCallbackFn makeScanFnForStepUp(
        std::vector<OperationSessionInfo>* sessionsToReacquireLocks) = 0;

    /**
     * Returns a function that should be used to determine when a session can be eagerly reaped from
     * the SessionCatalog on a mongod.
     */
    virtual ScanSessionsCallbackFn makeSessionWorkerFnForEagerReap(
        TxnNumber clientTxnNumberStarted, SessionCatalog::Provenance provenance) = 0;
};

}  // namespace mongo
