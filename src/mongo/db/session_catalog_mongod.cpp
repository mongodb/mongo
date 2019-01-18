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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/db/session_catalog_mongod.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

struct SessionTasksExecutor {
    SessionTasksExecutor()
        : threadPool([] {
              ThreadPool::Options options;
              options.threadNamePrefix = "MongoDSessionCatalog";
              options.minThreads = 0;
              options.maxThreads = 1;
              return options;
          }()) {}

    ThreadPool threadPool;
};

const auto sessionTasksExecutor = ServiceContext::declareDecoration<SessionTasksExecutor>();
const ServiceContext::ConstructorActionRegisterer sessionTasksExecutorRegisterer{
    "SessionCatalogD",
    [](ServiceContext* service) { sessionTasksExecutor(service).threadPool.startup(); },
    [](ServiceContext* service) {
        auto& pool = sessionTasksExecutor(service).threadPool;
        pool.shutdown();
        pool.join();
    }};

auto getThreadPool(OperationContext* opCtx) {
    return &sessionTasksExecutor(opCtx->getServiceContext()).threadPool;
}

void killSessionTokensFunction(
    OperationContext* opCtx,
    std::shared_ptr<std::vector<SessionCatalog::KillToken>> sessionKillTokens) {
    if (sessionKillTokens->empty())
        return;

    uassertStatusOK(getThreadPool(opCtx)->schedule([
        service = opCtx->getServiceContext(),
        sessionKillTokens = std::move(sessionKillTokens)
    ]() mutable {
        auto uniqueClient = service->makeClient("Kill-Session");
        auto uniqueOpCtx = uniqueClient->makeOperationContext();
        const auto opCtx = uniqueOpCtx.get();
        const auto catalog = SessionCatalog::get(opCtx);

        for (auto& sessionKillToken : *sessionKillTokens) {
            auto session = catalog->checkOutSessionForKill(opCtx, std::move(sessionKillToken));

            TransactionParticipant::get(session).invalidate(opCtx);
        }
    }));
}

}  // namespace

void MongoDSessionCatalog::onStepUp(OperationContext* opCtx) {
    // Invalidate sessions that could have a retryable write on it, so that we can refresh from disk
    // in case the in-memory state was out of sync.
    const auto catalog = SessionCatalog::get(opCtx);
    // The use of shared_ptr here is in order to work around the limitation of stdx::function that
    // the functor must be copyable.
    auto sessionKillTokens = std::make_shared<std::vector<SessionCatalog::KillToken>>();

    // Scan all sessions and reacquire locks for prepared transactions.
    // There may be sessions that are checked out during this scan, but none of them
    // can be prepared transactions, since only oplog application can make transactions
    // prepared on secondaries and oplog application has been stopped at this moment.
    std::vector<LogicalSessionId> sessionIdToReacquireLocks;

    SessionKiller::Matcher matcher(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    catalog->scanSessions(matcher, [&](const ObservableSession& session) {
        const auto txnParticipant = TransactionParticipant::get(session);
        if (!txnParticipant.inMultiDocumentTransaction()) {
            sessionKillTokens->emplace_back(session.kill());
        }

        if (txnParticipant.transactionIsPrepared()) {
            sessionIdToReacquireLocks.emplace_back(session.getSessionId());
        }
    });
    killSessionTokensFunction(opCtx, sessionKillTokens);

    {
        // Create a new opCtx because we need an empty locker to refresh the locks.
        auto newClient = opCtx->getServiceContext()->makeClient("restore-prepared-txn");
        AlternativeClientRegion acr(newClient);
        for (const auto& sessionId : sessionIdToReacquireLocks) {
            auto newOpCtx = cc().makeOperationContext();
            newOpCtx->setLogicalSessionId(sessionId);
            MongoDOperationContextSession ocs(newOpCtx.get());
            auto txnParticipant = TransactionParticipant::get(newOpCtx.get());
            LOG(3) << "Restoring locks of prepared transaction. SessionId: " << sessionId.getId()
                   << " TxnNumber: " << txnParticipant.getActiveTxnNumber();
            txnParticipant.refreshLocksForPreparedTransaction(newOpCtx.get(), false);
        }
    }

    const size_t initialExtentSize = 0;
    const bool capped = false;
    const bool maxSize = 0;

    BSONObj result;

    DBDirectClient client(opCtx);

    if (client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                                initialExtentSize,
                                capped,
                                maxSize,
                                &result)) {
        return;
    }

    const auto status = getStatusFromCommandResult(result);

    if (status == ErrorCodes::NamespaceExists) {
        return;
    }

    uassertStatusOKWithContext(status,
                               str::stream()
                                   << "Failed to create the "
                                   << NamespaceString::kSessionTransactionsTableNamespace.ns()
                                   << " collection");
}

