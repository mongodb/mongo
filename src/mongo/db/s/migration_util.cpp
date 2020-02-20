/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_util.h"

#include <fmt/format.h>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/query.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_coordinator.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/s/wait_for_majority_service.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/ensure_chunk_version_is_greater_than_gen.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/exit.h"

namespace mongo {
namespace migrationutil {
namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(hangBeforeFilteringMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(hangInEnsureChunkVersionIsGreaterThanInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRefreshFilteringMetadataUntilSuccessInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateCommitDecisionInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateAbortDecisionInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionOnRecipientInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionLocallyInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionOnRecipientInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionLocallyInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInAdvanceTxnNumInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInAdvanceTxnNumThenSimulateErrorUninterruptible);

const char kSourceShard[] = "source";
const char kDestinationShard[] = "destination";
const char kIsDonorShard[] = "isDonorShard";
const char kChunk[] = "chunk";
const char kCollection[] = "collection";
const auto kLogRetryAttemptThreshold = 20;

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout);

template <typename Cmd>
void sendToRecipient(OperationContext* opCtx,
                     const ShardId& recipientId,
                     const Cmd& cmd,
                     const BSONObj& passthroughFields = {}) {
    auto recipientShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, recipientId));

    auto cmdBSON = cmd.toBSON(passthroughFields);
    LOGV2_DEBUG(22023, 1, "Sending request {cmd} to recipient.", "cmd"_attr = cmdBSON);

    auto response = recipientShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        cmd.getDbName().toString(),
        cmdBSON,
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));
}

// Returns an executor to be used to run commands related to submitting tasks to the range deleter.
// The executor is initialized on the first call to this function. Uses a shared_ptr
// because a shared_ptr is required to work with ExecutorFutures.
static std::shared_ptr<ThreadPool> getMigrationUtilExecutor() {
    static Mutex mutex = MONGO_MAKE_LATCH("MigrationUtilExecutor::_mutex");
    static std::shared_ptr<ThreadPool> executor;

    stdx::lock_guard<Latch> lg(mutex);
    if (!executor) {
        ThreadPool::Options options;
        options.poolName = "MoveChunk";
        options.minThreads = 0;
        options.maxThreads = 16;
        executor = std::make_shared<ThreadPool>(std::move(options));
        executor->startup();
    }

    return executor;
}

/**
 * Runs doWork until it doesn't throw an error.
 *
 * Requirements:
 * - doWork must be idempotent.
 */
void retryIdempotentWorkUntilSuccess(OperationContext* opCtx,
                                     StringData taskDescription,
                                     std::function<void(OperationContext*)> doWork) {
    const std::string newClientName = "{}-{}"_format(getThreadName(), taskDescription);
    const auto initialTerm = repl::ReplicationCoordinator::get(opCtx)->getTerm();

    for (int attempt = 1;; attempt++) {
        // If the server is already doing a clean shutdown, join the shutdown.
        if (globalInShutdownDeprecated()) {
            shutdown(waitForShutdown());
        }

        // If the term changed, that means that the step up recovery could have run or is running
        // so stop retrying in order to avoid duplicate work.
        uassert(ErrorCodes::InterruptedDueToReplStateChange,
                "Stepped down while {}"_format(taskDescription),
                repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
                        repl::MemberState::RS_PRIMARY &&
                    initialTerm == repl::ReplicationCoordinator::get(opCtx)->getTerm());

        try {
            auto newClient = opCtx->getServiceContext()->makeClient(newClientName);

            {
                stdx::lock_guard<Client> lk(*newClient.get());
                newClient->setSystemOperationKillable(lk);
            }

            auto newOpCtx = newClient->makeOperationContext();
            AlternativeClientRegion altClient(newClient);

            doWork(newOpCtx.get());
            break;
        } catch (DBException& ex) {
            if (attempt % kLogRetryAttemptThreshold == 1) {
                LOGV2_WARNING(23937,
                              "retrying {taskDescription} after {attempt} failed attempts, last "
                              "seen error: {ex}",
                              "taskDescription"_attr = taskDescription,
                              "attempt"_attr = attempt,
                              "ex"_attr = ex);
            }
        }
    }
}

}  // namespace

