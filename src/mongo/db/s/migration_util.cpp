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

#include "mongo/db/s/migration_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/migration_coordinator.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/decorable.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

namespace mongo {
namespace migrationutil {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeFilteringMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateCommitDecisionInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInAdvanceTxnNumInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInAdvanceTxnNumThenSimulateErrorUninterruptible);

const char kSourceShard[] = "source";
const char kDestinationShard[] = "destination";
const char kIsDonorShard[] = "isDonorShard";
const char kChunk[] = "chunk";
const char kCollection[] = "collection";
const char kSessionOplogEntriesMigrated[] = "sessionOplogEntriesMigrated";
const char ksessionOplogEntriesSkippedSoFarLowerBound[] =
    "sessionOplogEntriesSkippedSoFarLowerBound";
const char ksessionOplogEntriesToBeMigratedSoFar[] = "sessionOplogEntriesToBeMigratedSoFar";
const Backoff kExponentialBackoff(Seconds(10), Milliseconds::max());

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout);

void refreshFilteringMetadataUntilSuccess(OperationContext* opCtx, const NamespaceString& nss) {
    hangBeforeFilteringMetadataRefresh.pauseWhileSet();

    sharding_util::retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
        opCtx, "refreshFilteringMetadataUntilSuccess", [&nss](OperationContext* newOpCtx) {
            hangInRefreshFilteringMetadataUntilSuccessInterruptible.pauseWhileSet(newOpCtx);

            try {
                uassertStatusOK(
                    FilteringMetadataCache::get(newOpCtx)->onCollectionPlacementVersionMismatch(
                        newOpCtx, nss, boost::none));
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // Can throw NamespaceNotFound if the collection/database was dropped
            }

            if (hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .shouldFail()) {
                hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .pauseWhileSet();
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response for onCollectionPlacementVersionMismatch");
            }
        });
}

BSONObj getQueryFilterForRangeDeletionTask(const UUID& collectionUuid, const ChunkRange& range) {
    return BSON(
        RangeDeletionTask::kCollectionUuidFieldName
        << collectionUuid << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMinFieldName
        << range.getMin() << RangeDeletionTask::kRangeFieldName + "." + ChunkRange::kMaxFieldName
        << range.getMax());
}

// TODO SERVER-103838 Remove this function and replace any call with 'coordinatorDoc.toBSON()'
// once 9.0 becomes LTS.
BSONObj serializeAndRedactCoordinatorDocument(OperationContext* opCtx,
                                              const MigrationCoordinatorDocument& coordinatorDoc) {
    // Only persist the backwards-incompatible transfersFirstCollectionChunkToRecipient field if
    // the current FCV supports the needed recovery document schema.
    if (feature_flags::gPersistRecipientPlacementInfoInMigrationRecoveryDoc.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return coordinatorDoc.toBSON();
    }

    auto redactedCoordinatorDoc = coordinatorDoc;
    redactedCoordinatorDoc.setTransfersFirstCollectionChunkToRecipient(boost::none);
    return redactedCoordinatorDoc.toBSON();
}


}  // namespace