boost::optional<UUID> MongoDSessionCatalog::getTransactionTableUUID(OperationContext* opCtx) {
    AutoGetCollection autoColl(opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IS);

    const auto coll = autoColl.getCollection();
    if (!coll) {
        return boost::none;
    }

    return coll->uuid();
}

void MongoDSessionCatalog::invalidateSessions(OperationContext* opCtx,
                                              boost::optional<BSONObj> singleSessionDoc) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isReplSet = replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    if (isReplSet) {
        uassert(40528,
                str::stream() << "Direct writes against "
                              << NamespaceString::kSessionTransactionsTableNamespace.ns()
                              << " cannot be performed using a transaction or on a session.",
                !opCtx->getLogicalSessionId());
    }

    const auto catalog = SessionCatalog::get(opCtx);

    // The use of shared_ptr here is in order to work around the limitation of stdx::function that
    // the functor must be copyable.
    auto sessionKillTokens = std::make_shared<std::vector<SessionCatalog::KillToken>>();

    if (singleSessionDoc) {
        sessionKillTokens->emplace_back(catalog->killSession(LogicalSessionId::parse(
            IDLParserErrorContext("lsid"), singleSessionDoc->getField("_id").Obj())));
    } else {
        SessionKiller::Matcher matcher(
            KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
        catalog->scanSessions(matcher, [&sessionKillTokens](const ObservableSession& session) {
            sessionKillTokens->emplace_back(session.kill());
        });
    }

    killSessionTokensFunction(opCtx, sessionKillTokens);
}

MongoDOperationContextSession::MongoDOperationContextSession(OperationContext* opCtx)
    : _operationContextSession(opCtx) {
    invariant(!opCtx->getClient()->isInDirectClient());

    auto txnParticipant = TransactionParticipant::get(opCtx);
    txnParticipant.refreshFromStorageIfNeeded(opCtx);
}

MongoDOperationContextSession::~MongoDOperationContextSession() = default;

void MongoDOperationContextSession::checkIn(OperationContext* opCtx) {
    if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
        txnParticipant.stashTransactionResources(opCtx);
    }

    OperationContextSession::checkIn(opCtx);
}

void MongoDOperationContextSession::checkOut(OperationContext* opCtx, const std::string& cmdName) {
    OperationContextSession::checkOut(opCtx);

    if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
        txnParticipant.unstashTransactionResources(opCtx, cmdName);
    }
}

MongoDOperationContextSessionWithoutRefresh::MongoDOperationContextSessionWithoutRefresh(
    OperationContext* opCtx)
    : _operationContextSession(opCtx), _opCtx(opCtx) {
    invariant(!opCtx->getClient()->isInDirectClient());
    const auto clientTxnNumber = *opCtx->getTxnNumber();

    auto txnParticipant = TransactionParticipant::get(opCtx);
    txnParticipant.beginOrContinueTransactionUnconditionally(opCtx, clientTxnNumber);
}

MongoDOperationContextSessionWithoutRefresh::~MongoDOperationContextSessionWithoutRefresh() {
    const auto txnParticipant = TransactionParticipant::get(_opCtx);
    // A session on secondaries should never be checked back in with a TransactionParticipant that
    // isn't prepared, aborted, or committed.
    invariant(!txnParticipant.inMultiDocumentTransaction() ||
              txnParticipant.transactionIsPrepared());
}

}  // namespace mongo
