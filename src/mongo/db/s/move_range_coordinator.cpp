/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/move_range_coordinator.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/primary_only_service_helpers/all_shards_and_config_causality_barrier.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangInMoveRangeCoordinatorDetermineOutcome);
MONGO_FAIL_POINT_DEFINE(hangInMoveRangeCoordinatorShardCatalogCommit);

// Rebuild the wire-format ShardsvrMoveRange command from the request struct stored on the
// coordinator. Required because MigrationSourceManager::createMigrationSourceManager still takes a
// full ShardsvrMoveRange and serializes it for its "Starting chunk migration donation" log.
ShardsvrMoveRange buildShardsvrMoveRange(const NamespaceString& nss,
                                         const ShardsvrMoveRangeRequest& request) {
    ShardsvrMoveRange cmd{nss};
    cmd.setDbName(nss.dbName());
    cmd.setShardsvrMoveRangeRequest(request);
    return cmd;
}

ConnectionString makeDonorConnStr(OperationContext* opCtx,
                                  const ShardsvrMoveRangeRequest& request) {
    return uassertStatusOK(
               Grid::get(opCtx)->shardRegistry()->getShard(opCtx, request.getFromShard()))
        ->getConnString();
}

HostAndPort makeRecipientHost(OperationContext* opCtx, const ShardsvrMoveRangeRequest& request) {
    auto recipientShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, request.getToShard()));
    return uassertStatusOK(recipientShard->getTargeter()->findHost(
        opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}, {}));
}

}  // namespace

MoveRangeCoordinator::MoveRangeCoordinator(ShardingCoordinatorService* service,
                                           const BSONObj& initialStateDoc)
    : ChunkOperationShardingCoordinator(service, "MoveRangeCoordinator", initialStateDoc),
      _request(_doc.getShardsvrMoveRangeRequest()),
      _cleanupExecutor(service->getInstanceCleanupExecutor()) {}

void MoveRangeCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc =
        MoveRangeCoordinatorDocument::parse(doc, IDLParserContext("MoveRangeCoordinatorDocument"));

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getShardsvrMoveRangeRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another move range operation for namespace "
                          << nss().toStringForErrorMsg()
                          << " is being executed with different parameters: " << redact(selfReq)
                          << " vs " << redact(otherReq),
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

void MoveRangeCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

logv2::DynamicAttributes MoveRangeCoordinator::getCoordinatorLogAttrs() const {
    return logv2::DynamicAttributes{getBasicCoordinatorAttrs(),
                                    logAttrs(nss()),
                                    "migrationId"_attr = _doc.getMigrationId(),
                                    "fromShard"_attr = _request.getFromShard(),
                                    "toShard"_attr = _request.getToShard()};
}

bool MoveRangeCoordinator::isInCriticalSection(Phase phase) const {
    // From kEnterCriticalSection onwards a critical section is held. These phases must not be
    // deprioritized, otherwise execution-control load could starve them and prolong the critical
    // section. The clone work in kMigrate runs before the critical section, so it stays
    // deprioritizable.
    return phase >= Phase::kEnterCriticalSection;
}

ExecutorFuture<void> MoveRangeCoordinator::_acquireLocksAsync(
    OperationContext*,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken&) {
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtx = makeOperationContext(/*deprioritizable=*/false);
        // registerDonateChunk has a legacy invariant requiring this flag. The cancellation
        // token already ensures this opCtx is interrupted on stepdown, but the invariant
        // checks for the flag specifically.
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        // Bypass the registry's waitForRecovery() when called from the coordinator recovery
        // path (_recoveredFromDisk == true): _acquireLocksAsync runs before
        // _constructionCompletionPromise is set, so waiting for recovery would deadlock against
        // the ShardingCoordinatorService waiting for this coordinator to finish construction.
        auto bypass = _recoveredFromDisk
            ? boost::optional<ActiveMigrationsRegistry::BypassRecoveryWait>(
                  makeRegistryRecoveryBypass())
            : boost::none;
        _scopedDonateChunk =
            uassertStatusOK(ActiveMigrationsRegistry::get(opCtx.get())
                                .registerDonateChunk(opCtx.get(), nss(), _request, bypass));
        if (_recoveredFromDisk) {
            ShardingStatistics::get(opCtx.get())
                .unfinishedMigrationFromPreviousPrimary.fetchAndAdd(1);
        }
    });
}

