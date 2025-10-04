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

#include "mongo/db/s/session_catalog_migration_util.h"

#include "mongo/db/session/session_catalog_mongod.h"

namespace mongo {
namespace session_catalog_migration_util {

boost::optional<SharedSemiFuture<void>> runWithSessionCheckedOutIfStatementNotExecuted(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumber txnNumber,
    boost::optional<StmtId> stmtId,
    unique_function<void()> callable) {
    {
        auto lk = stdx::lock_guard(*opCtx->getClient());
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

        if (stmtId && txnParticipant.checkStatementExecuted(opCtx, *stmtId)) {
            // Skip the incoming statement because it has already been logged locally.
            return boost::none;
        }
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

    callable();
    return boost::none;
}

}  // namespace session_catalog_migration_util
}  // namespace mongo
