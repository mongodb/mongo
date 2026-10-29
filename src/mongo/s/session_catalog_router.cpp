// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/s/session_catalog_router.h"

#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"

#include <absl/container/node_hash_set.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

int RouterSessionCatalog::reapSessionsOlderThan(OperationContext* opCtx,
                                                SessionsCollection& sessionsCollection,
                                                Date_t possiblyExpired) {
    const auto catalog = SessionCatalog::get(opCtx);

    // Find the possibly expired logical session ids in the in-memory catalog.
    // Skip child transaction sessions since they correspond to the same logical session as their
    // parent transaction session so they have the same last check-out time as the parent's.
    auto possiblyExpiredLogicalSessionIds = catalog->findExpiredParentSessions(possiblyExpired);
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