void MoveRangeCoordinator::_releaseLocks(OperationContext*) {
    // Destroy the MigrationSourceManager before releasing the ActiveMigrationsRegistry gate.
    // Otherwise a subsequent migration on the same namespace could acquire the gate and construct a
    // second MigrationSourceManager while this one is still registered, tripping the invariant in
    // MigrationSourceManager::ScopedRegisterer.
    _migrationAttempt.reset();
    _scopedDonateChunk.reset();
}

ExecutorFuture<void> MoveRangeCoordinator::_joinExistingExecution(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    // A join token (mustExecute == false) means a legacy ScopedDonateChunk migration is still
    // in-flight for this namespace with identical parameters. Wait for it and propagate its result.
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtx = makeOperationContext(/*deprioritizable=*/false);
        _completeOnError = true;
        uassertStatusOK(_scopedDonateChunk->waitForCompletion(opCtx.get()));
    });
}

ExecutorFuture<void> MoveRangeCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    if (!_scopedDonateChunk->mustExecute()) {
        // MoveRangeCoordinators will automatically join (if they have the same parameters) or
        // serialize (otherwise) with other MoveRangeCoordinators. The only reason we'd need to join
        // here is if there is an ongoing migration using the legacy path.
        // TODO SERVER-127253: Remove this when the legacy path is removed.
        return _joinExistingExecution(executor);
    }
    return ExecutorFuture<void>(**executor)
        .then(_buildPhaseHandler(
            Phase::kMigrate,
            [this, anchor = shared_from_this(), token](OperationContext* opCtx) {
                LOGV2(
                    12894207, "MoveRangeCoordinator executing kMigrate", getCoordinatorLogAttrs());
                uassert(ErrorCodes::InterruptedDueToReplStateChange,
                        "MoveRangeCoordinator interrupted during data transfer",
                        _firstExecution);
                if (!_migrationAttempt) {
                    _migrationAttempt.emplace(
                        opCtx,
                        token,
                        nss(),
                        _request,
                        _doc.getWriteConcern().value_or(defaultMajorityWriteConcern()),
                        _doc.getMigrationId());
                }
                uassertStatusOK(_migrationAttempt->migrate(opCtx));
            }))
        .then(_buildPhaseHandler(
            Phase::kEnterCriticalSection,
            [this, anchor = shared_from_this()](OperationContext* opCtx) {
                LOGV2(12795314,
                      "MoveRangeCoordinator executing kEnterCriticalSection",
                      getCoordinatorLogAttrs());
                uassert(ErrorCodes::InterruptedDueToReplStateChange,
                        "MoveRangeCoordinator interrupted before entering the commit critical "
                        "section",
                        _firstExecution);
                tassert(12795311,
                        "Migrate and enterCriticalSection must only run during the same term",
                        _migrationAttempt.has_value());

                _migrationAttempt->promoteCriticalSection(opCtx);
            }))
        .then(_buildPhaseHandler(Phase::kGlobalCatalogCommit,
                                 [this, anchor = shared_from_this()](OperationContext* opCtx) {
                                     LOGV2(12894208,
                                           "MoveRangeCoordinator executing kGlobalCatalogCommit",
                                           getCoordinatorLogAttrs());
                                     uassert(ErrorCodes::InterruptedDueToReplStateChange,
                                             "MoveRangeCoordinator interrupted during commit",
                                             _firstExecution);
                                     tassert(
                                         12894200,
                                         "Migrate and commit must only run during the same term",
                                         _migrationAttempt.has_value());

                                     auto session = getNewSession(opCtx);
                                     _writeCommitAttempted(opCtx);
                                     uassertStatusOK(_migrationAttempt->commit(opCtx, session));
                                 }))
        .onCompletion([this, anchor = shared_from_this()](Status migrateStatus) {
            // The kMigrate, kEnterCriticalSection, and kGlobalCatalogCommit phases always run in
            // the same term, since the migration machinery aborts on any error. Persist the
            // resulting Status for these three phases in the coordinator document here. After
            // failover, those phases are skipped and this handler instead receives
            // InterruptedDueToReplStateChange. In either ways, the actual migration outcome is
            // resolved in the kDetermineOutcome phase.

            auto opCtx = makeOperationContext(/*deprioritizable=*/false);
            if (!migrateStatus.isOK()) {
                // If the state document was never persisted (e.g. the coordinator was
                // interrupted before the first phase write committed), there is nothing on disk to
                // record the result into. Propagate the (retryable) failure directly; attempting to
                // update a non-existent document would hit the tripwire 10644540.
                if (_doc.getPhase() == Phase::kUnset) {
                    uassertStatusOK(migrateStatus);
                }

                LOGV2_WARNING(
                    12697303,
                    "Error while doing moveChunk",
                    logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                             "error"_attr = redact(migrateStatus),
                                             "errorCode"_attr = migrateStatus.codeString()});
                if (migrateStatus.code() == ErrorCodes::LockTimeout) {
                    ShardingStatistics::get(opCtx.get())
                        .countDonorMoveChunkLockTimeout.addAndFetch(1);
                }
            }
            _writeMigrationResult(opCtx.get(), migrateStatus);
        })
        .then(_buildPhaseHandler(
            Phase::kDetermineOutcome,
            [this, anchor = shared_from_this(), executor, token](OperationContext* opCtx) {
                LOGV2(12894209,
                      "MoveRangeCoordinator executing kDetermineOutcome",
                      logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                               "commitAttempted"_attr = _doc.getCommitAttempted()});
                hangInMoveRangeCoordinatorDetermineOutcome.pauseWhileSet(opCtx);

                // On a retry or recovery, the config commit may have been issued by a previous
                // execution whose effect this node does not yet observe. Establish a causality
                // barrier so the placement read below reflects that commit; otherwise the outcome
                // could be wrongly determined as aborted. (Replay protection of the commit itself
                // is handled separately, via the session id.)
                if (!_firstExecution) {
                    AllShardsAndConfigCausalityBarrier barrier{**executor, token};
                    performCausalityBarrier(opCtx, barrier);
                }

                _determineAndRecordOutcome(opCtx, executor, token);
            }))
        .then(_buildPhaseHandler(
            Phase::kShardCatalogCommit,
            [this, anchor = shared_from_this(), executor, token](OperationContext* opCtx) {
                LOGV2(12795317,
                      "MoveRangeCoordinator executing kShardCatalogCommit",
                      getCoordinatorLogAttrs());
                hangInMoveRangeCoordinatorShardCatalogCommit.pauseWhileSet(opCtx);

                // Only commit to the shard catalog if the config-server commit actually happened.
                // An aborted or failed migration leaves an error result and must not touch the
                // local shard catalog.
                const auto result = _getMigrationResult();
                if (result.has_value() && result->isOK()) {
                    // Both critical sections are still held here, so the shard catalog commit on
                    // the donor and recipient is protected from concurrent migrations.
                    _commitToShardCatalog(opCtx, executor, token);
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kReleaseCriticalSectionOnDonor,
            [this, anchor = shared_from_this()](OperationContext* opCtx) {
                LOGV2(12795318,
                      "MoveRangeCoordinator executing kReleaseCriticalSectionOnDonor",
                      getCoordinatorLogAttrs());

                // Release the donor critical section now that the shard catalog has been committed.
                // The recipient critical section and the coordinator document are completed later
                // by kFinalizeMigration. throwIfReasonDiffers=false makes this a no-op if the
                // section was never acquired or was already released by a prior attempt.
                ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
                    opCtx,
                    nss(),
                    migrationutil::makeCriticalSectionReasonForMoveRange(_request,
                                                                         _doc.getMigrationId()),
                    defaultMajorityWriteConcernDoNotUse(),
                    ShardingRecoveryService::NoCustomAction(),
                    false /* throwIfReasonDiffers */);
            }))
        .then(_buildPhaseHandler(
            Phase::kFinalizeMigration,
            [this, anchor = shared_from_this(), executor](OperationContext* opCtx) {
                LOGV2(12894210,
                      "MoveRangeCoordinator executing kFinalizeMigration",
                      getCoordinatorLogAttrs());
                tassert(12894202,
                        "migrationResult must be set before kFinalizeMigration",
                        _getMigrationResult().has_value());
                const auto completionStatus = *_getMigrationResult();

                // Release the recipient critical section and complete the migration coordinator
                // (range deletion + forgetting the on-disk document). On the same term that ran the
                // commit the live MigrationSourceManager drives this; after a failover we
                // reconstruct the migration coordinator from its persisted document.
                try {
                    _finalizeMigration(opCtx);
                } catch (const ExceptionFor<ErrorCodes::OrphanedRangeCleanUpFailed>& ex) {
                    // The migration committed; only the _waitForDelete orphan cleanup failed.
                    // Legacy semantics return OK for any committed migration, so log and continue
                    // with the committed completionStatus.
                    LOGV2_WARNING(12795319,
                                  "Migration committed but failed to clean up orphans for a "
                                  "waitForDelete request",
                                  logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                           "error"_attr = redact(ex.toStatus())});
                }

                PersistentTaskStore<MigrationCoordinatorDocument> store(
                    NamespaceString::kMigrationCoordinatorsNamespace);
                tassert(
                    12723002,
                    "Expected no MigrationCoordinator document on disk after completing migration",
                    store.count(opCtx,
                                BSON(MigrationCoordinatorDocument::kIdFieldName
                                     << _doc.getMigrationId())) == 0);
                if (_recoveredFromDisk) {
                    const auto term = repl::ReplicationCoordinator::get(opCtx)->getTerm();
                    RangeDeleterService::get(opCtx)->notifyRecoveryJobComplete(term);
                }
                _scopedDonateChunk->signalComplete(completionStatus);
                _completeOnError = true;
                uassertStatusOK(completionStatus);
            }))
        .thenRunOn(_cleanupExecutor)
        .onCompletion([this, anchor = shared_from_this()](Status status) {
            // Ensure the MigrationSourceManager is released after all phases that need the live
            // migration attempt have completed. We cannot rely on the MoveRangeCoordinator
            // destructor because the coordinator may remain alive after stepdown until the node
            // steps up again.
            _migrationAttempt.reset();
            return status;
        });
}

