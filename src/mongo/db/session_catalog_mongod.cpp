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

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

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

/**
 * Non-blocking call, which schedules asynchronously the work to finish cleaning up the specified
 * set of kill tokens.
 */
void killSessionTokens(OperationContext* opCtx,
                       std::vector<SessionCatalog::KillToken> sessionKillTokens) {
    if (sessionKillTokens.empty())
        return;

    getThreadPool(opCtx)->schedule(
        [service = opCtx->getServiceContext(),
         sessionKillTokens = std::move(sessionKillTokens)](auto status) mutable {
            invariant(status);

            ThreadClient tc("Kill-Sessions", service);
            auto uniqueOpCtx = tc->makeOperationContext();
            const auto opCtx = uniqueOpCtx.get();
            const auto catalog = SessionCatalog::get(opCtx);

            for (auto& sessionKillToken : sessionKillTokens) {
                auto session = catalog->checkOutSessionForKill(opCtx, std::move(sessionKillToken));
                auto participant = TransactionParticipant::get(session);
                participant.invalidate(opCtx);
            }
        });
}

void disallowDirectWritesUnderSession(OperationContext* opCtx) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isReplSet = replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    if (isReplSet) {
        uassert(40528,
                str::stream() << "Direct writes against "
                              << NamespaceString::kSessionTransactionsTableNamespace
                              << " cannot be performed using a transaction or on a session.",
                !opCtx->getLogicalSessionId());
    }
}

const auto kIdProjection = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kSortById = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kLastWriteDateFieldName = SessionTxnRecord::kLastWriteDateFieldName;

/**
 * Removes the specified set of session ids from the persistent sessions collection and returns the
 * number of sessions actually removed.
 */
int removeSessionsTransactionRecords(OperationContext* opCtx,
                                     SessionsCollection& sessionsCollection,
                                     const LogicalSessionIdSet& sessionIdsToRemove) {
    if (sessionIdsToRemove.empty())
        return 0;

    // From the passed-in sessions, find the ones which are actually expired/removed
    auto expiredSessionIds =
        uassertStatusOK(sessionsCollection.findRemovedSessions(opCtx, sessionIdsToRemove));

    if (expiredSessionIds.empty())
        return 0;

    // Remove the session ids from the on-disk catalog
    write_ops::Delete deleteOp(NamespaceString::kSessionTransactionsTableNamespace);
    deleteOp.setWriteCommandBase([] {
        write_ops::WriteCommandBase base;
        base.setOrdered(false);
        return base;
    }());
    deleteOp.setDeletes([&] {
        std::vector<write_ops::DeleteOpEntry> entries;
        for (const auto& lsid : expiredSessionIds) {
            entries.emplace_back(BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON()),
                                 false /* multi = false */);
        }
        return entries;
    }());

    BSONObj result;

    DBDirectClient client(opCtx);
    client.runCommand(NamespaceString::kSessionTransactionsTableNamespace.db().toString(),
                      deleteOp.toBSON({}),
                      result);

    BatchedCommandResponse response;
    std::string errmsg;
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Failed to parse response " << result,
            response.parseBSON(result, &errmsg));
    uassertStatusOK(response.getTopLevelStatus());

    return response.getN();
}

void createTransactionTable(OperationContext* opCtx) {
    auto serviceCtx = opCtx->getServiceContext();
    CollectionOptions options;
    auto status =
        repl::StorageInterface::get(serviceCtx)
            ->createCollection(opCtx, NamespaceString::kSessionTransactionsTableNamespace, options);
    if (status == ErrorCodes::NamespaceExists) {
        return;
    }

    uassertStatusOKWithContext(
        status,
        str::stream() << "Failed to create the "
                      << NamespaceString::kSessionTransactionsTableNamespace.ns() << " collection");
}

void abortInProgressTransactions(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    Query query(BSON(SessionTxnRecord::kStateFieldName
                     << DurableTxnState_serializer(DurableTxnStateEnum::kInProgress)));
    auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace, query);
    if (cursor->more()) {
        LOG(3) << "Aborting in-progress transactions on stepup.";
    }
    while (cursor->more()) {
        auto txnRecord = SessionTxnRecord::parse(
            IDLParserErrorContext("abort-in-progress-transactions"), cursor->next());
        opCtx->setLogicalSessionId(txnRecord.getSessionId());
        opCtx->setTxnNumber(txnRecord.getTxnNum());
        MongoDOperationContextSessionWithoutRefresh ocs(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        LOG(3) << "Aborting transaction sessionId: " << txnRecord.getSessionId().toBSON()
               << " txnNumber " << txnRecord.getTxnNum();
        txnParticipant.abortTransactionForStepUp(opCtx);
    }
}
}  // namespace

