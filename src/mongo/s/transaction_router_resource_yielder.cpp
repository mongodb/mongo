// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/transaction_router_resource_yielder.h"

#include "mongo/db/session/session.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeUnyieldingTransactionRouter);
}

std::unique_ptr<TransactionRouterResourceYielder>
TransactionRouterResourceYielder::makeForLocalHandoff() {
    return std::make_unique<TransactionRouterResourceYielder>();
}

std::unique_ptr<TransactionRouterResourceYielder>
TransactionRouterResourceYielder::makeForRemoteCommand() {
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        // Mongos cannot target itself so it does not need to yield for remote commands.
        return nullptr;
    }
    return std::make_unique<TransactionRouterResourceYielder>();
}

void TransactionRouterResourceYielder::yield(OperationContext* opCtx) {
    Session* const session = OperationContextSession::get(opCtx);
    if (session) {
        LOGV2_DEBUG(6753700,
                    5,
                    "TransactionRouterResourceYielder yielding",
                    "lsid"_attr = opCtx->getLogicalSessionId(),
                    "txnNumber"_attr = opCtx->getTxnNumber());
        RouterOperationContextSession::checkIn(opCtx,
                                               OperationContextSession::CheckInReason::kYield);
    }
    _yielded = (session != nullptr);
}

void TransactionRouterResourceYielder::unyield(OperationContext* opCtx) {
    if (_yielded) {
        hangBeforeUnyieldingTransactionRouter.pauseWhileSet();
        LOGV2_DEBUG(6753701,
                    5,
                    "TransactionRouterResourceYielder unyielding",
                    "lsid"_attr = opCtx->getLogicalSessionId(),
                    "txnNumber"_attr = opCtx->getTxnNumber());

        // Code that uses the TransactionRouter assumes it will only run with it, so check back out
        // the session ignoring interruptions, except at global shutdown to prevent stalling
        // shutdown. Unyield should always run with no resources held, so there shouldn't be a risk
        // of deadlock.
        try {
            opCtx->runWithoutInterruptionExceptAtGlobalShutdown(
                [&] { RouterOperationContextSession::checkOut(opCtx); });
        } catch (const DBException&) {
            // This can throw at global shutdown, so calling code that catches errors may
            // unexpectedly run without a session checked out. This is assumed safe because the
            // process is shutting down and can't do any meaningful work. This invariant is to
            // safeguard that assumption.
            invariant(globalInShutdownDeprecated());
            throw;
        }
    }
}

}  // namespace mongo
