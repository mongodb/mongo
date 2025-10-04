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


#include "mongo/db/session/session_catalog_mongod.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/index_builds_manager.h"
#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/ddl/create_indexes_gen.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/internal_transactions_reap_service.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangDuringStepUpPrepareRestoreLocks);
MONGO_FAIL_POINT_DEFINE(hangDuringStepUpAbortInProgressTransactions);

const auto getMongoDSessionCatalog =
    ServiceContext::declareDecoration<std::unique_ptr<MongoDSessionCatalog>>();

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
                       MongoDSessionCatalogTransactionInterface* ti,
                       std::vector<SessionCatalog::KillToken> sessionKillTokens) {
    if (sessionKillTokens.empty())
        return;

    getThreadPool(opCtx)->schedule(
        [service = opCtx->getServiceContext(),
         ti,
         sessionKillTokens = std::move(sessionKillTokens)](auto status) mutable {
            invariant(status);

            // TODO(SERVER-111754): Please revisit if this thread could be made killable.
            ThreadClient tc("Kill-Sessions",
                            service->getService(ClusterRole::ShardServer),
                            ClientOperationKillableByStepdown{false});
            auto uniqueOpCtx = tc->makeOperationContext();
            const auto opCtx = uniqueOpCtx.get();
            const auto catalog = SessionCatalog::get(opCtx);

            for (auto& sessionKillToken : sessionKillTokens) {
                auto session = catalog->checkOutSessionForKill(opCtx, std::move(sessionKillToken));
                ti->invalidateSessionToKill(opCtx, session);
            }
        });
}

void disallowDirectWritesUnderSession(OperationContext* opCtx) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isReplSet = replCoord->getSettings().isReplSet();
    if (isReplSet) {
        uassert(40528,
                str::stream()
                    << "Direct writes against "
                    << NamespaceString::kSessionTransactionsTableNamespace.toStringForErrorMsg()
                    << " cannot be performed using a transaction or on a session.",
                !opCtx->getLogicalSessionId());
    }
}

/**
 * Removes the transaction sessions that are expired and not in use from the in-memory catalog
 * (i.e. SessionCatalog). Returns the session ids for the expired transaction sessions that were
 * not removed because they were in use.
 */
LogicalSessionIdSet removeExpiredTransactionSessionsNotInUseFromMemory(
    OperationContext* opCtx,
    MongoDSessionCatalogTransactionInterface* ti,
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
            possiblyExpiredLogicalSessionIds.insert(sessionId);
        }
    });
    // From the possibly expired logical session ids, find the ones that have been removed from
    // from the config.system.sessions collection.
    LogicalSessionIdSet expiredLogicalSessionIds =
        sessionsCollection.findRemovedSessions(opCtx, possiblyExpiredLogicalSessionIds);

    // For each removed logical session id, removes all of its transaction session ids that are no
    // longer in use from the in-memory catalog.
    LogicalSessionIdSet expiredTransactionSessionIdsStillInUse;
    for (const auto& expiredLogicalSessionId : expiredLogicalSessionIds) {
        invariant(isParentSessionId(expiredLogicalSessionId));

        // Scan all the transaction sessions for this logical session at once so reaping can be done
        // atomically.
        TxnNumber parentSessionActiveTxnNumber;
        const auto transactionSessionIdsNotReaped = catalog->scanSessionsForReap(
            expiredLogicalSessionId,
            ti->makeParentSessionWorkerFnForReap(&parentSessionActiveTxnNumber),
            ti->makeChildSessionWorkerFnForReap(parentSessionActiveTxnNumber));
        expiredTransactionSessionIdsStillInUse.insert(transactionSessionIdsNotReaped.begin(),
                                                      transactionSessionIdsNotReaped.end());
    }

    LOGV2_DEBUG(6753705,
                5,
                "Expired sessions not reaped from the SessionCatalog",
                "lsids"_attr = expiredTransactionSessionIdsStillInUse);

    return expiredTransactionSessionIdsStillInUse;
}

const auto kIdProjection = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kSortById = BSON(SessionTxnRecord::kSessionIdFieldName << 1);
const auto kLastWriteDateFieldName = SessionTxnRecord::kLastWriteDateFieldName;