BSONObjBuilder _makeMigrationStatusDocumentCommon(const NamespaceString& nss,
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
    builder.append(kCollection,
                   NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
    return builder;
}

BSONObj makeMigrationStatusDocumentSource(
    const NamespaceString& nss,
    const ShardId& fromShard,
    const ShardId& toShard,
    const bool& isDonorShard,
    const BSONObj& min,
    const BSONObj& max,
    boost::optional<long long> sessionOplogEntriesToBeMigratedSoFar,
    boost::optional<long long> sessionOplogEntriesSkippedSoFarLowerBound) {
    BSONObjBuilder builder =
        _makeMigrationStatusDocumentCommon(nss, fromShard, toShard, isDonorShard, min, max);
    if (sessionOplogEntriesToBeMigratedSoFar) {
        builder.append(ksessionOplogEntriesToBeMigratedSoFar,
                       sessionOplogEntriesToBeMigratedSoFar.value());
    }
    if (sessionOplogEntriesSkippedSoFarLowerBound) {
        builder.append(ksessionOplogEntriesSkippedSoFarLowerBound,
                       sessionOplogEntriesSkippedSoFarLowerBound.value());
    }
    return builder.obj();
}

BSONObj makeMigrationStatusDocumentDestination(
    const NamespaceString& nss,
    const ShardId& fromShard,
    const ShardId& toShard,
    const bool& isDonorShard,
    const BSONObj& min,
    const BSONObj& max,
    boost::optional<long long> sessionOplogEntriesMigrated) {
    BSONObjBuilder builder =
        _makeMigrationStatusDocumentCommon(nss, fromShard, toShard, isDonorShard, min, max);
    if (sessionOplogEntriesMigrated) {
        builder.append(kSessionOplogEntriesMigrated, sessionOplogEntriesMigrated.value());
    }
    return builder.obj();
}

bool deletionTaskUuidMatchesFilteringMetadataUuid(
    OperationContext* opCtx,
    const boost::optional<mongo::CollectionMetadata>& optCollDescr,
    const RangeDeletionTask& deletionTask) {
    return optCollDescr && optCollDescr->isSharded() &&
        optCollDescr->uuidMatches(deletionTask.getCollectionUuid());
}

void insertMigrationCoordinatorDoc(OperationContext* opCtx,
                                   const MigrationCoordinatorDocument& migrationDoc) {
    // The transfersFirstCollectionChunkToRecipient is an optional, FCV gated field that is expected
    // to be only persisted through the migrationutil::updateMigrationCoordinatorDoc() method.
    // TODO SERVER-103838 remove the invariant once 9.0 becomes LTS.
    invariant(!migrationDoc.getTransfersFirstCollectionChunkToRecipient().has_value());

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
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

void updateMigrationCoordinatorDoc(OperationContext* opCtx,
                                   const MigrationCoordinatorDocument& migrationDoc) {
    const auto& docId = migrationDoc.getId();
    const auto serializedDoc = serializeAndRedactCoordinatorDocument(opCtx, migrationDoc);

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.update(opCtx, BSON(MigrationCoordinatorDocument::kIdFieldName << docId), serializedDoc);
}

void persistCommitDecision(OperationContext* opCtx,
                           const MigrationCoordinatorDocument& migrationDoc) {
    invariant(migrationDoc.getDecision() &&
              *migrationDoc.getDecision() == DecisionEnum::kCommitted);

    hangInPersistMigrateCommitDecisionInterruptible.pauseWhileSet(opCtx);
    try {
        updateMigrationCoordinatorDoc(opCtx, migrationDoc);
        ShardingStatistics::get(opCtx).countDonorMoveChunkCommitted.addAndFetch(1);
    } catch (const ExceptionFor<ErrorCodes::NoMatchingDocument>&) {
        LOGV2_ERROR(6439800,
                    "No coordination doc found on disk for migration",
                    "migration"_attr = redact(migrationDoc.toBSON()));
    }

    if (hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible.shouldFail()) {
        hangInPersistMigrateCommitDecisionThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when persisting migrate commit decision");
    }
}

void persistAbortDecision(OperationContext* opCtx,
                          const MigrationCoordinatorDocument& migrationDoc) {
    invariant(migrationDoc.getDecision() && *migrationDoc.getDecision() == DecisionEnum::kAborted);

    try {
        updateMigrationCoordinatorDoc(opCtx, migrationDoc);
        ShardingStatistics::get(opCtx).countDonorMoveChunkAborted.addAndFetch(1);
    } catch (const ExceptionFor<ErrorCodes::NoMatchingDocument>&) {
        LOGV2(6439801,
              "No coordination doc found on disk for migration",
              "migration"_attr = redact(migrationDoc.toBSON()));
    }

    if (hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible.shouldFail()) {
        hangInPersistMigrateAbortDecisionThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when persisting migrate abort decision");
    }
}

void advanceTransactionOnRecipient(OperationContext* opCtx,
                                   const ShardId& recipientId,
                                   const LogicalSessionId& lsid,
                                   TxnNumber currentTxnNumber) {
    write_ops::UpdateCommandRequest updateOp(NamespaceString::kServerConfigurationNamespace);
    auto queryFilter = BSON("_id" << "migrationCoordinatorStats");
    auto updateModification = write_ops::UpdateModification(
        write_ops::UpdateModification::parseFromClassicUpdate(BSON("$inc" << BSON("count" << 1))));

    write_ops::UpdateOpEntry updateEntry(queryFilter, updateModification);
    updateEntry.setMulti(false);
    updateEntry.setUpsert(true);
    updateOp.setUpdates({updateEntry});

    updateOp.setWriteConcern(defaultMajorityWriteConcernDoNotUse());
    updateOp.setLsid(generic_argument_util::toLogicalSessionFromClient(lsid));
    updateOp.setTxnNumber(currentTxnNumber + 1);

    hangInAdvanceTxnNumInterruptible.pauseWhileSet(opCtx);
    sharding_util::invokeCommandOnShardWithIdempotentRetryPolicy(
        opCtx,
        recipientId,
        NamespaceString::kServerConfigurationNamespace.dbName(),
        updateOp.toBSON());

    if (hangInAdvanceTxnNumThenSimulateErrorUninterruptible.shouldFail()) {
        hangInAdvanceTxnNumThenSimulateErrorUninterruptible.pauseWhileSet(opCtx);
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response when initiating range deletion locally");
    }
}

void resumeMigrationCoordinationsOnStepUp(OperationContext* opCtx) {
    LOGV2_DEBUG(4798510, 2, "Starting migration coordinator step-up recovery");

    unsigned long long unfinishedMigrationsCount = 0;

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.forEach(opCtx,
                  BSONObj{},
                  [&opCtx, &unfinishedMigrationsCount](const MigrationCoordinatorDocument& doc) {
                      unfinishedMigrationsCount++;
                      LOGV2_DEBUG(4798511,
                                  3,
                                  "Found unfinished migration on step-up",
                                  "migrationCoordinatorDoc"_attr = redact(doc.toBSON()),
                                  "unfinishedMigrationsCount"_attr = unfinishedMigrationsCount);

                      const auto& nss = doc.getNss();

                      {
                          AutoGetCollection autoColl(opCtx, nss, MODE_IX);
                          CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                              opCtx, nss)
                              ->clearFilteringMetadata(opCtx);
                      }

                      asyncRecoverMigrationUntilSuccessOrStepDown(opCtx, nss)
                          .thenRunOn(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor())
                          .getAsync([](auto) {});

                      return true;
                  });

    ShardingStatistics::get(opCtx).unfinishedMigrationFromPreviousPrimary.store(
        unfinishedMigrationsCount);

    LOGV2_DEBUG(4798513,
                2,
                "Finished migration coordinator step-up recovery",
                "unfinishedMigrationsCount"_attr = unfinishedMigrationsCount);
}

ExecutorFuture<void> launchReleaseCriticalSectionOnRecipientFuture(
    OperationContext* opCtx,
    const ShardId& recipientShardId,
    const NamespaceString& nss,
    const MigrationSessionId& sessionId) {
    const auto serviceContext = opCtx->getServiceContext();
    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    return ExecutorFuture<void>(executor).then([=] {
        ThreadClient tc("releaseRecipientCritSec",
                        serviceContext->getService(ClusterRole::ShardServer));
        auto uniqueOpCtx = tc->makeOperationContext();
        auto opCtx = uniqueOpCtx.get();

        const auto recipientShard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, recipientShardId));

        BSONObjBuilder builder;
        builder.append("_recvChunkReleaseCritSec",
                       NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        sessionId.append(&builder);
        builder.append(WriteConcernOptions::kWriteConcernField,
                       defaultMajorityWriteConcernDoNotUse().toBSON());
        const auto commandObj = builder.obj();

        sharding_util::retryIdempotentWorkAsPrimaryUntilSuccessOrStepdown(
            opCtx,
            "release migration critical section on recipient",
            [&](OperationContext* newOpCtx) {
                try {
                    const auto response = recipientShard->runCommand(
                        newOpCtx,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        DatabaseName::kAdmin,
                        commandObj,
                        Shard::RetryPolicy::kIdempotent);

                    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));
                } catch (const ExceptionFor<ErrorCodes::ShardNotFound>& exShardNotFound) {
                    LOGV2(5899106,
                          "Failed to release critical section on recipient",
                          "shardId"_attr = recipientShardId,
                          "sessionId"_attr = sessionId,
                          logAttrs(nss),
                          "error"_attr = exShardNotFound);
                }
            },
            Backoff(Seconds(1), Milliseconds::max()));
    });
}