MoveRangeCoordinator::MigrationAttempt::MigrationAttempt(OperationContext* opCtx,
                                                         CancellationToken token,
                                                         NamespaceString nss,
                                                         ShardsvrMoveRangeRequest request,
                                                         WriteConcernOptions writeConcern,
                                                         UUID migrationId)
    : _nss(std::move(nss)),
      _request(std::move(request)),
      _writeConcern(std::move(writeConcern)),
      _migrationId(std::move(migrationId)),
      _ownedClient(opCtx->getService()->makeClient("MoveRangeCoordinator::MigrationAttempt")),
      _ownedOpCtx(_ownedClient->makeOperationContext(),
                  token,
                  Grid::get(opCtx)->getExecutorPool()->getFixedExecutor()),
      _cloneMetrics(opCtx) {
    AlternativeClientRegion acr(_ownedClient);
    _msm = MigrationSourceManager::createMigrationSourceManager(
        _ownedOpCtx.get(),
        buildShardsvrMoveRange(_nss, _request),
        WriteConcernOptions{_writeConcern},
        makeDonorConnStr(_ownedOpCtx.get(), _request),
        makeRecipientHost(_ownedOpCtx.get(), _request),
        ManagementModeEnum::kMoveRangeCoordinator,
        _migrationId);
}