BSONObj makeMigrationStatusDocument(const NamespaceString& nss,
                                    const ShardId& fromShard,
                                    const ShardId& toShard,
                                    const bool& isDonorShard,
                                    const BSONObj& min,
                                    const BSONObj& max) {
    BSONObjBuilder builder;
    builder.append(kSourceShard, fromShard.toString());
    builder.append(kDestinationShard, toShard.toString());
    builder.append(kIsDonorShard, isDonorShard);
    builder.append(kChunk, BSON(ChunkType::min(min) << ChunkType::max(max)));
    builder.append(kCollection, nss.ns());
    return builder.obj();
}

Query overlappingRangeQuery(const ChunkRange& range, const UUID& uuid) {
    return QUERY(RangeDeletionTask::kCollectionUuidFieldName
                 << uuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinKey << LT
                 << range.getMax() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxKey
                 << GT << range.getMin());
}

bool checkForConflictingDeletions(OperationContext* opCtx,
                                  const ChunkRange& range,
                                  const UUID& uuid) {
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);

    return store.count(opCtx, overlappingRangeQuery(range, uuid)) > 0;
}

ExecutorFuture<void> submitRangeDeletionTask(OperationContext* opCtx,
                                             const RangeDeletionTask& deletionTask) {
    const auto serviceContext = opCtx->getServiceContext();
    return ExecutorFuture<void>(getMigrationUtilExecutor())
        .then([=] {
            ThreadClient tc(kRangeDeletionThreadName, serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillable(lk);
            }
            auto uniqueOpCtx = tc->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            boost::optional<AutoGetCollection> autoColl;
            autoColl.emplace(opCtx, deletionTask.getNss(), MODE_IS);

            auto css = CollectionShardingRuntime::get(opCtx, deletionTask.getNss());
            if (!css->getCurrentMetadataIfKnown() || !css->getCollectionDescription().isSharded() ||
                !css->getCollectionDescription().uuidMatches(deletionTask.getCollectionUuid())) {
                // If the collection's filtering metadata is not known, is unsharded, or its UUID
                // does not match the UUID of the deletion task, force a filtering metadata refresh
                // once, because this node may have just stepped up and therefore may have a stale
                // cache.
                LOGV2(
                    22024,
                    "Filtering metadata for namespace in deletion task "
                    "{deletionTask} {collectionStatus}, forcing a refresh of {deletionTask_getNss}",
                    "deletionTask"_attr = deletionTask.toBSON(),
                    "collectionStatus"_attr =
                        (css->getCurrentMetadataIfKnown()
                             ? (css->getCollectionDescription().isSharded()
                                    ? "has UUID that does not match UUID of the deletion task"
                                    : "is unsharded")
                             : "is not known"),
                    "deletionTask_getNss"_attr = deletionTask.getNss(),
                    "migrationId"_attr = deletionTask.getId());

                // TODO (SERVER-46075): Add an asynchronous version of
                // forceShardFilteringMetadataRefresh to avoid blocking on the network in the
                // thread pool.
                autoColl.reset();
                refreshFilteringMetadataUntilSuccess(opCtx, deletionTask.getNss());
            }

            autoColl.emplace(opCtx, deletionTask.getNss(), MODE_IS);
            uassert(
                ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist,
                str::stream() << "Even after forced refresh, filtering metadata for namespace in "
                                 "deletion task "
                              << (css->getCollectionDescription().isSharded()
                                      ? " has UUID that does not match UUID of the deletion task"
                                      : " is unsharded"),
                css->getCollectionDescription().isSharded() &&
                    css->getCollectionDescription().uuidMatches(deletionTask.getCollectionUuid()));

            LOGV2(22026,
                  "Submitting range deletion task {deletionTask}",
                  "deletionTask"_attr = deletionTask.toBSON(),
                  "migrationId"_attr = deletionTask.getId());

            const auto whenToClean = deletionTask.getWhenToClean() == CleanWhenEnum::kNow
                ? CollectionShardingRuntime::kNow
                : CollectionShardingRuntime::kDelayed;

            return css->cleanUpRange(deletionTask.getRange(), whenToClean);
        })
        .onError([=](const Status status) {
            ThreadClient tc(kRangeDeletionThreadName, serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillable(lk);
            }
            auto uniqueOpCtx = tc->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            LOGV2(22027,
                  "Failed to submit range deletion task "
                  "{deletionTask}{causedBy_status}",
                  "deletionTask"_attr = deletionTask.toBSON(),
                  "causedBy_status"_attr = causedBy(status),
                  "migrationId"_attr = deletionTask.getId());

            if (status == ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist) {
                deleteRangeDeletionTaskLocally(
                    opCtx, deletionTask.getId(), ShardingCatalogClient::kLocalWriteConcern);
            }

            // Note, we use onError and make it return its input status, because ExecutorFuture does
            // not support tapError.
            return status;
        });
}