template <typename SessionContainer>
int removeSessionsTransactionRecordsFromDisk(OperationContext* opCtx,
                                             const SessionContainer& transactionSessionIdsToReap) {
    if (transactionSessionIdsToReap.empty()) {
        return 0;
    }

    // Remove the config.image_collection entries for the expired transaction session ids. We first
    // delete any images belonging to sessions about to be reaped, followed by the sessions. This
    // way if there's a failure, we'll only be left with sessions that have a dangling reference
    // to an image. Session reaping will rediscover the sessions to delete and try again.
    //
    // We opt for this rather than performing the two sets of deletes in a single transaction simply
    // to reduce code complexity.
    DBDirectClient client(opCtx);
    write_ops::checkWriteErrors(client.remove([&] {
        write_ops::DeleteCommandRequest imageDeleteOp(NamespaceString::kConfigImagesNamespace);
        imageDeleteOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase base;
            base.setOrdered(false);
            return base;
        }());
        imageDeleteOp.setDeletes([&] {
            std::vector<write_ops::DeleteOpEntry> entries;
            for (const auto& transactionSessionId : transactionSessionIdsToReap) {
                entries.emplace_back(
                    BSON(LogicalSessionRecord::kIdFieldName << transactionSessionId.toBSON()),
                    false /* multi = false */);
            }
            return entries;
        }());
        return imageDeleteOp;
    }()));

    // Remove the config.transaction entries for the expired transaction session ids.
    auto sessionDeleteReply = write_ops::checkWriteErrors(client.remove([&] {
        write_ops::DeleteCommandRequest sessionDeleteOp(
            NamespaceString::kSessionTransactionsTableNamespace);
        sessionDeleteOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase base;
            base.setOrdered(false);
            return base;
        }());
        sessionDeleteOp.setDeletes([&] {
            std::vector<write_ops::DeleteOpEntry> entries;
            for (const auto& transactionSessionId : transactionSessionIdsToReap) {
                entries.emplace_back(
                    BSON(LogicalSessionRecord::kIdFieldName << transactionSessionId.toBSON()),
                    false /* multi = false */);
            }
            return entries;
        }());
        return sessionDeleteOp;
    }()));

    return sessionDeleteReply.getN();
}

/**
 * Removes the config.transactions and the config.image_collection entries for the transaction
 * sessions in 'expiredTransactionSessionIdsNotInUse' whose logical sessions have expired. Returns
 * the number of transaction sessions whose entries were removed.
 */
int removeSessionsTransactionRecordsIfExpired(
    OperationContext* opCtx,
    SessionsCollection& sessionsCollection,
    const LogicalSessionIdSet& expiredTransactionSessionIdsNotInUse) {
    if (expiredTransactionSessionIdsNotInUse.empty()) {
        return 0;
    }

    // From the expired transaction session ids that are no longer in use, find the ones whose
    // logical sessions have been removed from from the config.system.sessions collection.
    LogicalSessionIdSet transactionSessionIdsToReap;
    {
        LogicalSessionIdSet possiblyExpiredLogicalSessionIds;
        for (const auto& transactionSessionId : expiredTransactionSessionIdsNotInUse) {
            const auto logicalSessionId = isParentSessionId(transactionSessionId)
                ? transactionSessionId
                : *getParentSessionId(transactionSessionId);
            possiblyExpiredLogicalSessionIds.insert(std::move(logicalSessionId));
        }
        auto expiredLogicalSessionIds =
            sessionsCollection.findRemovedSessions(opCtx, possiblyExpiredLogicalSessionIds);

        for (const auto& transactionSessionId : expiredTransactionSessionIdsNotInUse) {
            const auto logicalSessionId = isParentSessionId(transactionSessionId)
                ? transactionSessionId
                : *getParentSessionId(transactionSessionId);
            if (expiredLogicalSessionIds.find(logicalSessionId) != expiredLogicalSessionIds.end()) {
                transactionSessionIdsToReap.insert(transactionSessionId);
            }
        }
    }

    return removeSessionsTransactionRecordsFromDisk(opCtx, transactionSessionIdsToReap);
}

/**
 * Removes the transaction sessions that are expired and not in use from the on-disk catalog (i.e.
 * the config.transactions collection and the config.image_collection collection). Returns the
 * number of transaction sessions whose entries were removed.
 */