Status MoveRangeCoordinator::MigrationAttempt::migrate(OperationContext* opCtx) {
    if (_migrateResult)
        return *_migrateResult;

    AlternativeClientRegion acr(_ownedClient);
    try {
        _msm->startClone();
        _msm->awaitToCatchUp();
        _msm->enterCriticalSection();
        _msm->commitChunkOnRecipient();
        _migrateResult = Status::OK();
    } catch (const DBException& ex) {
        _migrateResult = ex.toStatus();
    }

    return *_migrateResult;
}

void MoveRangeCoordinator::MigrationAttempt::promoteCriticalSection(OperationContext* opCtx) {
    AlternativeClientRegion acr(_ownedClient);
    _msm->promoteCriticalSectionToBlockReads();
}

Status MoveRangeCoordinator::MigrationAttempt::commit(OperationContext* opCtx,
                                                      const OperationSessionInfo& session) {
    tassert(12894203, "commit() called before migrate() completed", _migrateResult.has_value());
    if (!_migrateResult->isOK())
        return *_migrateResult;

    if (_commitResult)
        return *_commitResult;

    AlternativeClientRegion acr(_ownedClient);
    try {
        // Any failure after this point may have committed on the config server, so the recovery
        // flow is required to determine the actual outcome.
        uassertStatusOK(_msm->commitMigrationToGlobalCatalog(session));

        // The config commit succeeded: emit the change stream event, clear the time-series bucket
        // catalog and record the committed decision. The critical sections are released later, by
        // the kReleaseCriticalSectionOnDonor and kFinalizeMigration phases, after the authoritative
        // shard catalog commit.
        _msm->recordCommitSuccess();

        LOGV2(12697302,
              "Migration finished",
              "migrationId"_attr = _migrationId.toString(),
              "totalTimeMillis"_attr = _msm->getOpTimeMillis(),
              "docsCloned"_attr = _cloneMetrics.docsCloned(opCtx),
              "bytesCloned"_attr = _cloneMetrics.bytesCloned(opCtx),
              "cloneTime"_attr = _cloneMetrics.cloneTimeMillis(opCtx));

        _commitResult = Status::OK();
    } catch (const DBException& ex) {
        _commitResult = ex.toStatus();
    }

    return *_commitResult;
}