void submitPendingDeletions(OperationContext* opCtx) {
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);

    auto query = QUERY("pending" << BSON("$exists" << false));

    store.forEach(opCtx, query, [&opCtx](const RangeDeletionTask& deletionTask) {
        migrationutil::submitRangeDeletionTask(opCtx, deletionTask).getAsync([](auto) {});
        return true;
    });
}

void resubmitRangeDeletionsOnStepUp(ServiceContext* serviceContext) {
    LOGV2(22028, "Starting pending deletion submission thread.");

    ExecutorFuture<void>(getMigrationUtilExecutor())
        .then([serviceContext] {
            ThreadClient tc("ResubmitRangeDeletions", serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillable(lk);
            }

            auto opCtx = tc->makeOperationContext();

            submitPendingDeletions(opCtx.get());
        })
        .getAsync([](const Status& status) {
            if (!status.isOK()) {
                LOGV2(45739,
                      "Error while submitting pending deletions: {status}",
                      "status"_attr = status);
            }
        });
}

void dropRangeDeletionsCollection(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    client.dropCollection(NamespaceString::kRangeDeletionNamespace.toString(),
                          WriteConcerns::kMajorityWriteConcern);
}

template <typename Callable>
void forEachOrphanRange(OperationContext* opCtx, const NamespaceString& nss, Callable&& handler) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IX);

    const auto css = CollectionShardingRuntime::get(opCtx, nss);
    const auto collDesc = css->getCollectionDescription();
    const auto emptyChunkMap =
        RangeMap{SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>()};

    if (!collDesc.isSharded()) {
        LOGV2(22029,
              "Upgrade: skipping orphaned range enumeration for {nss}, collection is not sharded",
              "nss"_attr = nss);
        return;
    }

    auto startingKey = collDesc.getMinKey();

    while (true) {
        auto range = collDesc->getNextOrphanRange(emptyChunkMap, startingKey);
        if (!range) {
            LOGV2_DEBUG(22030,
                        2,
                        "Upgrade: Completed orphaned range enumeration for {nss} starting from "
                        "{startingKey}, no orphan ranges remain",
                        "nss"_attr = nss.toString(),
                        "startingKey"_attr = redact(startingKey));

            return;
        }

        handler(*range);

        startingKey = range->getMax();
    }
}

