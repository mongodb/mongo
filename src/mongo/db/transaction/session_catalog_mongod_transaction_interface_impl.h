// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod_transaction_interface.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {

/**
 * Facade around the TransactionParticipant class in the db/transaction library.
 */
class [[MONGO_MOD_PUBLIC]] MongoDSessionCatalogTransactionInterfaceImpl
    : public MongoDSessionCatalogTransactionInterface {
    MongoDSessionCatalogTransactionInterfaceImpl(
        const MongoDSessionCatalogTransactionInterfaceImpl&) = delete;
    MongoDSessionCatalogTransactionInterfaceImpl& operator=(
        const MongoDSessionCatalogTransactionInterfaceImpl&) = delete;

public:
    MongoDSessionCatalogTransactionInterfaceImpl() = default;
    ~MongoDSessionCatalogTransactionInterfaceImpl() override = default;

    bool isTransactionPrepared(const ObservableSession& session) override;

    bool isTransactionInProgress(OperationContext* opCtx) override;

    std::string transactionStateDescriptor(OperationContext* opCtx) override;

    void refreshTransactionFromStorageIfNeeded(OperationContext* opCtx) override;

    void refreshTransactionFromStorageIfNeededNoOplogEntryFetch(OperationContext* opCtx) override;

    void beginOrContinueTransactionUnconditionally(
        OperationContext* opCtx, TxnNumberAndRetryCounter txnNumberAndRetryCounter) override;

    void abortTransaction(OperationContext* opCtx, const SessionTxnRecord& txnRecord) override;

    void refreshLocksForPreparedTransaction(OperationContext* opCtx,
                                            const OperationSessionInfo& sessionInfo) override;

    void invalidateSessionToKill(OperationContext* opCtx, const SessionToKill& session) override;

    void invalidateTransactionOnCheckInIfNeeded(OperationContext* opCtx) override;

    ScanSessionsCallbackFn makeParentSessionWorkerFnForReap(
        TxnNumber* parentSessionActiveTxnNumber) override;

    ScanSessionsCallbackFn makeChildSessionWorkerFnForReap(
        const TxnNumber& parentSessionActiveTxnNumber) override;

    KillSessionsPredicateFn makeKillPredicateForStepUp() override;

    ScanSessionsReadOnlyCallbackFn makeScanFnForStepUp(
        std::vector<OperationSessionInfo>* sessionsToReacquireLocks) override;

    ScanSessionsCallbackFn makeSessionWorkerFnForEagerReap(
        TxnNumber clientTxnNumberStarted, SessionCatalog::Provenance provenance) override;
};

}  // namespace mongo