void MoveRangeCoordinator::MigrationAttempt::finalize(OperationContext* opCtx) {
    AlternativeClientRegion acr(_ownedClient);
    _msm->finishCommit();
}

boost::optional<Status> MoveRangeCoordinator::_getMigrationResult() const {
    if (_doc.getMigrationSuccess().value_or(false))
        return Status::OK();
    if (const auto& failure = _doc.getMigrationFailure())
        return *failure;
    return boost::none;
}

void MoveRangeCoordinator::_writeMigrationResult(OperationContext* opCtx, Status result) {
    if (_doc.getMigrationSuccess().value_or(false) || _doc.getMigrationFailure()) {
        return;
    }
    auto newDoc = MoveRangeCoordinatorDocument(_doc);
    if (result.isOK()) {
        newDoc.setMigrationSuccess(true);
    } else {
        newDoc.setMigrationFailure(result);
    }
    _updateStateDocument(opCtx, std::move(newDoc));
}

void MoveRangeCoordinator::_writeCommitAttempted(OperationContext* opCtx) {
    auto newDoc = MoveRangeCoordinatorDocument(_doc);
    newDoc.setCommitAttempted(true);
    _updateStateDocument(opCtx, std::move(newDoc));
}

void MoveRangeCoordinator::_overwriteMigrationResultWithOk(OperationContext* opCtx) {
    tassert(12894204,
            "Cannot overwrite a migration result with success if no commit was attempted",
            _doc.getCommitAttempted().value_or(false));
    auto newDoc = MoveRangeCoordinatorDocument(_doc);
    newDoc.setMigrationSuccess(true);
    newDoc.setMigrationFailure(boost::none);
    _updateStateDocument(opCtx, std::move(newDoc));
}

