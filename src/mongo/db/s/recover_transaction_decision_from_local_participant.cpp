
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/recover_transaction_decision_from_local_participant.h"

#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"

namespace mongo {

void recoverDecisionFromLocalParticipantOrAbortLocalParticipant(OperationContext* opCtx) {
    ON_BLOCK_EXIT([opCtx] {
        // Ensure waiting for the user-supplied writeConcern of the decision.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    });

    MongoDOperationContextSession checkOutSession(opCtx);

    auto txnParticipant = TransactionParticipant::get(opCtx);

    try {
        if (txnParticipant->getActiveTxnNumber() < *opCtx->getTxnNumber()) {
            // TODO (SERVER-38133): Remove the if-block and just execute the else-block.
            // Until SERVER-38133, calling beginOrContinue with startTransaction=boost::none with a
            // higher txnNumber than the local participant's txnNumber will return NoSuchTransaction
            // *without* marking the transaction at the higher txnNumber as aborted. As a stop-gap,
            // if the request's txnNumber is higher than the local participant's txnNumber,
            // explicitly start a transaction at the higher txnNumber here by calling
            // beginOrContinue with startTransaction=true; the higher transaction will be aborted
            // below.
            // After SERVER-38133, the new transaction will be started *and* aborted by calling
            // beginOrContinue with startTransaction=boost::none.
            txnParticipant->beginOrContinue(
                *opCtx->getTxnNumber(), false /* autocommit */, true /* startTransaction */);
        } else {
            // If the local participant's transaction number is *equal* to this request's and
            // corresponds to a *retryable write*, throws NoSuchTransaction, else is a no-op.
            // If the local participant's transaction number is *higher* than this request's, throws
            // TransactionTooOld.
            txnParticipant->beginOrContinue(
                *opCtx->getTxnNumber(), false /* autocommit */, boost::none /* startTransaction */);
        }

        // The local participant's txnNumber matched the request's txnNumber, and the txnNumber
        // corresponds to a transaction (not a retryable write).

        if (txnParticipant->transactionIsCommitted()) {
            return;
        }

        txnParticipant->unstashTransactionResources(opCtx, "coordinateCommitTransaction");
    } catch (const DBException& e) {
        // Convert a PreparedTransactionInProgress error to an anonymous error code.
        uassert(51021,
                "coordinateCommitTransaction command found local participant is prepared but no "
                "active coordinator exists",
                e.code() != ErrorCodes::PreparedTransactionInProgress);
        throw;
    }

    // Abort the transaction. Since there was no active coordinator for this transaction on this
    // node, either the coordinator has already timed out waiting for the participant list and
    // the local participant's transaction will time out and abort anyway, or another node in
    // this replica set has stepped up and received all the transaction statements and may
    // commit. It's safe to abort the transaction even if the latter is the case, because this
    // command will fail waiting for majority writeConcern.
    txnParticipant->abortActiveTransaction(opCtx);
    uassert(ErrorCodes::NoSuchTransaction,
            "Transaction was aborted",
            txnParticipant->transactionIsCommitted());
}

}  // namespace mongo