void submitOrphanRanges(OperationContext* opCtx, const NamespaceString& nss, const UUID& uuid) {
    try {
        auto version = forceShardFilteringMetadataRefresh(opCtx, nss, true);

        if (version == ChunkVersion::UNSHARDED())
            return;

        LOGV2_DEBUG(22031,
                    2,
                    "Upgrade: Cleaning up existing orphans for {nss} : {uuid}",
                    "nss"_attr = nss,
                    "uuid"_attr = uuid);

        std::vector<RangeDeletionTask> deletions;
        forEachOrphanRange(opCtx, nss, [&deletions, &opCtx, &nss, &uuid](const auto& range) {
            // Since this is not part of an active migration, the migration UUID and the donor shard
            // are set to unused values so that they don't conflict.
            RangeDeletionTask task(
                UUID::gen(), nss, uuid, ShardId("fromFCVUpgrade"), range, CleanWhenEnum::kDelayed);
            deletions.emplace_back(task);
        });

        if (deletions.empty())
            return;

        PersistentTaskStore<RangeDeletionTask> store(opCtx,
                                                     NamespaceString::kRangeDeletionNamespace);

        for (const auto& task : deletions) {
            LOGV2_DEBUG(22032,
                        2,
                        "Upgrade: Submitting range for cleanup: {task_getRange} from {nss}",
                        "task_getRange"_attr = task.getRange(),
                        "nss"_attr = nss);
            store.add(opCtx, task);
        }
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>& e) {
        LOGV2(22033,
              "Upgrade: Failed to cleanup orphans for {nss} because the namespace was not found: "
              "{e_what}, the collection must have been dropped",
              "nss"_attr = nss,
              "e_what"_attr = e.what());
    }
}

void submitOrphanRangesForCleanup(OperationContext* opCtx) {
    auto& catalog = CollectionCatalog::get(opCtx);
    const auto& dbs = catalog.getAllDbNames();

    for (const auto& dbName : dbs) {
        if (dbName == NamespaceString::kLocalDb)
            continue;

        for (auto collIt = catalog.begin(dbName); collIt != catalog.end(); ++collIt) {
            auto uuid = collIt.uuid().get();
            auto nss = catalog.lookupNSSByUUID(opCtx, uuid).get();
            LOGV2_DEBUG(22034, 2, "Upgrade: processing collection: {nss}", "nss"_attr = nss);

            submitOrphanRanges(opCtx, nss, uuid);
        }
    }
}

void persistMigrationCoordinatorLocally(OperationContext* opCtx,
                                        const MigrationCoordinatorDocument& migrationDoc) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        opCtx, NamespaceString::kMigrationCoordinatorsNamespace);
    try {
        store.add(opCtx, migrationDoc);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(
            31374,
            str::stream() << "While attempting to write migration information for migration "
                          << ", found document with the same migration id. Attempted migration: "
                          << migrationDoc.toBSON());
    }
}

void persistRangeDeletionTaskLocally(OperationContext* opCtx,
                                     const RangeDeletionTask& deletionTask) {
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);
    try {
        store.add(opCtx, deletionTask);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(31375,
                  str::stream() << "While attempting to write range deletion task for migration "
                                << ", found document with the same migration id. Attempted range "
                                   "deletion task: "
                                << deletionTask.toBSON());
    }
}

void persistCommitDecision(OperationContext* opCtx, const UUID& migrationId) {
    retryIdempotentWorkUntilSuccess(
        opCtx, "persist migrate commit decision", [&](OperationContext* newOpCtx) {
            hangInPersistMigrateCommitDecisionInterruptible.pauseWhileSet(newOpCtx);

            PersistentTaskStore<MigrationCoordinatorDocument> store(
                newOpCtx, NamespaceString::kMigrationCoordinatorsNamespace);
            store.update(newOpCtx,
                         QUERY(MigrationCoordinatorDocument::kIdFieldName << migrationId),
                         BSON("$set" << BSON(MigrationCoordinatorDocument::kDecisionFieldName
                                             << "committed")));

            if (hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible.shouldFail()) {
                hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible.pauseWhileSet(
                    newOpCtx);
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response when persisting migrate commit decision");
            }
        });
}