void persistMigrationRecipientRecoveryDocument(
    OperationContext* opCtx, const MigrationRecipientRecoveryDocument& migrationRecipientDoc) {
    PersistentTaskStore<MigrationRecipientRecoveryDocument> store(
        NamespaceString::kMigrationRecipientsNamespace);
    try {
        store.add(opCtx, migrationRecipientDoc);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // Convert a DuplicateKey error to an anonymous error.
        uasserted(6064502,
                  str::stream()
                      << "While attempting to write migration recipient information for migration "
                      << ", found document with the same migration id. Attempted migration: "
                      << migrationRecipientDoc.toBSON());
    }
}

void deleteMigrationRecipientRecoveryDocument(OperationContext* opCtx, const UUID& migrationId) {
    // Before deleting the migration recipient recovery document, ensure that in the case of a
    // crash, the node will start-up from a configTime that is inclusive of the migration that was
    // committed during the critical section.
    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

    PersistentTaskStore<MigrationRecipientRecoveryDocument> store(
        NamespaceString::kMigrationRecipientsNamespace);
    store.remove(opCtx,
                 BSON(MigrationRecipientRecoveryDocument::kIdFieldName << migrationId),
                 defaultMajorityWriteConcernDoNotUse());
}

void resumeMigrationRecipientsOnStepUp(OperationContext* opCtx) {
    LOGV2_DEBUG(6064504, 2, "Starting migration recipient step-up recovery");

    unsigned long long ongoingMigrationRecipientsCount = 0;

    PersistentTaskStore<MigrationRecipientRecoveryDocument> store(
        NamespaceString::kMigrationRecipientsNamespace);

    store.forEach(
        opCtx,
        BSONObj{},
        [&opCtx, &ongoingMigrationRecipientsCount](const MigrationRecipientRecoveryDocument& doc) {
            invariant(ongoingMigrationRecipientsCount == 0,
                      str::stream()
                          << "Upon step-up a second migration recipient recovery document was found"
                          << redact(doc.toBSON()));
            ongoingMigrationRecipientsCount++;
            LOGV2_DEBUG(5899102,
                        3,
                        "Found ongoing migration recipient critical section on step-up",
                        "migrationRecipientCoordinatorDoc"_attr = redact(doc.toBSON()));

            const auto& nss = doc.getNss();

            // Register this receiveChunk on the ActiveMigrationsRegistry before completing step-up
            // to prevent a new migration from starting while a receiveChunk was ongoing. Wait for
            // any migrations that began in a previous term to complete if there are any.
            auto scopedReceiveChunk(
                uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerReceiveChunk(
                    opCtx,
                    nss,
                    doc.getRange(),
                    doc.getDonorShardIdForLoggingPurposesOnly(),
                    true /* waitForCompletionOfConflictingOps */)));

            const auto mdm = MigrationDestinationManager::get(opCtx);
            uassertStatusOK(
                mdm->restoreRecoveredMigrationState(opCtx, std::move(scopedReceiveChunk), doc));

            return true;
        });

    LOGV2_DEBUG(6064505,
                2,
                "Finished migration recipient step-up recovery",
                "ongoingRecipientCritSecCount"_attr = ongoingMigrationRecipientsCount);
}

