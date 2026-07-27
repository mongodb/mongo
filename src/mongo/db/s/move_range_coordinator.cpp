// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/move_range_coordinator.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/commit_chunk_migration_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/migration_coordinator_document_gen.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangInMoveRangeCoordinatorGlobalCatalogCommit);
MONGO_FAIL_POINT_DEFINE(hangInMoveRangeCoordinatorShardCatalogCommit);

// Errors the commit raises before it changes the global catalog. They prove the commit did not take
// effect, so the coordinator can safely abort. Other errors are ambiguous, but the commit is
// idempotent, so the coordinator keeps retrying instead.
//
// - ConflictingOperationInProgress: allowChunkOperations has been disabled concurrently.
// - ShardNotFound: the recipient shard no longer exists or has started draining.
// - BSONObjectTooLarge: the resulting changed-chunk set would exceed the BSON size limit, so the
//   commit is rejected before it changes the global catalog.
bool isFatalGlobalCatalogCommitError(const Status& status) {
    return status == ErrorCodes::ConflictingOperationInProgress ||
        status == ErrorCodes::ShardNotFound || status == ErrorCodes::BSONObjectTooLarge;
}

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

                _registerChunkOperationStarted(opCtx);
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

                // Persist the donor's pre-migration shard version for using it to build the global
                // catalog commit on kGlobalCatalogCommit.
                // We cannot read it during kGlobalCatalogCommit because, after a failover, it may
                // no longer reflect the pre-migration shard version.

                auto newDoc = MoveRangeCoordinatorDocument(_doc);
                newDoc.setDonorShardVersionPreMigration(
                    _migrationAttempt->donorShardVersionPreMigration());
                _updateStateDocument(opCtx, std::move(newDoc));

                // Mark the commit as in progress before issuing it, so a MigrationSourceManager
                // doesn't cancel the clone driver.
                _migrationAttempt->markCommitInProgress();
            }))
        .then(_buildPhaseHandler(Phase::kGlobalCatalogCommit,
                                 [this, anchor = shared_from_this()](OperationContext* opCtx) {
                                     LOGV2(12894208,
                                           "MoveRangeCoordinator executing kGlobalCatalogCommit",
                                           getCoordinatorLogAttrs());
                                     hangInMoveRangeCoordinatorGlobalCatalogCommit.pauseWhileSet(
                                         opCtx);

                                     auto changedChunks = _commitToGlobalCatalog(opCtx);

                                     // Persist the changed chunks so the shard catalog commit phase
                                     // can install them even after a failover, without contacting
                                     // the config server again.
                                     auto newDoc = MoveRangeCoordinatorDocument(_doc);
                                     newDoc.setChangedChunks(std::move(changedChunks));
                                     _updateStateDocument(opCtx, std::move(newDoc));
                                 }))
        .then(_buildPhaseHandler(Phase::kPostGlobalCatalogCommit,
                                 [this, anchor = shared_from_this()](OperationContext* opCtx) {
                                     LOGV2(
                                         12953604,
                                         "MoveRangeCoordinator executing kPostGlobalCatalogCommit",
                                         getCoordinatorLogAttrs());
                                     _postGlobalCatalogCommit(opCtx);
                                 }))
        .then(_buildPhaseHandler(
            Phase::kShardCatalogCommit,
            [this, anchor = shared_from_this(), executor, token](OperationContext* opCtx) {
                LOGV2(12795317,
                      "MoveRangeCoordinator executing kShardCatalogCommit",
                      getCoordinatorLogAttrs());
                hangInMoveRangeCoordinatorShardCatalogCommit.pauseWhileSet(opCtx);

                _commitToShardCatalog(opCtx, executor, token);
            }))
        .then(_buildPhaseHandler(Phase::kFinalizeMigration,
                                 [this, anchor = shared_from_this()](OperationContext* opCtx) {
                                     LOGV2(12894210,
                                           "MoveRangeCoordinator executing kFinalizeMigration",
                                           getCoordinatorLogAttrs());

                                     // Release the donor critical section, then
                                     // complete the migration (release the recipient critical
                                     // section, schedule range deletion, forget the on-disk
                                     // document) and signal joiners with success.
                                     _releaseCriticalSectionAndFinalize(opCtx, Status::OK());
                                 }))
        .onError([this, anchor = shared_from_this()](const Status& status) {
            const auto phase = _doc.getPhase();

            // The data clone cannot be resumed, so any error in these phases aborts the migration.
            if (phase <= Phase::kEnterCriticalSection) {
                if (phase == Phase::kUnset) {
                    // If nothing is persisted yet (kUnset) there is no document to record an abort
                    // reason on, so just propagate the error and let the framework retry.
                    return status;
                }
                auto opCtxHolder = makeOperationContext(/*deprioritizable=*/false);
                triggerCleanup(opCtxHolder.get(), status);
                MONGO_UNREACHABLE_TASSERT(12953605);
            }

            // During the commit, abort only for an error that is expected and proves the commit did
            // not take effect like a disabled allowChunkOperations flag.
            // For other errors keep retrying.
            if (phase == Phase::kGlobalCatalogCommit && isFatalGlobalCatalogCommitError(status)) {
                auto opCtxHolder = makeOperationContext(/*deprioritizable=*/false);
                triggerCleanup(opCtxHolder.get(), status);
                MONGO_UNREACHABLE_TASSERT(12953606);
            }

            // From kGlobalCatalogCommit onwards the migration is (or will be) committed, so the
            // coordinator keeps retrying to completion rather than aborting;
            // _mustAlwaysMakeProgress() keeps the retry loop running even for non-retryable errors.
            return status;
        })
        .thenRunOn(_cleanupExecutor)
        .onCompletion([this, anchor = shared_from_this()](Status status) {
            // Drop the migration attempt at the end of each execution attempt. Its cleanup runs
            // only once, so a retry (or the abort cleanup) completes from the persisted document
            // instead. The destructor is not enough because the coordinator can stay alive after a
            // stepdown until the node steps up again.
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

ChunkVersion MoveRangeCoordinator::MigrationAttempt::donorShardVersionPreMigration() const {
    return _msm->getDonorShardVersionPreMigration();
}

void MoveRangeCoordinator::MigrationAttempt::markCommitInProgress() {
    AlternativeClientRegion acr(_ownedClient);
    _msm->markCommitInProgress();
}

void MoveRangeCoordinator::MigrationAttempt::recordCommitSuccess(OperationContext* opCtx) {
    // Don't use AlternativeClientRegion here because we want to go ahead even if _ownedClient was
    // interrupted by an external client.
    _msm->recordCommitSuccess(opCtx);

    LOGV2(12697302,
          "Migration finished",
          "migrationId"_attr = _migrationId.toString(),
          "totalTimeMillis"_attr = _msm->getOpTimeMillis(),
          "docsCloned"_attr = _cloneMetrics.docsCloned(opCtx),
          "bytesCloned"_attr = _cloneMetrics.bytesCloned(opCtx),
          "cloneTime"_attr = _cloneMetrics.cloneTimeMillis(opCtx));
}

void MoveRangeCoordinator::MigrationAttempt::finalize(OperationContext* opCtx) {
    AlternativeClientRegion acr(_ownedClient);
    _msm->finishCommit();
}

std::vector<BSONObj> MoveRangeCoordinator::_commitToGlobalCatalog(OperationContext* opCtx) {
    auto doc = _getMigrationCoordinatorDocumentIfExists(opCtx);
    tassert(12953607,
            "MigrationCoordinator document must exist before committing to the global catalog",
            doc.has_value());
    tassert(12953608,
            "MigrationCoordinator document can't be kAborted at this point",
            doc->getDecision() != DecisionEnum::kAborted);

    tassert(12953609,
            "donorShardVersionPreMigration must have been persisted in kEnterCriticalSection",
            _doc.getDonorShardVersionPreMigration().has_value());

    const auto& range = doc->getRange();

    // Build the command from durable state so it can be re-sent after a failover. The command is
    // idempotent: if the chunk already belongs to the recipient it no-ops and returns the same
    // changed chunks.
    CommitMoveRangeRequest request(
        nss(),
        _request.getFromShard(),
        _request.getToShard(),
        MigratedChunkType(doc->getPreMigrationChunkVersion(), range.getMin(), range.getMax()),
        *_doc.getDonorShardVersionPreMigration());

    BSONObjBuilder builder;
    request.serialize(&builder);
    builder.append(WriteConcernOptions::kWriteConcernField,
                   defaultMajorityWriteConcernDoNotUse().toBSON());
    // Issue as a retryable write so the config server fences replays of stale requests.
    getNewSession(opCtx).serialize(&builder);


    const auto response = Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        DatabaseName::kAdmin,
        builder.obj(),
        Shard::RetryPolicy::kIdempotent);
    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));

    // Collect the chunks the commit changed so the shard-catalog commit phase can install exactly
    // those, avoiding a full metadata refresh on donor and recipient.
    auto reply = ConfigSvrCommitMoveRangeResponse::parse(
        response.getValue().response, IDLParserContext("ConfigSvrCommitMoveRangeResponse"));

    std::vector<BSONObj> changedChunks;
    changedChunks.reserve(reply.getChangedChunks().size());
    for (const auto& chunk : reply.getChangedChunks()) {
        changedChunks.push_back(chunk.getOwned());
    }
    return changedChunks;
}