void MongoDSessionCatalog::onStepUp(OperationContext* opCtx) {
    // Invalidate sessions that could have a retryable write on it, so that we can refresh from disk
    // in case the in-memory state was out of sync.
    const auto catalog = SessionCatalog::get(opCtx);

    std::vector<SessionCatalog::KillToken> sessionKillTokens;

    // Scan all sessions and reacquire locks for prepared transactions.
    // There may be sessions that are checked out during this scan, but none of them
    // can be prepared transactions, since only oplog application can make transactions
    // prepared on secondaries and oplog application has been stopped at this moment.
    std::vector<LogicalSessionId> sessionIdToReacquireLocks;

    SessionKiller::Matcher matcher(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    catalog->scanSessions(matcher, [&](const ObservableSession& session) {
        const auto txnParticipant = TransactionParticipant::get(session);
        if (!txnParticipant.transactionIsOpen()) {
            sessionKillTokens.emplace_back(session.kill());
        }

        if (txnParticipant.transactionIsPrepared()) {
            sessionIdToReacquireLocks.emplace_back(session.getSessionId());
        }
    });
    killSessionTokens(opCtx, std::move(sessionKillTokens));

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

    abortInProgressTransactions(opCtx);

    createTransactionTable(opCtx);
}

boost::optional<UUID> MongoDSessionCatalog::getTransactionTableUUID(OperationContext* opCtx) {
    AutoGetCollection autoColl(opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IS);

    const auto coll = autoColl.getCollection();
    if (!coll) {
        return boost::none;
    }

    return coll->uuid();
}

void MongoDSessionCatalog::observeDirectWriteToConfigTransactions(OperationContext* opCtx,
                                                                  BSONObj singleSessionDoc) {
    disallowDirectWritesUnderSession(opCtx);

    class KillSessionTokenOnCommit : public RecoveryUnit::Change {
    public:
        KillSessionTokenOnCommit(OperationContext* opCtx,
                                 SessionCatalog::KillToken sessionKillToken)
            : _opCtx(opCtx), _sessionKillToken(std::move(sessionKillToken)) {}

        void commit(boost::optional<Timestamp>) override {
            rollback();
        }

        void rollback() override {
            std::vector<SessionCatalog::KillToken> sessionKillTokenVec;
            sessionKillTokenVec.emplace_back(std::move(_sessionKillToken));
            killSessionTokens(_opCtx, std::move(sessionKillTokenVec));
        }

    private:
        OperationContext* _opCtx;
        SessionCatalog::KillToken _sessionKillToken;
    };

    const auto catalog = SessionCatalog::get(opCtx);

    const auto lsid =
        LogicalSessionId::parse(IDLParserErrorContext("lsid"), singleSessionDoc["_id"].Obj());
    catalog->scanSession(lsid, [&](const ObservableSession& session) {
        const auto participant = TransactionParticipant::get(session);
        uassert(ErrorCodes::PreparedTransactionInProgress,
                str::stream() << "Cannot modify the entry for session "
                              << session.getSessionId().getId()
                              << " because it is in the prepared state",
                !participant.transactionIsPrepared());

        opCtx->recoveryUnit()->registerChange(new KillSessionTokenOnCommit(opCtx, session.kill()));
    });
}

void MongoDSessionCatalog::invalidateAllSessions(OperationContext* opCtx) {
    disallowDirectWritesUnderSession(opCtx);

    const auto catalog = SessionCatalog::get(opCtx);

    std::vector<SessionCatalog::KillToken> sessionKillTokens;

    SessionKiller::Matcher matcher(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    catalog->scanSessions(matcher, [&sessionKillTokens](const ObservableSession& session) {
        sessionKillTokens.emplace_back(session.kill());
    });

    killSessionTokens(opCtx, std::move(sessionKillTokens));
}

int MongoDSessionCatalog::reapSessionsOlderThan(OperationContext* opCtx,
                                                SessionsCollection& sessionsCollection,
                                                Date_t possiblyExpired) {
    {
        const auto catalog = SessionCatalog::get(opCtx);

        // Capture the possbily expired in-memory session ids
        LogicalSessionIdSet lsids;
        catalog->scanSessions(SessionKiller::Matcher(
                                  KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)}),
                              [&](const ObservableSession& session) {
                                  if (session.getLastCheckout() < possiblyExpired) {
                                      lsids.insert(session.getSessionId());
                                  }
                              });

        // From the passed-in sessions, find the ones which are actually expired/removed
        auto expiredSessionIds =
            uassertStatusOK(sessionsCollection.findRemovedSessions(opCtx, lsids));

        // Remove the session ids from the in-memory catalog
        for (const auto& lsid : expiredSessionIds) {
            catalog->scanSession(lsid, [](ObservableSession& session) {
                const auto participant = TransactionParticipant::get(session);
                if (!participant.transactionIsOpen()) {
                    session.markForReap();
                }
            });
        }
    }

    // The "unsafe" check for primary below is a best-effort attempt to ensure that the on-disk
    // state reaping code doesn't run if the node is secondary and cause log spam. It is a work
    // around the fact that the logical sessions cache is not registered to listen for replication
    // state changes.
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, NamespaceString::kConfigDb))
        return 0;

    // Scan for records older than the minimum lifetime and uses a sort to walk the '_id' index
    DBDirectClient client(opCtx);
    auto cursor =
        client.query(NamespaceString::kSessionTransactionsTableNamespace,
                     Query(BSON(kLastWriteDateFieldName << LT << possiblyExpired)).sort(kSortById),
                     0,
                     0,
                     &kIdProjection);

    // The max batch size is chosen so that a single batch won't exceed the 16MB BSON object size
    // limit
    const int kMaxBatchSize = 10'000;

    LogicalSessionIdSet lsids;
    int numReaped = 0;
    while (cursor->more()) {
        auto transactionSession = SessionsCollectionFetchResultIndividualResult::parse(
            "TransactionSession"_sd, cursor->next());

        lsids.insert(transactionSession.get_id());
        if (lsids.size() > kMaxBatchSize) {
            numReaped += removeSessionsTransactionRecords(opCtx, sessionsCollection, lsids);
            lsids.clear();
        }
    }

    numReaped += removeSessionsTransactionRecords(opCtx, sessionsCollection, lsids);

    return numReaped;
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
    invariant(!txnParticipant.transactionIsInProgress());
}

}  // namespace mongo
