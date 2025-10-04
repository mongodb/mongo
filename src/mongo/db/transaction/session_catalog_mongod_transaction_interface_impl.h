/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod_transaction_interface.h"
#include "mongo/db/session/session_txn_record_gen.h"

#include <vector>

namespace mongo {

/**
 * Facade around the TransactionParticipant class in the db/transaction library.
 */
class MongoDSessionCatalogTransactionInterfaceImpl
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

    ScanSessionsCallbackFn makeParentSessionWorkerFnForReap(
        TxnNumber* parentSessionActiveTxnNumber) override;

    ScanSessionsCallbackFn makeChildSessionWorkerFnForReap(
        const TxnNumber& parentSessionActiveTxnNumber) override;

    ScanSessionsCallbackFn makeSessionWorkerFnForStepUp(
        std::vector<SessionCatalog::KillToken>* sessionKillTokens,
        std::vector<OperationSessionInfo>* sessionsToReacquireLocks) override;

    ScanSessionsCallbackFn makeSessionWorkerFnForEagerReap(
        TxnNumber clientTxnNumberStarted, SessionCatalog::Provenance provenance) override;
};

}  // namespace mongo