int removeExpiredTransactionSessionsFromDisk(
    OperationContext* opCtx,
    SessionsCollection& sessionsCollection,
    Date_t possiblyExpired,
    const LogicalSessionIdSet& expiredTransactionSessionIdsStillInUse) {
    // Scan for records older than the minimum lifetime and uses a sort to walk the '_id' index.
    DBDirectClient client(opCtx);
    FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
    findRequest.setFilter(BSON(kLastWriteDateFieldName << LT << possiblyExpired));
    findRequest.setSort(kSortById);
    findRequest.setProjection(kIdProjection);
    auto cursor = client.find(std::move(findRequest));

    LogicalSessionIdSet expiredTransactionSessionIdsNotInUse;
    int numReaped = 0;
    while (cursor->more()) {
        auto transactionSession = SessionsCollectionFetchResultIndividualResult::parse(
            cursor->next(), IDLParserContext{"TransactionSession"});
        const auto transactionSessionId = transactionSession.get_id();

        if (expiredTransactionSessionIdsStillInUse.find(transactionSessionId) !=
            expiredTransactionSessionIdsStillInUse.end()) {
            continue;
        }

        expiredTransactionSessionIdsNotInUse.insert(transactionSessionId);
        if (expiredTransactionSessionIdsNotInUse.size() >
            MongoDSessionCatalog::kMaxSessionDeletionBatchSize) {
            numReaped += removeSessionsTransactionRecordsIfExpired(
                opCtx, sessionsCollection, expiredTransactionSessionIdsNotInUse);
            expiredTransactionSessionIdsNotInUse.clear();
        }
    }
    numReaped += removeSessionsTransactionRecordsIfExpired(
        opCtx, sessionsCollection, expiredTransactionSessionIdsNotInUse);

    return numReaped;
}

void createTransactionTable(OperationContext* opCtx) {
    CollectionOptions options;
    // We cluster by _id for improved performance at the cost of increased index maintenance.
    // Because we only have one partial index on this collection, the performance benefit outweighs
    // that cost.
    if (feature_flags::gFeatureFlagClusteredConfigTransactions.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()))
        options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    auto storageInterface = repl::StorageInterface::get(opCtx);
    auto createCollectionStatus = storageInterface->createCollection(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, options);

    if (createCollectionStatus == ErrorCodes::NamespaceExists) {
        bool collectionIsEmpty = false;
        {
            AutoGetCollection autoColl(
                opCtx, NamespaceString::kSessionTransactionsTableNamespace, LockMode::MODE_IS);
            invariant(autoColl);

            if (autoColl->getIndexCatalog()->findIndexByName(
                    opCtx, MongoDSessionCatalog::kConfigTxnsPartialIndexName)) {
                // Index already exists, so there's nothing to do.
                return;
            }

            collectionIsEmpty = autoColl->isEmpty(opCtx);
        }

        if (!collectionIsEmpty) {
            // Unless explicitly enabled, don't create the index to avoid delaying step up.
            // (Ignore FCV check): This is used to fix a bug in Atlas where the index is not created
            // in a cluster. This feature flag may no longer be used.
            if (feature_flags::gFeatureFlagAlwaysCreateConfigTransactionsPartialIndexOnStepUp
                    .isEnabledAndIgnoreFCVUnsafe()) {
                AutoGetCollection autoColl(
                    opCtx, NamespaceString::kSessionTransactionsTableNamespace, LockMode::MODE_X);
                IndexBuildsCoordinator::get(opCtx)->createIndex(
                    opCtx,
                    autoColl->uuid(),
                    MongoDSessionCatalog::getConfigTxnPartialIndexSpec(),
                    IndexBuildsManager::IndexConstraints::kEnforce,
                    false /* fromMigration */);
            }

            return;
        }

        // The index does not exist and the collection is empty, so fall through to create it on the
        // empty collection. This can happen after a failover because the collection and index
        // creation are recorded as separate oplog entries.
    } else {
        uassertStatusOKWithContext(
            createCollectionStatus,
            str::stream()
                << "Failed to create the "
                << NamespaceString::kSessionTransactionsTableNamespace.toStringForErrorMsg()
                << " collection");
    }

    auto indexSpec = MongoDSessionCatalog::getConfigTxnPartialIndexSpec();

    const auto createIndexStatus = storageInterface->createIndexesOnEmptyCollection(
        opCtx, NamespaceString::kSessionTransactionsTableNamespace, {indexSpec});
    uassertStatusOKWithContext(
        createIndexStatus,
        str::stream() << "Failed to create partial index for the "
                      << NamespaceString::kSessionTransactionsTableNamespace.toStringForErrorMsg()
                      << " collection");
}