void MoveRangeCoordinator::_postGlobalCatalogCommit(OperationContext* opCtx) {
    // The config commit succeeded. Run the post-commit bookkeeping
    auto doc = _getMigrationCoordinatorDocumentIfExists(opCtx);
    tassert(12953611, "MigrationCoordinatorDocument must exist", doc.has_value());

    // Emit the change-stream "chunk migrated" event.
    migrationutil::notifyChangeStreamsOnChunkMigrationCommitted(
        opCtx,
        nss(),
        doc->getCollectionUuid(),
        _request.getFromShard(),
        _request.getToShard(),
        doc->getTransfersFirstCollectionChunkToRecipient().value_or(false));

    // On the committing term, let the migration attempt do the in-memory bookkeeping: clear the
    // time-series bucket catalog, write the moveChunk.commit changelog, and record the committed
    // decision so finalize completes as committed. Skipped after a failover, where the bucket
    // catalog is rebuilt on the new primary and the changelog is best-effort.
    if (_migrationAttempt) {
        _migrationAttempt->recordCommitSuccess(opCtx);
    }

    // Persist the committed decision last: it is both the durable record that finalization reads
    // and the marker that fences the bookkeeping above on a retry.
    _persistMigrationDecision(opCtx, *doc, DecisionEnum::kCommitted);
}

