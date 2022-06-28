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

#include "mongo/s/transaction_router_resource_yielder.h"

#include "mongo/db/session_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/util/exit.h"

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
    if (isMongos()) {
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