void createRetryableFindAndModifyTable(OperationContext* opCtx) {
    auto serviceCtx = opCtx->getServiceContext();
    CollectionOptions options;
    auto status = repl::StorageInterface::get(serviceCtx)
                      ->createCollection(opCtx, NamespaceString::kConfigImagesNamespace, options);
    if (status == ErrorCodes::NamespaceExists) {
        return;
    }

    uassertStatusOKWithContext(status,
                               str::stream()
                                   << "Failed to create the "
                                   << NamespaceString::kConfigImagesNamespace.toStringForErrorMsg()
                                   << " collection");
}


void abortInProgressTransactions(OperationContext* opCtx,
                                 MongoDSessionCatalog* mongoDSessionCatalog,
                                 MongoDSessionCatalogTransactionInterface* ti) {
    DBDirectClient client(opCtx);
    FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
    findRequest.setFilter(BSON(SessionTxnRecord::kStateFieldName
                               << DurableTxnState_serializer(DurableTxnStateEnum::kInProgress)));
    auto cursor = client.find(std::move(findRequest));

    if (cursor->more()) {
        LOGV2_DEBUG(21977, 3, "Aborting in-progress transactions on stepup.");
    }
    while (cursor->more()) {
        auto txnRecord = SessionTxnRecord::parse(
            cursor->next(), IDLParserContext("abort-in-progress-transactions"));

        // Synchronize with killOps to make this unkillable.
        {
            auto lk = stdx::lock_guard(*opCtx->getClient());
            opCtx->setKillOpsExempt();
            opCtx->setLogicalSessionId(txnRecord.getSessionId());
        }
        opCtx->setTxnNumber(txnRecord.getTxnNum());
        opCtx->setInMultiDocumentTransaction();

        hangDuringStepUpAbortInProgressTransactions.pauseWhileSet();
        auto ocs = mongoDSessionCatalog->checkOutSessionWithoutRefresh(opCtx);
        ti->abortTransaction(opCtx, txnRecord);
    }
}

void _checkInUnscopedSession(OperationContext* opCtx,
                             OperationContextSession::CheckInReason reason) {
    OperationContextSession::checkIn(opCtx, reason);
}

void _checkOutUnscopedSession(OperationContext* opCtx,
                              MongoDSessionCatalogTransactionInterface* ti) {
    OperationContextSession::checkOut(opCtx);
    ti->refreshTransactionFromStorageIfNeeded(opCtx);
}

}  // namespace

const std::string MongoDSessionCatalog::kConfigTxnsPartialIndexName = "parent_lsid";

MongoDSessionCatalog* MongoDSessionCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

MongoDSessionCatalog* MongoDSessionCatalog::get(ServiceContext* service) {
    const auto& sessionCatalog = getMongoDSessionCatalog(service);
    invariant(sessionCatalog);
    return sessionCatalog.get();
}

void MongoDSessionCatalog::set(ServiceContext* service,
                               std::unique_ptr<MongoDSessionCatalog> sessionCatalog) {
    getMongoDSessionCatalog(service) = std::move(sessionCatalog);

    // Set mongod specific behaviors on the SessionCatalog.
    SessionCatalog::get(service)->setEagerReapSessionsFns(
        InternalTransactionsReapService::onEagerlyReapedSessions,
        [](ServiceContext* service,
           TxnNumber clientTxnNumberStarted,
           SessionCatalog::Provenance provenance) {
            return MongoDSessionCatalog::get(service)->makeSessionWorkerFnForEagerReap(
                clientTxnNumberStarted, provenance);
        });
}

BSONObj MongoDSessionCatalog::getConfigTxnPartialIndexSpec() {
    NewIndexSpec index;
    index.setV(int(IndexConfig::kLatestIndexVersion));
    index.setKey(BSON(
        SessionTxnRecord::kParentSessionIdFieldName
        << 1
        << (SessionTxnRecord::kSessionIdFieldName + "." + LogicalSessionId::kTxnNumberFieldName)
        << 1 << SessionTxnRecord::kSessionIdFieldName << 1));
    index.setName(MongoDSessionCatalog::kConfigTxnsPartialIndexName);
    index.setPartialFilterExpression(BSON("parentLsid" << BSON("$exists" << true)));
    return index.toBSON();
}