void MoveRangeCoordinator::_commitToShardCatalog(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    // A committed migration always changes at least one chunk (the migrated chunk gets a new owning
    // shard and version). Reaching this phase without any persisted changed chunks means the global
    // catalog commit result was not persisted, which would silently install nothing.
    tassert(
        12953612,
        "Expected persisted changed chunks after committing the migration to the global catalog",
        _doc.getChangedChunks() && !_doc.getChangedChunks()->empty());

    const auto& changedChunks = *_doc.getChangedChunks();
    const auto& toShard = _request.getToShard();

    // The donor receives every changed chunk. It owns the control chunk (unless the donor is
    // donating the last chunk) and any split side chunks, must overwrite its now-stale ownership of
    // the migrated range, and appears in the migrated chunk's history, so holding all of them in
    // its shard catalog is valid.
    sharding_ddl_util::commitChunkOperationsMetadataToShardCatalog(
        opCtx,
        nss(),
        changedChunks,
        {ShardRef(_request.getFromShard())},
        getNewSession(opCtx),
        executor,
        token);

    // The recipient keeps only chunks it currently owns or has owned before (that is, chunks whose
    // history includes the recipient). This always includes the migrated chunk, because its history
    // is prepended with the recipient. It drops the control chunk and donor-owned side chunks,
    // which recipient never owned.
    std::vector<BSONObj> recipientChunks;
    for (const auto& chunkDoc : changedChunks) {
        const auto chunk =
            uassertStatusOK(ChunkType::parseFromConfigBSON(chunkDoc, OID(), Timestamp()));
        if (chunk.isOwnedNowOrHistoricallyBy(toShard)) {
            recipientChunks.push_back(chunkDoc);
        }
    }

    // The migrated chunk always lists the recipient as its current owner, so the recipient set can
    // never be empty.
    tassert(12953613,
            "Expected the recipient to receive at least the migrated chunk",
            !recipientChunks.empty());

    // If this is the recipient's first chunk, its local config/chunks entries may be stale,
    // so it cannot rely on its own CSR. Mark it as a first chunk so it installs the full
    // metadata from scratch.
    const bool recipientReceivingFirstChunk = [&] {
        const auto doc = _getMigrationCoordinatorDocumentIfExists(opCtx);
        tassert(13059501, "Migration document should exist at this point", doc.has_value());
        return doc->getTransfersFirstCollectionChunkToRecipient().value_or(false);
    }();
    sharding_ddl_util::commitChunkOperationsMetadataToShardCatalog(opCtx,
                                                                   nss(),
                                                                   std::move(recipientChunks),
                                                                   {ShardRef(toShard)},
                                                                   getNewSession(opCtx),
                                                                   executor,
                                                                   token,
                                                                   recipientReceivingFirstChunk);

    // A committed migration relocates exactly one range.
    ShardingStatistics::get(opCtx).chunkOperationsStatistics.registerMoveRangeChunksMoved(1);
}