void persistAbortDecision(OperationContext* opCtx, const UUID& migrationId) {
    retryIdempotentWorkUntilSuccess(
        opCtx, "persist migrate abort decision", [&](OperationContext* newOpCtx) {
            hangInPersistMigrateAbortDecisionInterruptible.pauseWhileSet(newOpCtx);

            PersistentTaskStore<MigrationCoordinatorDocument> store(
                newOpCtx, NamespaceString::kMigrationCoordinatorsNamespace);
            store.update(newOpCtx,
                         QUERY(MigrationCoordinatorDocument::kIdFieldName << migrationId),
                         BSON("$set" << BSON(MigrationCoordinatorDocument::kDecisionFieldName
                                             << "aborted")));

            if (hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible.shouldFail()) {
                hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible.pauseWhileSet(
                    newOpCtx);
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response when persisting migrate abort decision");
            }
        });
}

void deleteRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                        const ShardId& recipientId,
                                        const UUID& migrationId) {
    write_ops::Delete deleteOp(NamespaceString::kRangeDeletionNamespace);
    write_ops::DeleteOpEntry query(BSON(RangeDeletionTask::kIdFieldName << migrationId),
                                   false /*multi*/);
    deleteOp.setDeletes({query});

    retryIdempotentWorkUntilSuccess(
        opCtx, "cancel range deletion on recipient", [&](OperationContext* newOpCtx) {
            hangInDeleteRangeDeletionOnRecipientInterruptible.pauseWhileSet(newOpCtx);

            sendToRecipient(
                newOpCtx,
                recipientId,
                deleteOp,
                BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));

            if (hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible.shouldFail()) {
                hangInDeleteRangeDeletionOnRecipientThenSimulateErrorUninterruptible.pauseWhileSet(
                    newOpCtx);
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response when deleting range deletion on recipient");
            }
        });
}

void deleteRangeDeletionTaskLocally(OperationContext* opCtx,
                                    const UUID& deletionTaskId,
                                    const WriteConcernOptions& writeConcern) {
    retryIdempotentWorkUntilSuccess(
        opCtx, "cancel local range deletion", [&](OperationContext* newOpCtx) {
            hangInDeleteRangeDeletionLocallyInterruptible.pauseWhileSet(newOpCtx);
            PersistentTaskStore<RangeDeletionTask> store(newOpCtx,
                                                         NamespaceString::kRangeDeletionNamespace);
            store.remove(
                newOpCtx, QUERY(RangeDeletionTask::kIdFieldName << deletionTaskId), writeConcern);

            if (hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible.shouldFail()) {
                hangInDeleteRangeDeletionLocallyThenSimulateErrorUninterruptible.pauseWhileSet(
                    newOpCtx);
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response when deleting range deletion locally");
            }
        });
}

void markAsReadyRangeDeletionTaskOnRecipient(OperationContext* opCtx,
                                             const ShardId& recipientId,
                                             const UUID& migrationId) {
    write_ops::Update updateOp(NamespaceString::kRangeDeletionNamespace);
    auto queryFilter = BSON(RangeDeletionTask::kIdFieldName << migrationId);
    auto updateModification = write_ops::UpdateModification(
        BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << "")));
    write_ops::UpdateOpEntry updateEntry(queryFilter, updateModification);
    updateEntry.setMulti(false);
    updateEntry.setUpsert(false);
    updateOp.setUpdates({updateEntry});

    retryIdempotentWorkUntilSuccess(
        opCtx, "ready remote range deletion", [&](OperationContext* newOpCtx) {
            hangInReadyRangeDeletionOnRecipientInterruptible.pauseWhileSet(newOpCtx);

            sendToRecipient(
                newOpCtx,
                recipientId,
                updateOp,
                BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));

            if (hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible.shouldFail()) {
                hangInReadyRangeDeletionOnRecipientThenSimulateErrorUninterruptible.pauseWhileSet(
                    newOpCtx);
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response when initiating range deletion on recipient");
            }
        });
}

