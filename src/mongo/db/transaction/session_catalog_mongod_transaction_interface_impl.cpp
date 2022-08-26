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

#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"

#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/s/transaction_router.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction

namespace mongo {

void MongoDSessionCatalogTransactionInterfaceImpl::invalidateSessionToKill(
    OperationContext* opCtx, const SessionToKill& session) {
    auto participant = TransactionParticipant::get(session);
    participant.invalidate(opCtx);
}

MongoDSessionCatalogTransactionInterface::ScanSessionsCallbackFn
MongoDSessionCatalogTransactionInterfaceImpl::makeParentSessionWorkerFnForReap(
    TxnNumber* parentSessionActiveTxnNumber) {
    return [parentSessionActiveTxnNumber](ObservableSession& parentSession) {
        const auto transactionSessionId = parentSession.getSessionId();
        const auto txnParticipant = TransactionParticipant::get(parentSession);
        const auto txnRouter = TransactionRouter::get(parentSession);

        *parentSessionActiveTxnNumber =
            txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber();
        if (txnParticipant.canBeReaped() && txnRouter.canBeReaped()) {
            LOGV2_DEBUG(6753702,
                        5,
                        "Marking parent transaction session for reap",
                        "lsid"_attr = transactionSessionId);
            // This is an external session so it can be reaped if and only if all of its
            // internal sessions can be reaped.
            parentSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
        }
    };
}

MongoDSessionCatalogTransactionInterface::ScanSessionsCallbackFn
MongoDSessionCatalogTransactionInterfaceImpl::makeChildSessionWorkerFnForReap(
    const TxnNumber& parentSessionActiveTxnNumber) {
    return [&parentSessionActiveTxnNumber](ObservableSession& childSession) {
        const auto transactionSessionId = childSession.getSessionId();
        const auto txnParticipant = TransactionParticipant::get(childSession);
        const auto txnRouter = TransactionRouter::get(childSession);

        if (txnParticipant.canBeReaped() && txnRouter.canBeReaped()) {
            if (isInternalSessionForNonRetryableWrite(transactionSessionId)) {
                LOGV2_DEBUG(6753703,
                            5,
                            "Marking child transaction session for reap",
                            "lsid"_attr = transactionSessionId);
                // This is an internal session for a non-retryable write so it can be reaped
                // independently of the external session that write ran in.
                childSession.markForReap(ObservableSession::ReapMode::kExclusive);
            } else if (isInternalSessionForRetryableWrite(transactionSessionId)) {
                LOGV2_DEBUG(6753704,
                            5,
                            "Marking child transaction session for reap",
                            "lsid"_attr = transactionSessionId);
                // This is an internal session for a retryable write so it must be reaped
                // atomically with the external session and internal sessions for that
                // retryable write, unless the write is no longer active (i.e. there is
                // already a retryable write or transaction with a higher txnNumber).
                childSession.markForReap(*transactionSessionId.getTxnNumber() <
                                                 parentSessionActiveTxnNumber
                                             ? ObservableSession::ReapMode::kExclusive
                                             : ObservableSession::ReapMode::kNonExclusive);
            } else {
                MONGO_UNREACHABLE;
            }
        }
    };
}

}  // namespace mongo