void MoveRangeCoordinator::_persistMigrationDecision(OperationContext* opCtx,
                                                     MigrationCoordinatorDocument& doc,
                                                     DecisionEnum decision) {

    // Persist the decision for durability/recovery only, using the stat-free primitive. Do NOT use
    // persistCommitDecision / persistAbortDecision here: those bump countDonorMoveChunkCommitted /
    // countDonorMoveChunkAborted, and the migration is counted once when completeMigration()
    // persists the decision in the kFinalizeMigration phase. Using them here would double-count.
    doc.setDecision(decision);
    migrationutil::updateMigrationCoordinatorDoc(opCtx, doc);
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
    auto notifyRecoveryJobComplete = [this, opCtx] {
        if (_recoveredFromDisk) {
            const auto term = repl::ReplicationCoordinator::get(opCtx)->getTerm();
            RangeDeleterService::get(opCtx)->notifyRecoveryJobComplete(
                term, RecoveryJob::kMoveRangeCoordinator);
        }
    };

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
    // here. This path covers failover recovery (no live MigrationSourceManager) and the abort
    // cleanup driven by _cleanupOnAbort.
    auto doc = _getMigrationCoordinatorDocumentIfExists(opCtx);
    if (!doc) {
        // Nothing left to do: finalize() already completed it, or the migration aborted before a
        // coordinator document was ever persisted.
        notifyRecoveryJobComplete();
        return;
    }

    // The decision is persisted before this runs (by kGlobalCatalogCommit on commit, by
    // _cleanupOnAbort on abort), so any document that survives here always carries one.
    // completeMigration uses it to release the recipient critical section, schedule range deletion
    // and forget the document.
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

    const auto cleanupFuture = coordinator.completeMigration(opCtx, false);

    notifyRecoveryJobComplete();

    if (cleanupFuture && _request.getWaitForDelete()) {
        const auto cleanupStatus = cleanupFuture->getNoThrow(opCtx);
        if (!cleanupStatus.isOK()) {
            uasserted(
                ErrorCodes::OrphanedRangeCleanUpFailed,
                str::stream()
                    << "Migration committed but failed to clean up orphans for a waitForDelete "
                       "request due to: "
                    << redact(cleanupStatus));
        }
    }
}

void MoveRangeCoordinator::_releaseCriticalSectionAndFinalize(OperationContext* opCtx,
                                                              const Status& outcome) {
    // Release the donor critical section (no-op if never acquired / already released).
    ShardingRecoveryService::get(opCtx)->releaseRecoverableCriticalSection(
        opCtx,
        nss(),
        migrationutil::makeCriticalSectionReasonForMoveRange(_request, _doc.getMigrationId()),
        defaultMajorityWriteConcernDoNotUse(),
        ShardingRecoveryService::NoCustomAction(),
        false /* throwIfReasonDiffers */);

    // Complete the migration from its persisted decision: release the recipient critical section,
    // schedule range deletion / orphan cleanup, and forget the on-disk coordinator document.
    try {
        _finalizeMigration(opCtx);
    } catch (const ExceptionFor<ErrorCodes::OrphanedRangeCleanUpFailed>& ex) {
        // Only the committed waitForDelete path can raise this; the migration still succeeds. The
        // aborted path never schedules a cleanup future, so it never reaches here.
        LOGV2_WARNING(12795319,
                      "Migration committed but failed to clean up orphans for a "
                      "waitForDelete request",
                      logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                               "error"_attr = redact(ex.toStatus())});
    }

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    tassert(12723002,
            "Expected no MigrationCoordinator document on disk after completing migration",
            store.count(
                opCtx, BSON(MigrationCoordinatorDocument::kIdFieldName << _doc.getMigrationId())) ==
                0);

    _scopedDonateChunk->signalComplete(outcome);
}

ExecutorFuture<void> MoveRangeCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this(), status] {
        auto opCtxHolder = makeOperationContext(/*deprioritizable=*/false);
        auto* opCtx = opCtxHolder.get();

        LOGV2(12953610,
              "MoveRangeCoordinator cleaning up aborted migration",
              logv2::DynamicAttributes{getCoordinatorLogAttrs(), "reason"_attr = redact(status)});

        // Persist the aborted decision first so finalize reads it and it survives a failover
        // mid-cleanup. The shared teardown then releases the donor critical section, completes the
        // migration as aborted (releasing the recipient critical section, scheduling orphan
        // cleanup, forgetting the coordinator document) and signals joiners with the failure
        // status.
        if (auto doc = _getMigrationCoordinatorDocumentIfExists(opCtx)) {
            _persistMigrationDecision(opCtx, *doc, DecisionEnum::kAborted);
        }
        _releaseCriticalSectionAndFinalize(opCtx, status);
    });
}

}  // namespace mongo