void drainMigrationsPendingRecovery(OperationContext* opCtx) {
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);

    while (store.count(opCtx)) {
        store.forEach(opCtx, BSONObj(), [opCtx](const MigrationCoordinatorDocument& doc) {
            try {
                uassertStatusOK(
                    FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
                        opCtx, doc.getNss(), boost::none));
            } catch (DBException& ex) {
                ex.addContext(str::stream() << "Failed to recover pending migration for document "
                                            << doc.toBSON());
                throw;
            }
            return true;
        });
    }
}

SemiFuture<void> asyncRecoverMigrationUntilSuccessOrStepDown(OperationContext* opCtx,
                                                             const NamespaceString& nss) {
    return ExecutorFuture<void>{Grid::get(opCtx)->getExecutorPool()->getFixedExecutor()}
        .then([svcCtx{opCtx->getServiceContext()}, nss] {
            ThreadClient tc{"MigrationRecovery", svcCtx->getService(ClusterRole::ShardServer)};
            auto uniqueOpCtx{tc->makeOperationContext()};
            auto opCtx{uniqueOpCtx.get()};

            try {
                refreshFilteringMetadataUntilSuccess(opCtx, nss);
            } catch (const DBException& ex) {
                // This is expected in the event of a stepdown.
                LOGV2(6316100,
                      "Interrupted deferred migration recovery",
                      logAttrs(nss),
                      "error"_attr = redact(ex));
            }
        })
        .semi();
}

