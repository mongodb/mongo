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

#include "mongo/s/session_catalog_router.h"

#include "mongo/db/session/sessions_collection.h"
#include "mongo/s/transaction_router.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

int RouterSessionCatalog::reapSessionsOlderThan(OperationContext* opCtx,
                                                SessionsCollection& sessionsCollection,
                                                Date_t possiblyExpired) {
    const auto catalog = SessionCatalog::get(opCtx);

    // Find the possibly expired logical session ids in the in-memory catalog.
    LogicalSessionIdSet possiblyExpiredLogicalSessionIds;
    // Skip child transaction sessions since they correspond to the same logical session as their
    // parent transaction session so they have the same last check-out time as the parent's.
    catalog->scanParentSessions([&](const ObservableSession& session) {
        const auto sessionId = session.getSessionId();
        invariant(isParentSessionId(sessionId));
        if (session.getLastCheckout() < possiblyExpired) {
            possiblyExpiredLogicalSessionIds.insert(session.getSessionId());
        }
    });
    // From the possibly expired logical session ids, find the ones that have been removed from
    // from the config.system.sessions collection.
    auto expiredLogicalSessionIds =
        sessionsCollection.findRemovedSessions(opCtx, possiblyExpiredLogicalSessionIds);

    // For each removed logical session id, removes all of its transaction session ids that are no
    // longer in use from the in-memory catalog.
    int numReaped = 0;

    for (const auto& logicalSessionId : expiredLogicalSessionIds) {
        // Scan all the transaction sessions for this logical session at once so reaping can be done
        // atomically.
        int numTransactionSessions = 0;
        const auto transactionSessionIdsNotReaped = catalog->scanSessionsForReap(
            logicalSessionId,
            [&](ObservableSession& parentSession) {
                const auto txnRouter = TransactionRouter::get(parentSession);
                if (txnRouter.canBeReaped()) {
                    // Only reap this transaction session if every other transaction session for
                    // this logical session is also safe to be reaped.
                    parentSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
                }
                ++numTransactionSessions;
            },
            [&](ObservableSession& childSession) {
                const auto txnRouter = TransactionRouter::get(childSession);
                if (txnRouter.canBeReaped()) {
                    // Only reap this transaction session if every other transaction session for
                    // this logical session is also safe to be reaped.
                    childSession.markForReap(ObservableSession::ReapMode::kNonExclusive);
                }
                ++numTransactionSessions;
            });
        numReaped += numTransactionSessions - transactionSessionIdsNotReaped.size();
    }

    return numReaped;
}

RouterOperationContextSession::RouterOperationContextSession(OperationContext* opCtx)
    : _opCtx(opCtx), _operationContextSession(opCtx) {}

RouterOperationContextSession::~RouterOperationContextSession() {
    if (auto txnRouter = TransactionRouter::get(_opCtx)) {
        // Only stash if the session wasn't yielded. This should only happen at global shutdown.
        txnRouter.stash(_opCtx, TransactionRouter::StashReason::kDone);
    }
};

void RouterOperationContextSession::checkIn(OperationContext* opCtx,
                                            OperationContextSession::CheckInReason reason) {
    invariant(OperationContextSession::get(opCtx));

    TransactionRouter::get(opCtx).stash(opCtx,
                                        reason == OperationContextSession::CheckInReason::kYield
                                            ? TransactionRouter::StashReason::kYield
                                            : TransactionRouter::StashReason::kDone);
    OperationContextSession::checkIn(opCtx, reason);
}

void RouterOperationContextSession::checkOut(OperationContext* opCtx) {
    invariant(!OperationContextSession::get(opCtx));

    OperationContextSession::checkOut(opCtx);
    TransactionRouter::get(opCtx).unstash(opCtx);
}

}  // namespace mongo
