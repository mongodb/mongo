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

#include "mongo/db/transaction/transaction_participant_resource_yielder.h"

#include "mongo/base/status.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/s/transaction_participant_failed_unyield_exception.h"

namespace mongo {

std::unique_ptr<TransactionParticipantResourceYielder> TransactionParticipantResourceYielder::make(
    StringData cmdName) {
    return std::make_unique<TransactionParticipantResourceYielder>(cmdName);
}

void TransactionParticipantResourceYielder::yield(OperationContext* opCtx) {
    // We're about to block. Check back in the session so that it's available to other threads. Note
    // that we may block on a request to _ourselves_, meaning that we may have to wait for another
    // thread which will use the same session. This step is necessary to prevent deadlocks.

    Session* const session = OperationContextSession::get(opCtx);
    if (session) {
        if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
            txnParticipant.stashTransactionResources(opCtx);
        }

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->checkInUnscopedSession(
            opCtx, OperationContextSession::CheckInReason::kYield);
    }
    _yielded = (session != nullptr);
}

void TransactionParticipantResourceYielder::unyield(OperationContext* opCtx) {
    if (_yielded) {
        // This may block on a sub-operation on this node finishing. It's possible that while
        // blocked on the network layer, another shard could have responded, theoretically
        // unblocking this thread of execution. However, we must wait until the child operation on
        // this shard finishes so we can get the session back. This may limit the throughput of the
        // operation, but it's correct.
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        mongoDSessionCatalog->checkOutUnscopedSession(opCtx);

        if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
            // Re-check the session back in in case of failure during unstashing of the transaction
            // resources.
            ScopeGuard releaseOnError([&] {
                mongoDSessionCatalog->checkInUnscopedSession(
                    opCtx, OperationContextSession::CheckInReason::kYield);
            });

            txnParticipant.unstashTransactionResources(opCtx, _cmdName, true /* forUnyield */);
            releaseOnError.dismiss();
        }
    }
}

Status TransactionParticipantResourceYielder::unyieldNoThrow(OperationContext* opCtx) noexcept {
    try {
        unyield(opCtx);
    } catch (const DBException& e) {
        auto status = e.toStatus();
        return Status{TransactionParticipantFailedUnyieldInfo(status),
                      "Failed to unyield transaction participant resources"};
    }

    return Status::OK();
}

}  // namespace mongo