void advanceTransactionOnRecipient(OperationContext* opCtx,
                                   const ShardId& recipientId,
                                   const LogicalSessionId& lsid,
                                   TxnNumber currentTxnNumber) {
    write_ops::Update updateOp(NamespaceString::kServerConfigurationNamespace);
    auto queryFilter = BSON("_id"
                            << "migrationCoordinatorStats");
    auto updateModification = write_ops::UpdateModification(BSON("$inc" << BSON("count" << 1)));

    write_ops::UpdateOpEntry updateEntry(queryFilter, updateModification);
    updateEntry.setMulti(false);
    updateEntry.setUpsert(true);
    updateOp.setUpdates({updateEntry});

    auto passthroughFields = BSON(WriteConcernOptions::kWriteConcernField
                                  << WriteConcernOptions::Majority << "lsid" << lsid.toBSON()
                                  << "txnNumber" << currentTxnNumber + 1);

    retryIdempotentWorkUntilSuccess(
        opCtx, "advance migration txn number", [&](OperationContext* newOpCtx) {
            hangInAdvanceTxnNumInterruptible.pauseWhileSet(newOpCtx);
            sendToRecipient(newOpCtx, recipientId, updateOp, passthroughFields);

            if (hangInAdvanceTxnNumThenSimulateErrorUninterruptible.shouldFail()) {
                hangInAdvanceTxnNumThenSimulateErrorUninterruptible.pauseWhileSet(newOpCtx);
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response when initiating range deletion locally");
            }
        });
}

void markAsReadyRangeDeletionTaskLocally(OperationContext* opCtx, const UUID& migrationId) {
    PersistentTaskStore<RangeDeletionTask> store(opCtx, NamespaceString::kRangeDeletionNamespace);
    auto query = QUERY(RangeDeletionTask::kIdFieldName << migrationId);
    auto update = BSON("$unset" << BSON(RangeDeletionTask::kPendingFieldName << ""));

    retryIdempotentWorkUntilSuccess(
        opCtx, "ready local range deletion", [&](OperationContext* newOpCtx) {
            hangInReadyRangeDeletionLocallyInterruptible.pauseWhileSet(newOpCtx);
            store.update(newOpCtx, query, update);

            if (hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible.shouldFail()) {
                hangInReadyRangeDeletionLocallyThenSimulateErrorUninterruptible.pauseWhileSet(
                    newOpCtx);
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response when initiating range deletion locally");
            }
        });
}

void deleteMigrationCoordinatorDocumentLocally(OperationContext* opCtx, const UUID& migrationId) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        opCtx, NamespaceString::kMigrationCoordinatorsNamespace);
    store.remove(opCtx,
                 QUERY(MigrationCoordinatorDocument::kIdFieldName << migrationId),
                 {1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)});
}