MongoDSessionCatalog::MongoDSessionCatalog(
    std::unique_ptr<MongoDSessionCatalogTransactionInterface> ti)
    : _ti(std::move(ti)) {}

void MongoDSessionCatalog::onStepUp(OperationContext* opCtx) {
    // Invalidate sessions that could have a retryable write on it, so that we can refresh from disk
    // in case the in-memory state was out of sync.
    const auto catalog = SessionCatalog::get(opCtx);

    std::vector<SessionCatalog::KillToken> sessionKillTokens;

    // Scan all sessions and reacquire locks for prepared transactions.
    // There may be sessions that are checked out during this scan, but none of them
    // can be prepared transactions, since only oplog application can make transactions
    // prepared on secondaries and oplog application has been stopped at this moment.
    std::vector<OperationSessionInfo> sessionsToReacquireLocks;

    SessionKiller::Matcher matcher(
        KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
    catalog->scanSessions(
        matcher, _ti->makeSessionWorkerFnForStepUp(&sessionKillTokens, &sessionsToReacquireLocks));
    killSessionTokens(opCtx, _ti.get(), std::move(sessionKillTokens));

    if (sessionsToReacquireLocks.size() > 0) {
        LOGV2(8083200, "Reacquiring locks for prepared transactions on step-up.");
        // Create a new opCtx because we need an empty locker to refresh the locks.
        auto newClient = opCtx->getServiceContext()
                             ->getService(ClusterRole::ShardServer)
                             ->makeClient("restore-prepared-txn",
                                          Client::noSession(),
                                          ClientOperationKillableByStepdown{false});

        AlternativeClientRegion acr(newClient);
        for (const auto& sessionInfo : sessionsToReacquireLocks) {
            auto newOpCtx = cc().makeOperationContext();

            // Avoid ticket acquisition during step up.
            ScopedAdmissionPriority<ExecutionAdmissionContext> admissionPriority(
                newOpCtx.get(), AdmissionContext::Priority::kExempt);

            // Synchronize with killOps to make this unkillable.
            {
                auto lk = stdx::lock_guard(*newOpCtx->getClient());
                newOpCtx->setKillOpsExempt();
                newOpCtx->setLogicalSessionId(*sessionInfo.getSessionId());
            }
            newOpCtx->setTxnNumber(*sessionInfo.getTxnNumber());
            newOpCtx->setTxnRetryCounter(*sessionInfo.getTxnRetryCounter());
            newOpCtx->setInMultiDocumentTransaction();

            hangDuringStepUpPrepareRestoreLocks.pauseWhileSet();

            // Use MongoDOperationContextSessionWithoutRefresh to check out the session because:
            // - The in-memory state for this session has been kept in sync with the on-disk state
            //   by secondary oplog application for prepared transactions so no refresh will be
            //   done anyway.
            // - The in-memory state for any external and/or internal sessions associated with this
            //   session may be out-of-date with the on-disk state but no refresh is necessary since
            //   the transaction is already in the prepared state so it no longer needs to go
            //   through conflict and write history check. In addition, a refresh of any session is
            //   expected to cause a deadlock since this 'newOpCtx' will need to acquire the global
            //   lock in the IS mode prior to reading the config.transactions collection but it
            //   cannot do that while the RSTL lock is being held by 'opCtx'.
            auto ocs = checkOutSessionWithoutRefresh(newOpCtx.get());
            _ti->refreshLocksForPreparedTransaction(newOpCtx.get(), sessionInfo);
        }
    }

    abortInProgressTransactions(opCtx, this, _ti.get());

    createTransactionTable(opCtx);
    createRetryableFindAndModifyTable(opCtx);
}

void MongoDSessionCatalog::observeDirectWriteToConfigTransactions(OperationContext* opCtx,
                                                                  BSONObj singleSessionDoc) {
    disallowDirectWritesUnderSession(opCtx);

    class KillSessionTokenOnCommit : public RecoveryUnit::Change {
    public:
        KillSessionTokenOnCommit(MongoDSessionCatalogTransactionInterface* ti,
                                 SessionCatalog::KillToken sessionKillToken)
            : _ti(ti), _sessionKillToken(std::move(sessionKillToken)) {}

        void commit(OperationContext* opCtx, boost::optional<Timestamp>) noexcept override {
            rollback(opCtx);
        }

        void rollback(OperationContext* opCtx) noexcept override {
            std::vector<SessionCatalog::KillToken> sessionKillTokenVec;
            sessionKillTokenVec.emplace_back(std::move(_sessionKillToken));
            killSessionTokens(opCtx, _ti, std::move(sessionKillTokenVec));
        }

    private:
        MongoDSessionCatalogTransactionInterface* _ti;
        SessionCatalog::KillToken _sessionKillToken;
    };

    const auto catalog = SessionCatalog::get(opCtx);

    const auto lsid =
        LogicalSessionId::parse(singleSessionDoc["_id"].Obj(), IDLParserContext("lsid"));
    catalog->scanSession(lsid, [&, ti = _ti.get()](const ObservableSession& session) {
        uassert(ErrorCodes::PreparedTransactionInProgress,
                str::stream() << "Cannot modify the entry for session " << lsid.getId()
                              << " because it is in the prepared state",
                !ti->isTransactionPrepared(session));

        // Internal sessions for an old retryable write are marked as reapable as soon as a
        // retryable write or transaction with a newer txnNumber starts. Therefore, when deleting
        // the config.transactions doc for such internal sessions, the corresponding transaction
        // sessions should not be interrupted since they are guaranteed to be performing a
        // transaction or retryable write for newer txnNumber.
        bool shouldRegisterKill = !isInternalSessionForRetryableWrite(lsid) ||
            *lsid.getTxnNumber() >= session.getLastClientTxnNumberStarted();
        if (shouldRegisterKill) {
            shard_role_details::getRecoveryUnit(opCtx)->registerChange(
                std::make_unique<KillSessionTokenOnCommit>(ti,
                                                           session.kill(ErrorCodes::Interrupted)));
        }
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

    killSessionTokens(opCtx, _ti.get(), std::move(sessionKillTokens));
}

int MongoDSessionCatalog::reapSessionsOlderThan(OperationContext* opCtx,
                                                SessionsCollection& sessionsCollection,
                                                Date_t possiblyExpired) {
    const auto expiredTransactionSessionIdsStillInUse =
        removeExpiredTransactionSessionsNotInUseFromMemory(
            opCtx, _ti.get(), sessionsCollection, possiblyExpired);

    // The "unsafe" check for primary below is a best-effort attempt to ensure that the on-disk
    // state reaping code doesn't run if the node is secondary and cause log spam. It is a work
    // around the fact that the logical sessions cache is not registered to listen for replication
    // state changes.
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->canAcceptWritesForDatabase_UNSAFE(opCtx, DatabaseName::kConfig))
        return 0;

    return removeExpiredTransactionSessionsFromDisk(
        opCtx, sessionsCollection, possiblyExpired, expiredTransactionSessionIdsStillInUse);
}

int MongoDSessionCatalog::removeSessionsTransactionRecords(
    OperationContext* opCtx, const std::vector<LogicalSessionId>& transactionSessionIdsToRemove) {
    std::vector<LogicalSessionId> nextLsidBatch;
    int numReaped = 0;
    for (const auto& transactionSessionIdToRemove : transactionSessionIdsToRemove) {
        nextLsidBatch.push_back(transactionSessionIdToRemove);
        if (nextLsidBatch.size() > MongoDSessionCatalog::kMaxSessionDeletionBatchSize) {
            numReaped += removeSessionsTransactionRecordsFromDisk(opCtx, nextLsidBatch);
            nextLsidBatch.clear();
        }
    }
    numReaped += removeSessionsTransactionRecordsFromDisk(opCtx, nextLsidBatch);

    return numReaped;
}

std::unique_ptr<MongoDSessionCatalog::Session> MongoDSessionCatalog::checkOutSession(
    OperationContext* opCtx) {
    return std::make_unique<MongoDOperationContextSession>(opCtx, _ti.get());
}

std::unique_ptr<MongoDSessionCatalog::Session> MongoDSessionCatalog::checkOutSessionWithoutRefresh(
    OperationContext* opCtx) {
    return std::make_unique<MongoDOperationContextSessionWithoutRefresh>(opCtx, _ti.get());
}

std::unique_ptr<MongoDSessionCatalog::Session>
MongoDSessionCatalog::checkOutSessionWithoutOplogRead(OperationContext* opCtx) {
    return std::make_unique<MongoDOperationContextSessionWithoutOplogRead>(opCtx, _ti.get());
}

void MongoDSessionCatalog::checkInUnscopedSession(OperationContext* opCtx,
                                                  OperationContextSession::CheckInReason reason) {
    _checkInUnscopedSession(opCtx, reason);
}

void MongoDSessionCatalog::checkOutUnscopedSession(OperationContext* opCtx) {
    _checkOutUnscopedSession(opCtx, _ti.get());
}

SessionCatalog::ScanSessionsCallbackFn MongoDSessionCatalog::makeSessionWorkerFnForEagerReap(
    TxnNumber clientTxnNumberStarted, SessionCatalog::Provenance provenance) {
    return _ti->makeSessionWorkerFnForEagerReap(clientTxnNumberStarted, provenance);
}

MongoDOperationContextSession::MongoDOperationContextSession(
    OperationContext* opCtx, MongoDSessionCatalogTransactionInterface* ti)
    : _operationContextSession(opCtx), _ti(ti) {
    invariant(!opCtx->getClient()->isInDirectClient());

    _ti->refreshTransactionFromStorageIfNeeded(opCtx);
}

MongoDOperationContextSession::~MongoDOperationContextSession() = default;

void MongoDOperationContextSession::checkIn(OperationContext* opCtx,
                                            OperationContextSession::CheckInReason reason) {
    _checkInUnscopedSession(opCtx, reason);
}

void MongoDOperationContextSession::checkOut(OperationContext* opCtx) {
    _checkOutUnscopedSession(opCtx, _ti);
}

MongoDOperationContextSessionWithoutRefresh::MongoDOperationContextSessionWithoutRefresh(
    OperationContext* opCtx, MongoDSessionCatalogTransactionInterface* ti)
    : _operationContextSession(opCtx), _opCtx(opCtx), _ti(ti) {
    invariant(!opCtx->getClient()->isInDirectClient());
    const auto clientTxnNumber = *opCtx->getTxnNumber();
    const auto clientTxnRetryCounter = *opCtx->getTxnRetryCounter();

    _ti->beginOrContinueTransactionUnconditionally(opCtx, {clientTxnNumber, clientTxnRetryCounter});
}

MongoDOperationContextSessionWithoutRefresh::~MongoDOperationContextSessionWithoutRefresh() {
    // A session on secondaries should never be checked back in with a TransactionParticipant that
    // isn't prepared, aborted, or committed.
    if (_ti->isTransactionInProgress(_opCtx)) {
        auto state = _ti->transactionStateDescriptor(_opCtx);
        auto txnNum = _opCtx->getTxnNumber().get_value_or(TxnNumber(-1));
        auto txnRetries = _opCtx->getTxnRetryCounter().get_value_or(-1);
        auto opId = _opCtx->getOpID();
        auto sessionId = _opCtx->getClient()->session()->id();
        auto lsid = _opCtx->getLogicalSessionId();
        auto clientAddress = _opCtx->getClient()->clientAddress(true);
        invariant(!_ti->isTransactionInProgress(_opCtx),
                  str::stream() << "state: " << state << " txnNum: " << txnNum
                                << " txnRetries: " << txnRetries << " opId: " << opId
                                << " lsid: " << lsid << " sessionId: " << sessionId
                                << " clientAddress: " << clientAddress);
    }
}

MongoDOperationContextSessionWithoutOplogRead::MongoDOperationContextSessionWithoutOplogRead(
    OperationContext* opCtx, MongoDSessionCatalogTransactionInterface* ti)
    : _operationContextSession(opCtx) {
    invariant(!opCtx->getClient()->isInDirectClient());

    ti->refreshTransactionFromStorageIfNeededNoOplogEntryFetch(opCtx);
}

MongoDOperationContextSessionWithoutOplogRead::~MongoDOperationContextSessionWithoutOplogRead() =
    default;

}  // namespace mongo