void drainMigrationsOnFcvDowngrade(OperationContext* opCtx) {
    // Chunk migrations that started under FCV >= 8.2 contain a
    // 'transfersFirstCollectionChunkToRecipient' field in their recovery document that (due to a
    // 'schema strictness' constraint present in previous binaries) is backwards incompatible.
    // Ensure that these operations get drained before completing the downgrade.

    // 1. If this shard is currently donating a chunk, abort the operation.
    auto& activeMigrationRegistry = ActiveMigrationsRegistry::get(opCtx);
    boost::optional<SharedSemiFuture<void>> abortingMigrationFuture;
    if (const auto collNameBeingTargetedByChunkDonation =
            activeMigrationRegistry.getActiveDonateChunkNss();
        collNameBeingTargetedByChunkDonation.has_value()) {
        const auto scopedCsr =
            CollectionShardingRuntime::acquireShared(opCtx, *collNameBeingTargetedByChunkDonation);
        if (auto msm = MigrationSourceManager::get(*scopedCsr)) {
            abortingMigrationFuture = msm->abort();
        }
    }
    if (abortingMigrationFuture) {
        // Drain the abortion after releasing the CSR.
        abortingMigrationFuture->get(opCtx);
    }

    // 2. If this shard has still to recover any migration containing with backwards
    // incompatible metadata, do that now.
    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    const auto incompatibleMigrationsFilter =
        BSON(MigrationCoordinatorDocument::kTransfersFirstCollectionChunkToRecipientFieldName
             << BSON("$ne" << BSONNULL));
    store.forEach(opCtx,
                  incompatibleMigrationsFilter,
                  [&](const MigrationCoordinatorDocument& migrationToRecover) {
                      migrationutil::asyncRecoverMigrationUntilSuccessOrStepDown(
                          opCtx, migrationToRecover.getNss())
                          .get(opCtx);
                      return true;  // keep iterating
                  });
}


}  // namespace migrationutil
}  // namespace mongo