void MoveRangeCoordinator::_commitToShardCatalog(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    // Donor and recipient are the only shards whose ownership of the moved range changed; both must
    // refresh their authoritative local shard catalog.
    std::set<ShardId> involvedShards{_request.getFromShard(), _request.getToShard()};


    // TODO (SERVER-129536): Replace the invalidation authoritative commit with an incremental
    // authoritative commit.
    sharding_ddl_util::commitCreateCollectionMetadataToShardCatalog(
        opCtx,
        nss(),
        {involvedShards.begin(), involvedShards.end()},
        getNewSession(opCtx),
        executor,
        token);
}

void MoveRangeCoordinator::_determineAndRecordOutcome(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {

    const auto result = _getMigrationResult();
    const bool committedKnown = result.has_value() && result->isOK();

    const auto outcome = _resolveMigrationOutcome(opCtx, result);

    LOGV2(12795321,
          "MoveRangeCoordinator resolved migration outcome",
          logv2::DynamicAttributes{
              getCoordinatorLogAttrs(),
              "outcome"_attr = (outcome == MigrationOutcome::kCommitted ? "committed" : "aborted"),
              // True when the commit result was already known on this term; false when the outcome
              // had to be determined authoritatively from the config server (failover or uncertain
              // commit).
              "commitResultKnown"_attr = committedKnown,
              "commitAttempted"_attr = _doc.getCommitAttempted(),
              "recoveredFromDisk"_attr = _recoveredFromDisk});

    // The coordinator document is needed to persist the resolved decision (so kFinalizeMigration
    // can complete the migration from a durable decision even after a failover into a later phase,
    // where there is no live MigrationSourceManager) and to emit the change-stream event below.
    auto doc = _getMigrationCoordinatorDocumentIfExists(opCtx);

    if (outcome == MigrationOutcome::kCommitted) {
        // Emit the change-stream "chunk migrated" event.
        if (doc.has_value()) {
            migrationutil::notifyChangeStreamsOnChunkMigrationCommitted(
                opCtx,
                nss(),
                doc->getCollectionUuid(),
                _request.getFromShard(),
                _request.getToShard(),
                doc->getTransfersFirstCollectionChunkToRecipient().value_or(false));
        }
        if (!committedKnown) {
            // The commit was resolved via the config server rather than the live commit() path, so
            // the success was not recorded yet. No-op if it was already recorded.
            _overwriteMigrationResultWithOk(opCtx);
        }
    } else {
        // The migration aborted. The onCompletion handler always records a (failure) result before
        // this phase runs, and a committed result never coexists with an aborted outcome, so a
        // failure result is already present.
        tassert(12795325,
                "an aborted migration must have a failure result recorded",
                result.has_value() && !result->isOK());
    }

    const auto decision =
        outcome == MigrationOutcome::kCommitted ? DecisionEnum::kCommitted : DecisionEnum::kAborted;
    _persistMigrationDecision(opCtx, doc, decision);
}

MoveRangeCoordinator::MigrationOutcome MoveRangeCoordinator::_resolveMigrationOutcome(
    OperationContext* opCtx, const boost::optional<Status>& result) {
    const bool committedKnown = result.has_value() && result->isOK();
    const bool commitAttempted = _doc.getCommitAttempted().value_or(false);

    // The outcome is known locally when the commit clearly succeeded (result OK) or was never
    // attempted on this term (a clean abort during migrate). It must be resolved from the config
    // server only when the commit was attempted but its result is uncertain (a failover, or an
    // ambiguous commit error).
    if (committedKnown) {
        return MigrationOutcome::kCommitted;
    }
    if (!commitAttempted && !_recoveredFromDisk) {
        tassert(12795312, "At this point migrationResult should have a value", result.has_value());
        return MigrationOutcome::kAborted;
    }
    LOGV2(12795320,
          "MoveRangeCoordinator determining migration outcome from the config server",
          logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                   "commitAttempted"_attr = _doc.getCommitAttempted(),
                                   "recoveredFromDisk"_attr = _recoveredFromDisk});
    return _determineMigrationOutcome(opCtx);
}