void ensureChunkVersionIsGreaterThan(OperationContext* opCtx,
                                     const ChunkRange& range,
                                     const ChunkVersion& preMigrationChunkVersion) {
    ConfigsvrEnsureChunkVersionIsGreaterThan ensureChunkVersionIsGreaterThanRequest;
    ensureChunkVersionIsGreaterThanRequest.setDbName(NamespaceString::kAdminDb);
    ensureChunkVersionIsGreaterThanRequest.setMinKey(range.getMin());
    ensureChunkVersionIsGreaterThanRequest.setMaxKey(range.getMax());
    ensureChunkVersionIsGreaterThanRequest.setVersion(preMigrationChunkVersion);
    const auto ensureChunkVersionIsGreaterThanRequestBSON =
        ensureChunkVersionIsGreaterThanRequest.toBSON({});

    retryIdempotentWorkUntilSuccess(
        opCtx, "ensureChunkVersionIsGreaterThan", [&](OperationContext* newOpCtx) {
            hangInEnsureChunkVersionIsGreaterThanInterruptible.pauseWhileSet(newOpCtx);

            const auto ensureChunkVersionIsGreaterThanResponse =
                Grid::get(newOpCtx)
                    ->shardRegistry()
                    ->getConfigShard()
                    ->runCommandWithFixedRetryAttempts(
                        newOpCtx,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        "admin",
                        CommandHelpers::appendMajorityWriteConcern(
                            ensureChunkVersionIsGreaterThanRequestBSON),
                        Shard::RetryPolicy::kIdempotent);
            const auto ensureChunkVersionIsGreaterThanStatus =
                Shard::CommandResponse::getEffectiveStatus(ensureChunkVersionIsGreaterThanResponse);

            uassertStatusOK(ensureChunkVersionIsGreaterThanStatus);

            if (hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible
                    .shouldFail()) {
                hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible
                    .pauseWhileSet();
                uasserted(
                    ErrorCodes::InternalError,
                    "simulate an error response for _configsvrEnsureChunkVersionIsGreaterThan");
            }
        });
}

void refreshFilteringMetadataUntilSuccess(OperationContext* opCtx, const NamespaceString& nss) {
    retryIdempotentWorkUntilSuccess(
        opCtx, "refreshFilteringMetadataUntilSuccess", [&nss](OperationContext* newOpCtx) {
            hangInRefreshFilteringMetadataUntilSuccessInterruptible.pauseWhileSet(newOpCtx);

            try {
                forceShardFilteringMetadataRefresh(newOpCtx, nss, true);
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // A filtering metadata refresh can throw NamespaceNotFound if the database was
                // dropped from the cluster.
            }

            if (hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .shouldFail()) {
                hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .pauseWhileSet();
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response for forceShardFilteringMetadataRefresh");
            }
        });
}

void resumeMigrationCoordinationsOnStepUp(ServiceContext* serviceContext) {
    LOGV2(22037, "Starting migration coordinator stepup recovery thread.");

    ExecutorFuture<void>(getMigrationUtilExecutor())
        .then([serviceContext] {
            ThreadClient tc("MigrationCoordinatorStepupRecovery", serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillable(lk);
            }
            auto uniqueOpCtx = tc->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            // Wait for the latest OpTime to be majority committed to ensure any decision that is
            // read is on the true branch of history.
            // Note (Esha): I don't think this is strictly required for correctness, but it is
            // difficult to reason about, and being pessimistic by waiting for the decision to be
            // majority committed does not cost much, since stepup should be rare. It *is* required
            // that this node ensure a decision that it itself recovers is majority committed. For
            // example, it is possible that this node is a stale primary, and the true primary has
            // already sent a *commit* decision and re-received a chunk containing the minKey of
            // this migration. In this case, this node would see that the minKey is still owned and
            // assume the migration *aborted*. If this node communicated the abort decision to the
            // recipient, the recipient (if it had not heard the decision yet) would delete data
            // that the recipient actually owns. (The recipient does not currently wait to hear the
            // range deletion decision for the first migration before being able to donate (any
            // part of) the chunk again.)
            auto& replClientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
            replClientInfo.setLastOpToSystemLastOpTime(opCtx);
            const auto lastOpTime = replClientInfo.getLastOp();
            LOGV2_DEBUG(22038,
                        2,
                        "Waiting for OpTime {lastOpTime} to become majority committed",
                        "lastOpTime"_attr = lastOpTime);
            return WaitForMajorityService::get(serviceContext).waitUntilMajority(lastOpTime);
        })
        .thenRunOn(getMigrationUtilExecutor())
        .then([serviceContext]() {
            ThreadClient tc("MigrationCoordinatorStepupRecovery", serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillable(lk);
            }
            auto uniqueOpCtx = tc->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            long long migrationRecoveryCount = 0;
            PersistentTaskStore<MigrationCoordinatorDocument> store(
                opCtx, NamespaceString::kMigrationCoordinatorsNamespace);
            Query query;
            store.forEach(
                opCtx,
                query,
                [&opCtx, &migrationRecoveryCount](const MigrationCoordinatorDocument& doc) {
                    LOGV2_DEBUG(22039, 2, "Recovering migration {doc}", "doc"_attr = doc.toBSON());

                    migrationRecoveryCount++;

                    // Create a MigrationCoordinator to complete the coordination.
                    MigrationCoordinator coordinator(doc);

                    if (doc.getDecision()) {
                        // The decision is already known.
                        coordinator.setMigrationDecision(
                            (*doc.getDecision()) == DecisionEnum::kCommitted
                                ? MigrationCoordinator::Decision::kCommitted
                                : MigrationCoordinator::Decision::kAborted);
                        coordinator.completeMigration(opCtx);
                        return true;
                    }

                    // The decision is not known. Recover the decision from the config server.

                    ensureChunkVersionIsGreaterThan(
                        opCtx, doc.getRange(), doc.getPreMigrationChunkVersion());

                    hangBeforeFilteringMetadataRefresh.pauseWhileSet();

                    refreshFilteringMetadataUntilSuccess(opCtx, doc.getNss());

                    auto refreshedMetadata = [&] {
                        AutoGetCollection autoColl(opCtx, doc.getNss(), MODE_IS);
                        auto* const css = CollectionShardingRuntime::get(opCtx, doc.getNss());
                        return css->getCurrentMetadataIfKnown();
                    }();

                    if (!refreshedMetadata || !(*refreshedMetadata)->isSharded() ||
                        !(*refreshedMetadata)->uuidMatches(doc.getCollectionUuid())) {
                        LOGV2(
                            22040,
                            "Even after forced refresh, filtering metadata for namespace in "
                            "migration coordinator doc "
                            "{doc}{refreshedMetadata_refreshedMetadata_isSharded_is_not_known_has_"
                            "UUID_"
                            "that_does_not_match_the_collection_UUID_in_the_coordinator_doc}. "
                            "Deleting "
                            "the range deletion tasks on the donor and recipient as "
                            "well as the migration coordinator document on this node.",
                            "doc"_attr = doc.toBSON(),
                            "refreshedMetadata_refreshedMetadata_isSharded_is_not_known_has_UUID_that_does_not_match_the_collection_UUID_in_the_coordinator_doc"_attr =
                                (!refreshedMetadata || !(*refreshedMetadata)->isSharded()
                                     ? "is not known"
                                     : "has UUID that does not match the collection UUID in the "
                                       "coordinator doc"));

                        // TODO (SERVER-45707): Test that range deletion tasks are eventually
                        // deleted even if the collection is dropped before migration coordination
                        // is resumed.
                        deleteRangeDeletionTaskOnRecipient(
                            opCtx, doc.getRecipientShardId(), doc.getId());
                        deleteRangeDeletionTaskLocally(opCtx, doc.getId());
                        coordinator.forgetMigration(opCtx);
                        return true;
                    }

                    if ((*refreshedMetadata)->keyBelongsToMe(doc.getRange().getMin())) {
                        coordinator.setMigrationDecision(MigrationCoordinator::Decision::kAborted);
                    } else {
                        coordinator.setMigrationDecision(
                            MigrationCoordinator::Decision::kCommitted);
                    }

                    coordinator.completeMigration(opCtx);
                    return true;
                });

            ShardingStatistics::get(opCtx).unfinishedMigrationFromPreviousPrimary.store(
                migrationRecoveryCount);
        })
        .getAsync([](const Status& status) {
            if (!status.isOK()) {
                LOGV2(22041,
                      "Failed to resume coordinating migrations on stepup {causedBy_status}",
                      "causedBy_status"_attr = causedBy(status));
            }
        });
}

}  // namespace migrationutil
}  // namespace mongo