void MoveRangeCoordinator::_persistMigrationDecision(
    OperationContext* opCtx,
    boost::optional<MigrationCoordinatorDocument>& doc,
    DecisionEnum decision) {
    // The coordinator document may not exist yet (e.g. a clean abort before it was persisted),
    // so there is nothing to persist.
    if (!doc.has_value()) {
        return;
    }
    doc->setDecision(decision);
    // Persist the decision for durability/recovery only, using the stat-free primitive. Do NOT use
    // persistCommitDecision / persistAbortDecision here: those bump countDonorMoveChunkCommitted /
    // countDonorMoveChunkAborted, and the migration is counted once when completeMigration()
    // persists the decision in the kFinalizeMigration phase. Using them here would double-count.
    migrationutil::updateMigrationCoordinatorDoc(opCtx, *doc);
}

MoveRangeCoordinator::MigrationOutcome MoveRangeCoordinator::_determineMigrationOutcome(
    OperationContext* opCtx) {
    // Resolve the outcome from the authoritative global-catalog placement on the config server: if
    // the migrated range's min key now belongs to the recipient the commit happened, otherwise it
    // aborted.
    //
    // getMin() is always set here: the moveChunk/moveRange command resolves any find-key to an
    // explicit min/max before sending _shardsvrMoveRange to the shard.
    const auto& min = _request.getMoveRangeRequestBase().getMin().value();
    const auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss()));

    const auto owningShard = cm.findIntersectingChunkWithSimpleCollation(min).getShardId();
    return owningShard == _request.getToShard() ? MigrationOutcome::kCommitted
                                                : MigrationOutcome::kAborted;
}

boost::optional<MigrationCoordinatorDocument>
MoveRangeCoordinator::_getMigrationCoordinatorDocumentIfExists(OperationContext* opCtx) {
    boost::optional<MigrationCoordinatorDocument> result;

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.forEach(opCtx,
                  BSON(MigrationCoordinatorDocument::kIdFieldName << _doc.getMigrationId()),
                  [&](const MigrationCoordinatorDocument& doc) {
                      result = doc;
                      return false;  // Stop after the first (and only) match.
                  });
    return result;
}

void MoveRangeCoordinator::_finalizeMigration(OperationContext* opCtx) {
    if (_migrationAttempt) {
        LOGV2(12795323,
              "MoveRangeCoordinator finalizing migration via the live MigrationSourceManager",
              getCoordinatorLogAttrs());
        // Same term that ran the commit: the live MigrationSourceManager cleans up the cloner,
        // honours waitForDelete, and completes the migration coordinator from its in-memory
        // decision (releasing the recipient critical section, scheduling range deletion and
        // forgetting the document).
        _migrationAttempt->finalize(opCtx);
    }

    // Otherwise complete the migration coordinator from its persisted document. The two paths are
    // mutually exclusive on the document's existence: if finalize() above forgot it, none is found
    // here. This path covers failover recovery (no live MigrationSourceManager) and the same-term
    // uncertain-commit case (finalize() ran but had no in-memory decision).
    auto doc = _getMigrationCoordinatorDocumentIfExists(opCtx);
    if (!doc) {
        // Nothing left to do: finalize() already completed it, or the migration aborted before a
        // coordinator document was ever persisted.
        return;
    }

    // kDetermineOutcome persists the decision before advancing, so any document that survives here
    // always carries one. completeMigration uses it to release the recipient critical section,
    // schedule range deletion and forget the document.
    tassert(12795313,
            "Migration coordinator document must have a persisted decision before finalization",
            doc->getDecision().has_value());
    LOGV2(12795324,
          "MoveRangeCoordinator finalizing migration from the persisted coordinator document",
          logv2::DynamicAttributes{
              getCoordinatorLogAttrs(),
              "decision"_attr =
                  (doc->getDecision() == DecisionEnum::kCommitted ? "committed" : "aborted")});
    migrationutil::MigrationCoordinator coordinator(*doc);
    coordinator.setShardKeyPattern(
        rangedeletionutil::getShardKeyPatternFromRangeDeletionTask(opCtx, doc->getId()));
    coordinator.completeMigration(opCtx, false);
}

}  // namespace mongo
