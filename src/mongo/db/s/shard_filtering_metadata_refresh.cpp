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

#include "mongo/db/s/shard_filtering_metadata_refresh.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
#include <memory>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/read_only_catalog_cache_loader.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/shard_filtering_util.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/ensure_chunk_version_is_greater_than_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangInRefreshFilteringMetadataUntilSuccessInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible);

namespace {

MONGO_FAIL_POINT_DEFINE(skipDatabaseVersionMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(skipShardFilteringMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(hangInEnsureChunkVersionIsGreaterThanInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRecoverRefreshThread);

const auto getDecoration = ServiceContext::declareDecoration<FilteringMetadataCache>();

/**
 * Waits for a refresh operation to complete. Returns true if the refresh completed successfully,
 * false if it was aborted.
 *
 * All waits for metadata refresh operations should go through this code path, because it also
 * accounts for transactions and locking.
 */
bool waitForRefreshToComplete(OperationContext* opCtx, SharedSemiFuture<void> refresh) {
    try {
        refresh_util::waitForRefreshToComplete(opCtx, refresh);
    } catch (const ExceptionFor<ErrorCodes::DatabaseMetadataRefreshCanceled>&) {
        return false;
    } catch (const ExceptionFor<ErrorCodes::PlacementVersionRefreshCanceled>&) {
        return false;
    }
    return true;
}

/**
 * Blocking method, which will wait for any concurrent operations that could change the database
 * version to complete (namely critical section and concurrent onDbVersionMismatch invocations).
 *
 * Returns 'true' if there were concurrent operations that had to be joined (in which case all locks
 * will be dropped). If there were none, returns false and the locks continue to be held.
 */
template <typename ScopedDatabaseShardingState>
bool joinDbVersionOperation(OperationContext* opCtx,
                            boost::optional<Lock::DBLock>* dbLock,
                            boost::optional<ScopedDatabaseShardingState>* scopedDss) {
    invariant(dbLock->has_value());
    invariant(scopedDss->has_value());

    if (auto critSect =
            (**scopedDss)->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite)) {
        LOGV2_DEBUG(6697201,
                    2,
                    "Waiting for exit from the critical section",
                    logAttrs((**scopedDss)->getDbName()),
                    "reason"_attr = (**scopedDss)->getCriticalSectionReason());

        scopedDss->reset();
        dbLock->reset();

        uassertStatusOK(refresh_util::waitForCriticalSectionToComplete(opCtx, *critSect));
        return true;
    }

    if (auto refreshVersionFuture = (**scopedDss)->getDbMetadataRefreshFuture()) {
        LOGV2_DEBUG(6697202,
                    2,
                    "Waiting for completion of another database metadata refresh",
                    logAttrs((**scopedDss)->getDbName()));

        scopedDss->reset();
        dbLock->reset();

        waitForRefreshToComplete(opCtx, *refreshVersionFuture);

        return true;
    }

    return false;
}

/**
 * Blocking method, which will wait for any concurrent operations that could change the shard
 * version to complete (namely critical section and concurrent onCollectionPlacementVersionMismatch
 * invocations).
 *
 * Returns 'true' if there were concurrent operations that had to be joined (in which case all locks
 * will be dropped). If there were none, returns false and the locks continue to be held.
 */
template <typename ScopedCSR>
bool joinCollectionPlacementVersionOperation(OperationContext* opCtx,
                                             boost::optional<Lock::DBLock>* dbLock,
                                             boost::optional<Lock::CollectionLock>* collLock,
                                             boost::optional<ScopedCSR>* scopedCsr) {
    invariant(dbLock->has_value());
    invariant(collLock->has_value());
    invariant(scopedCsr->has_value());

    if (auto critSecSignal =
            (**scopedCsr)
                ->getCriticalSectionSignal(opCtx, ShardingMigrationCriticalSection::kWrite)) {
        scopedCsr->reset();
        collLock->reset();
        dbLock->reset();

        uassertStatusOK(refresh_util::waitForCriticalSectionToComplete(opCtx, *critSecSignal));

        return true;
    }

    if (auto inRecoverOrRefresh = (**scopedCsr)->getPlacementVersionRecoverRefreshFuture(opCtx)) {
        scopedCsr->reset();
        collLock->reset();
        dbLock->reset();

        waitForRefreshToComplete(opCtx, *inRecoverOrRefresh);

        return true;
    }

    return false;
}

void ensureChunkVersionIsGreaterThan(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const UUID& collUUID,
                                     const ChunkRange& range,
                                     const ChunkVersion& preMigrationChunkVersion) {
    ConfigsvrEnsureChunkVersionIsGreaterThan ensureChunkVersionIsGreaterThanRequest;
    ensureChunkVersionIsGreaterThanRequest.setDbName(DatabaseName::kAdmin);
    ensureChunkVersionIsGreaterThanRequest.setMinKey(range.getMin());
    ensureChunkVersionIsGreaterThanRequest.setMaxKey(range.getMax());
    ensureChunkVersionIsGreaterThanRequest.setVersion(preMigrationChunkVersion);
    ensureChunkVersionIsGreaterThanRequest.setNss(nss);
    ensureChunkVersionIsGreaterThanRequest.setCollectionUUID(collUUID);
    generic_argument_util::setMajorityWriteConcern(ensureChunkVersionIsGreaterThanRequest);
    const auto ensureChunkVersionIsGreaterThanRequestBSON =
        ensureChunkVersionIsGreaterThanRequest.toBSON();

    hangInEnsureChunkVersionIsGreaterThanInterruptible.pauseWhileSet(opCtx);

    const auto ensureChunkVersionIsGreaterThanResponse =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            ensureChunkVersionIsGreaterThanRequestBSON,
            Shard::RetryPolicy::kIdempotent);
    const auto ensureChunkVersionIsGreaterThanStatus =
        Shard::CommandResponse::getEffectiveStatus(ensureChunkVersionIsGreaterThanResponse);

    uassertStatusOK(ensureChunkVersionIsGreaterThanStatus);

    if (hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible.shouldFail()) {
        hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible.pauseWhileSet();
        uasserted(ErrorCodes::InternalError,
                  "simulate an error response for _configsvrEnsureChunkVersionIsGreaterThan");
    }
}
}  // namespace

void FilteringMetadataCache::init(ServiceContext* serviceCtx,
                                  std::shared_ptr<ShardServerCatalogCacheLoader> loader,
                                  bool isPrimary) {
    invariant(serviceCtx->getService(ClusterRole::ShardServer) != nullptr);

    loader->initializeReplicaSetRole(isPrimary);

    auto decoration = FilteringMetadataCache::get(serviceCtx);
    invariant(decoration->_loader == nullptr);
    decoration->_loader = loader;

    // (Ignore FCV check): this feature flag is not FCV-gated.
    if (feature_flags::gDualCatalogCache.isEnabledAndIgnoreFCVUnsafe()) {
        decoration->_cache =
            std::make_unique<CatalogCache>(serviceCtx, loader, "FilteringMetadata"_sd);
    }
}

void FilteringMetadataCache::initForTesting(ServiceContext* serviceCtx,
                                            std::shared_ptr<ShardServerCatalogCacheLoader> loader) {
    invariant(serviceCtx->getService(ClusterRole::ShardServer) != nullptr);

    auto decoration = FilteringMetadataCache::get(serviceCtx);
    invariant(decoration->_loader == nullptr);
    decoration->_loader = loader;
}

FilteringMetadataCache* FilteringMetadataCache::get(ServiceContext* serviceCtx) {
    return &getDecoration(serviceCtx);
}

FilteringMetadataCache* FilteringMetadataCache::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void FilteringMetadataCache::shutDown() {
    if (_cache)
        _cache->shutDownAndJoin();
}

void FilteringMetadataCache::onStepDown() {
    // TODO (SERVER-84243): Remove this once FilteringMetadataCache is always instantiated with a
    // SSCCL as part of its constructor.
    tassert(9539100,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->onStepDown();
}

void FilteringMetadataCache::onStepUp() {
    // TODO (SERVER-84243): Remove this once FilteringMetadataCache is always instantiated with a
    // SSCCL as part of its constructor.
    tassert(9539101,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->onStepUp();
}

void FilteringMetadataCache::onReplicationRollback() {
    // TODO (SERVER-84243): Remove this once FilteringMetadataCache is always instantiated with a
    // SSCCL as part of its constructor.
    tassert(9539102,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->onReplicationRollback();
}

void FilteringMetadataCache::notifyOfCollectionRefreshEndMarkerSeen(const NamespaceString& nss,
                                                                    const Timestamp& commitTime) {
    // TODO (SERVER-84243): Remove this once FilteringMetadataCache is always instantiated with a
    // SSCCL as part of its constructor.
    tassert(9539103,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->notifyOfCollectionRefreshEndMarkerSeen(nss, commitTime);
}

void FilteringMetadataCache::waitForCollectionFlush(OperationContext* opCtx,
                                                    const NamespaceString& nss) {
    // TODO (SERVER-84243): Remove this once FilteringMetadataCache is always instantiated with a
    // SSCCL as part of its constructor.
    tassert(9539104,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->waitForCollectionFlush(opCtx, nss);
}

void FilteringMetadataCache::waitForDatabaseFlush(OperationContext* opCtx,
                                                  const DatabaseName& dbName) {
    // TODO (SERVER-84243): Remove this once FilteringMetadataCache is always instantiated with a
    // SSCCL as part of its constructor.
    tassert(9539105,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->waitForDatabaseFlush(opCtx, dbName);
}

void FilteringMetadataCache::report(BSONObjBuilder* builder) const {
    if (_cache)
        _cache->report(builder);
}

Status FilteringMetadataCache::onCollectionPlacementVersionMismatch(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<ChunkVersion> chunkVersionReceived) noexcept {
    try {
        _onCollectionPlacementVersionMismatch(opCtx, nss, chunkVersionReceived);
        return Status::OK();
    } catch (const DBException& ex) {
        LOGV2(22062,
              "Failed to refresh metadata for collection",
              logAttrs(nss),
              "error"_attr = redact(ex));
        return ex.toStatus();
    }
}

void FilteringMetadataCache::forceCollectionPlacementRefresh(OperationContext* opCtx,
                                                             const NamespaceString& nss) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    // TODO (SERVER-97261): remove the Grid's CatalogCache usages once 9.0 becomes last LTS.
    const auto catalogCache = _cache ? _cache.get() : Grid::get(opCtx)->catalogCache();
    const auto cm =
        uassertStatusOK(catalogCache->getCollectionPlacementInfoWithRefresh(opCtx, nss));

    if (!cm.hasRoutingTable()) {
        Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss);
        scopedCsr->setFilteringMetadata(opCtx, CollectionMetadata());
        return;
    }

    auto isCollectionPlacementUpToDate = [&](boost::optional<CollectionMetadata> optMetadata) {
        if (optMetadata) {
            const auto& metadata = *optMetadata;
            if (metadata.hasRoutingTable() &&
                (cm.getVersion().isOlderOrEqualThan(metadata.getCollPlacementVersion()))) {
                LOGV2_DEBUG(22063,
                            1,
                            "Skipping metadata refresh because collection already is up-to-date",
                            logAttrs(nss),
                            "latestCollectionPlacementVersion"_attr =
                                metadata.getCollPlacementVersion(),
                            "refreshedCollectionPlacementVersion"_attr = cm.getVersion());
                return true;
            }
        }
        return false;
    };

    // Optimistic check with IS lock to avoid threads piling up on the collection X lock below.
    {
        Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IS);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
        const auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
        if (isCollectionPlacementUpToDate(
                scopedCsr->getCurrentMetadataIfKnown() /* optMetadata */)) {
            return;
        }
    }

    // Exclusive collection lock needed since we're now changing the metadata.
    Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
    auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss);
    if (isCollectionPlacementUpToDate(scopedCsr->getCurrentMetadataIfKnown() /* optMetadata */)) {
        return;
    }

    CollectionMetadata metadata(cm, ShardingState::get(opCtx)->shardId());
    scopedCsr->setFilteringMetadata(opCtx, std::move(metadata));
}

Status FilteringMetadataCache::onDbVersionMismatch(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    boost::optional<DatabaseVersion> clientDbVersion) noexcept {
    try {
        _onDbVersionMismatch(opCtx, dbName, clientDbVersion);
        return Status::OK();
    } catch (const DBException& ex) {
        LOGV2(22065,
              "Failed to refresh databaseVersion",
              "db"_attr = dbName,
              "error"_attr = redact(ex));
        return ex.toStatus();
    }
}

CollectionMetadata FilteringMetadataCache::_forceGetCurrentMetadata(OperationContext* opCtx,
                                                                    const NamespaceString& nss) {
    try {
        // TODO (SERVER-97261): remove the Grid's CatalogCache usages once 9.0 becomes last LTS.
        const auto catalogCache = _cache ? _cache.get() : Grid::get(opCtx)->catalogCache();
        const auto cm =
            uassertStatusOK(catalogCache->getCollectionPlacementInfoWithRefresh(opCtx, nss));

        if (!cm.hasRoutingTable()) {
            return CollectionMetadata();
        }

        return CollectionMetadata(cm, ShardingState::get(opCtx)->shardId());
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        LOGV2(505070,
              "Namespace not found, collection may have been dropped",
              logAttrs(nss),
              "error"_attr = redact(ex));
        return CollectionMetadata();
    }
}

void FilteringMetadataCache::_recoverMigrationCoordinations(OperationContext* opCtx,
                                                            NamespaceString nss,
                                                            CancellationToken cancellationToken) {
    LOGV2_DEBUG(4798501, 2, "Starting migration recovery", logAttrs(nss));

    unsigned migrationRecoveryCount = 0;

    PersistentTaskStore<MigrationCoordinatorDocument> store(
        NamespaceString::kMigrationCoordinatorsNamespace);
    store.forEach(
        opCtx,
        BSON(MigrationCoordinatorDocument::kNssFieldName
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
        [&](const MigrationCoordinatorDocument& doc) {
            LOGV2_DEBUG(4798502,
                        2,
                        "Recovering migration",
                        "migrationCoordinatorDocument"_attr = redact(doc.toBSON()));

            // Ensure there is only one migrationCoordinator document to be recovered for this
            // namespace.
            invariant(++migrationRecoveryCount == 1,
                      str::stream() << "Found more then one migration to recover for namespace '"
                                    << nss.toStringForErrorMsg() << "'");

            // Create a MigrationCoordinator to complete the coordination.
            migrationutil::MigrationCoordinator coordinator(doc);

            if (doc.getDecision()) {
                // The decision is already known.
                coordinator.setShardKeyPattern(
                    rangedeletionutil::getShardKeyPatternFromRangeDeletionTask(opCtx, doc.getId()));
                coordinator.completeMigration(opCtx);
                return true;
            }

            // The decision is not known. Recover the decision from the config server.

            ensureChunkVersionIsGreaterThan(opCtx,
                                            doc.getNss(),
                                            doc.getCollectionUuid(),
                                            doc.getRange(),
                                            doc.getPreMigrationChunkVersion());

            hangInRefreshFilteringMetadataUntilSuccessInterruptible.pauseWhileSet(opCtx);

            auto currentMetadata = _forceGetCurrentMetadata(opCtx, doc.getNss());

            if (hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .shouldFail()) {
                hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .pauseWhileSet();
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response for forceGetCurrentMetadata");
            }

            auto setFilteringMetadata = [&opCtx, &currentMetadata, &doc, &cancellationToken]() {
                AutoGetDb autoDb(opCtx, doc.getNss().dbName(), MODE_IX);
                Lock::CollectionLock collLock(opCtx, doc.getNss(), MODE_IX);
                auto scopedCsr =
                    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                        opCtx, doc.getNss());

                auto optMetadata = scopedCsr->getCurrentMetadataIfKnown();
                invariant(!optMetadata);

                if (!cancellationToken.isCanceled()) {
                    scopedCsr->setFilteringMetadata(opCtx, std::move(currentMetadata));
                }
            };

            if (!currentMetadata.isSharded() ||
                !currentMetadata.uuidMatches(doc.getCollectionUuid())) {
                if (!currentMetadata.isSharded()) {
                    LOGV2(4798503,
                          "During migration recovery the collection was discovered to have been "
                          "dropped."
                          "Deleting the range deletion tasks on the donor and the recipient "
                          "as well as the migration coordinator document on this node",
                          "migrationCoordinatorDocument"_attr = redact(doc.toBSON()));
                } else {
                    // UUID don't match
                    LOGV2(4798504,
                          "During migration recovery the collection was discovered to have been "
                          "dropped and recreated. Collection has a UUID that "
                          "does not match the one in the migration coordinator "
                          "document. Deleting the range deletion tasks on the donor and "
                          "recipient as well as the migration coordinator document on this node",
                          "migrationCoordinatorDocument"_attr = redact(doc.toBSON()),
                          "refreshedMetadataUUID"_attr =
                              currentMetadata.getChunkManager()->getUUID(),
                          "coordinatorDocumentUUID"_attr = doc.getCollectionUuid(),
                          logAttrs(doc.getNss()));
                }

                // TODO SERVER-77472: remove this once we are sure all operations persist the config
                // time after a collection drop. Since the collection has been dropped, persist
                // config time inclusive of the drop collection event before deleting leftover
                // migration metadata. This will ensure that in case of stepdown the new primary
                // won't read stale data from config server and think that the sharded collection
                // still exists.
                VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);

                rangedeletionutil::deleteRangeDeletionTaskOnRecipient(opCtx,
                                                                      doc.getRecipientShardId(),
                                                                      doc.getCollectionUuid(),
                                                                      doc.getRange(),
                                                                      doc.getId());
                rangedeletionutil::deleteRangeDeletionTaskLocally(
                    opCtx, doc.getCollectionUuid(), doc.getRange());
                coordinator.forgetMigration(opCtx);
                setFilteringMetadata();
                return true;
            }

            // Note this should only extend the range boundaries (if there has been a shard key
            // refine since the migration began) and never truncate them.
            auto chunkRangeToCompareToMetadata =
                migrationutil::extendOrTruncateBoundsForMetadata(currentMetadata, doc.getRange());
            if (currentMetadata.keyBelongsToMe(chunkRangeToCompareToMetadata.getMin())) {
                coordinator.setMigrationDecision(DecisionEnum::kAborted);
            } else {
                coordinator.setMigrationDecision(DecisionEnum::kCommitted);
                if (!currentMetadata.getChunkManager()->getVersion(doc.getDonorShardId()).isSet()) {
                    migrationutil::notifyChangeStreamsOnDonorLastChunk(
                        opCtx, doc.getNss(), doc.getDonorShardId(), doc.getCollectionUuid());
                }
            }

            coordinator.setShardKeyPattern(KeyPattern(currentMetadata.getKeyPattern()));
            coordinator.completeMigration(opCtx);
            setFilteringMetadata();
            return true;
        });
}


Status FilteringMetadataCache::_refreshDbMetadata(OperationContext* opCtx,
                                                  const DatabaseName& dbName,
                                                  CancellationToken cancellationToken) {
    ScopeGuard resetRefreshFutureOnError([&] {
        // TODO (SERVER-71444): Fix to be interruptible or document exception.
        // Can be uninterruptible because the work done under it can never block.
        UninterruptibleLockGuard noInterrupt(opCtx);  // NOLINT.
        auto scopedDss = DatabaseShardingState::acquireExclusive(opCtx, dbName);
        scopedDss->resetDbMetadataRefreshFuture();
    });

    // TODO (SERVER-97261): remove the Grid's CatalogCache usages once 9.0 becomes last LTS.
    const auto catalogCache = _cache ? _cache.get() : Grid::get(opCtx)->catalogCache();

    // Force a refresh of the cached database metadata from the config server.
    catalogCache->onStaleDatabaseVersion(dbName, boost::none /* wantedVersion */);
    const auto swDbMetadata = catalogCache->getDatabase(opCtx, dbName);

    // Before setting the database metadata, exit early if the database version received by the
    // config server is not newer than the cached one. This is a best-effort optimization to reduce
    // the number of possible threads convoying on the exclusive lock below.
    {
        Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
        const auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx, dbName);

        const auto cachedDbVersion = scopedDss->getDbVersion(opCtx);
        if (swDbMetadata.isOK() && swDbMetadata.getValue()->getVersion() <= cachedDbVersion) {
            LOGV2_DEBUG(7079300,
                        2,
                        "Skip setting cached database metadata as there are no updates",
                        logAttrs(dbName),
                        "cachedDbVersion"_attr = *cachedDbVersion,
                        "refreshedDbVersion"_attr = swDbMetadata.getValue()->getVersion());

            return Status::OK();
        }
    }

    Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, dbName);
    if (!cancellationToken.isCanceled()) {
        if (swDbMetadata.isOK()) {
            // Set the refreshed database metadata in the local catalog.
            scopedDss->setDbInfo(opCtx, *swDbMetadata.getValue());
        } else if (swDbMetadata == ErrorCodes::NamespaceNotFound) {
            // The database has been dropped, so clear its metadata in the local catalog.
            scopedDss->clearDbInfo(opCtx, false /* cancelOngoingRefresh */);
        }
    }

    // Reset the future reference to allow any other thread to refresh the database metadata.
    scopedDss->resetDbMetadataRefreshFuture();
    resetRefreshFutureOnError.dismiss();

    return swDbMetadata.getStatus();
}

SharedSemiFuture<void> FilteringMetadataCache::_recoverRefreshDbVersion(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const CancellationToken& cancellationToken) {
    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    return ExecutorFuture<void>(executor)
        .then([=,
               this,
               serviceCtx = opCtx->getServiceContext(),
               forwardableOpMetadata = ForwardableOperationMetadata(opCtx)] {
            ThreadClient tc("DbMetadataRefreshThread",
                            serviceCtx->getService(ClusterRole::ShardServer));
            const auto opCtxHolder =
                CancelableOperationContext(tc->makeOperationContext(), cancellationToken, executor);
            auto opCtx = opCtxHolder.get();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // Forward `users` and `roles` attributes from the original request.
            forwardableOpMetadata.setOn(opCtx);

            LOGV2_DEBUG(6697203, 2, "Started database metadata refresh", logAttrs(dbName));

            return _refreshDbMetadata(opCtx, dbName, cancellationToken);
        })
        .onCompletion([=](Status status) {
            uassert(ErrorCodes::DatabaseMetadataRefreshCanceled,
                    str::stream() << "Canceled metadata refresh for database "
                                  << dbName.toStringForErrorMsg(),
                    !cancellationToken.isCanceled());

            if (status.isOK() || status == ErrorCodes::NamespaceNotFound) {
                LOGV2(6697204, "Refreshed database metadata", logAttrs(dbName));
                return Status::OK();
            }

            LOGV2_ERROR(6697205,
                        "Failed database metadata refresh",
                        logAttrs(dbName),
                        "error"_attr = redact(status));
            return status;
        })
        .semi()
        .share();
}

void FilteringMetadataCache::_onDbVersionMismatch(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    boost::optional<DatabaseVersion> receivedDbVersion) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    using namespace fmt::literals;
    tassert(ErrorCodes::IllegalOperation,
            "Can't check version of {} database"_format(dbName.toStringForErrorMsg()),
            !dbName.isAdminDB() && !dbName.isConfigDB());

    Timer t{};
    ScopeGuard finishTiming([&] {
        CurOp::get(opCtx)->debug().databaseVersionRefreshMillis += Milliseconds(t.millis());
    });

    LOGV2_DEBUG(6697200,
                2,
                "Handle database version mismatch",
                "db"_attr = dbName,
                "receivedDbVersion"_attr = receivedDbVersion);

    while (true) {
        boost::optional<SharedSemiFuture<void>> dbMetadataRefreshFuture;

        {
            boost::optional<Lock::DBLock> dbLock;
            dbLock.emplace(opCtx, dbName, MODE_IS);

            if (receivedDbVersion) {
                auto scopedDss = boost::make_optional(
                    DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx, dbName));

                if (joinDbVersionOperation(opCtx, &dbLock, &scopedDss)) {
                    // Waited for another thread to exit from the critical section or to complete an
                    // ongoing refresh, so reacquire the locks.
                    continue;
                }

                // From now until the end of this block [1] no thread is in the critical section or
                // can enter it (would require to X-lock the database) and [2] no metadata refresh
                // is in progress or can start (would require to exclusive lock the DSS).
                // Therefore, the database version can be accessed safely.

                const auto wantedVersion = (*scopedDss)->getDbVersion(opCtx);
                if (receivedDbVersion <= wantedVersion) {
                    // No need to refresh the database metadata as the wanted version is newer
                    // than the one received.
                    return;
                }
            }

            if (MONGO_unlikely(skipDatabaseVersionMetadataRefresh.shouldFail())) {
                return;
            }

            auto scopedDss = boost::make_optional(
                DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, dbName));

            if (joinDbVersionOperation(opCtx, &dbLock, &scopedDss)) {
                // Waited for another thread to exit from the critical section or to complete an
                // ongoing refresh, so reacquire the locks.
                continue;
            }

            // From now until the end of this block [1] no thread is in the critical section or can
            // enter it (would require to X-lock the database) and [2] this is the only metadata
            // refresh in progress (holding the exclusive lock on the DSS).
            // Therefore, the future to refresh the database metadata can be set.

            CancellationSource cancellationSource;
            CancellationToken cancellationToken = cancellationSource.token();
            (*scopedDss)
                ->setDbMetadataRefreshFuture(
                    _recoverRefreshDbVersion(opCtx, dbName, cancellationToken),
                    std::move(cancellationSource));
            dbMetadataRefreshFuture = (*scopedDss)->getDbMetadataRefreshFuture();
        }

        // No other metadata refresh for this database can run in parallel. If another thread enters
        // the critical section, the ongoing refresh would be interrupted and subsequently
        // re-queued.
        if (!waitForRefreshToComplete(opCtx, *dbMetadataRefreshFuture)) {
            // The refresh was canceled by a 'clearFilteringMetadata'. Retry the refresh.
            continue;
        }

        break;
    }
}

SharedSemiFuture<void> FilteringMetadataCache::_recoverRefreshCollectionPlacementVersion(
    ServiceContext* serviceContext,
    const NamespaceString& nss,
    bool runRecover,
    CancellationToken cancellationToken) {
    auto executor = Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor();
    return ExecutorFuture<void>(executor)
        .then([=, this] {
            ThreadClient tc("RecoverRefreshThread",
                            serviceContext->getService(ClusterRole::ShardServer));

            if (MONGO_unlikely(hangInRecoverRefreshThread.shouldFail())) {
                hangInRecoverRefreshThread.pauseWhileSet();
            }

            const auto opCtxHolder =
                CancelableOperationContext(tc->makeOperationContext(), cancellationToken, executor);
            auto const opCtx = opCtxHolder.get();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            boost::optional<CollectionMetadata> currentMetadataToInstall;

            ScopeGuard resetRefreshFutureOnError([&] {
                // TODO (SERVER-71444): Fix to be interruptible or document exception.
                // Can be uninterruptible because the work done under it can never block
                UninterruptibleLockGuard noInterrupt(opCtx);  // NOLINT.
                auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
                scopedCsr->resetPlacementVersionRecoverRefreshFuture();
            });

            if (runRecover) {
                auto* const replCoord = repl::ReplicationCoordinator::get(opCtx);
                if (!replCoord->getSettings().isReplSet() ||
                    replCoord->getMemberState().primary()) {
                    _recoverMigrationCoordinations(opCtx, nss, cancellationToken);
                }
            }

            auto currentMetadata = _forceGetCurrentMetadata(opCtx, nss);

            if (currentMetadata.hasRoutingTable()) {
                // Abort and join any ongoing migration if migrations are disallowed for the
                // namespace.
                if (!currentMetadata.allowMigrations()) {
                    boost::optional<SharedSemiFuture<void>> waitForMigrationAbort;
                    {
                        Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IS);
                        Lock::CollectionLock collLock(opCtx, nss, MODE_IS);

                        const auto scopedCsr =
                            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx,
                                                                                              nss);
                        // There is no need to abort an ongoing migration if the refresh is
                        // cancelled.
                        if (!cancellationToken.isCanceled()) {
                            if (auto msm = MigrationSourceManager::get(*scopedCsr)) {
                                waitForMigrationAbort.emplace(msm->abort());
                            }
                        }
                    }

                    if (waitForMigrationAbort) {
                        waitForMigrationAbort->get(opCtx);
                    }
                }

                // If the collection metadata after a refresh has 'reshardingFields', then pass it
                // to the resharding subsystem to process.
                const auto& reshardingFields = currentMetadata.getReshardingFields();
                if (reshardingFields) {
                    resharding::processReshardingFieldsForCollection(
                        opCtx, nss, currentMetadata, *reshardingFields);
                }
            }

            boost::optional<SharedSemiFuture<void>> waitForMigrationAbort;
            {
                // Only if all actions taken as part of refreshing the placement version completed
                // successfully do we want to install the current metadata. A view can potentially
                // be created after spawning a thread to recover nss's shard version. It is then ok
                // to lock views in order to clear filtering metadata. DBLock and CollectionLock
                // must be used in order to avoid placement version checks
                Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
                Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
                auto scopedCsr =
                    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx,
                                                                                         nss);

                // cancellationToken needs to be checked under the CSR lock before overwriting the
                // filtering metadata to serialize with other threads calling
                // 'clearFilteringMetadata'.
                if (!cancellationToken.isCanceled()) {
                    // Atomically set the new filtering metadata and check if there is a migration
                    // that must be aborted.
                    scopedCsr->setFilteringMetadata(opCtx, currentMetadata);

                    if (currentMetadata.isSharded() && !currentMetadata.allowMigrations()) {
                        if (auto msm = MigrationSourceManager::get(*scopedCsr)) {
                            waitForMigrationAbort.emplace(msm->abort());
                        }
                    }
                }
            }

            // Join any ongoing migration outside of the CSR lock.
            if (waitForMigrationAbort) {
                waitForMigrationAbort->get(opCtx);
            }

            {
                // Remember to wake all waiting threads for this refresh to finish.
                Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
                Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
                auto scopedCsr =
                    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx,
                                                                                         nss);

                scopedCsr->resetPlacementVersionRecoverRefreshFuture();
                resetRefreshFutureOnError.dismiss();
            }
        })
        .onCompletion([=](Status status) {
            // Check the cancellation token here to ensure we throw in all cancelation events.
            if (cancellationToken.isCanceled() &&
                (status.isOK() || status == ErrorCodes::Interrupted)) {
                uasserted(ErrorCodes::PlacementVersionRefreshCanceled,
                          "Collection placement version refresh canceled by an interruption, "
                          "probably due to a 'clearFilteringMetadata'");
            }
            return status;
        })
        .semi()
        .share();
}

void FilteringMetadataCache::_onCollectionPlacementVersionMismatch(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<ChunkVersion> chunkVersionReceived) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    Timer t{};
    ScopeGuard finishTiming([&] {
        CurOp::get(opCtx)->debug().placementVersionRefreshMillis += Milliseconds(t.millis());
    });

    if (nss.isNamespaceAlwaysUntracked()) {
        return;
    }

    LOGV2_DEBUG(22061,
                2,
                "Metadata refresh requested for collection",
                logAttrs(nss),
                "chunkVersionReceived"_attr = chunkVersionReceived);

    while (true) {
        boost::optional<SharedSemiFuture<void>> inRecoverOrRefresh;

        {
            // The refresh threads do not perform any data reads themselves, therefore they don't
            // need to go through admission control.
            ScopedAdmissionPriority<ExecutionAdmissionContext> skipAdmissionControl(
                opCtx, AdmissionContext::Priority::kExempt);

            boost::optional<Lock::DBLock> dbLock;
            boost::optional<Lock::CollectionLock> collLock;
            dbLock.emplace(opCtx, nss.dbName(), MODE_IS);
            collLock.emplace(opCtx, nss, MODE_IS);

            if (chunkVersionReceived) {
                auto scopedCsr = boost::make_optional(
                    CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss));

                if (joinCollectionPlacementVersionOperation(
                        opCtx, &dbLock, &collLock, &scopedCsr)) {
                    continue;
                }

                if (auto metadata = (*scopedCsr)->getCurrentMetadataIfKnown()) {
                    const auto currentCollectionPlacementVersion =
                        metadata->getShardPlacementVersion();
                    // Don't need to remotely reload if the requested version is smaller than the
                    // known one. This means that the remote side is behind.
                    if (chunkVersionReceived->isOlderOrEqualThan(
                            currentCollectionPlacementVersion)) {
                        return;
                    }
                }
            }

            auto scopedCsr = boost::make_optional(
                CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss));

            if (joinCollectionPlacementVersionOperation(opCtx, &dbLock, &collLock, &scopedCsr)) {
                continue;
            }

            // If we reached here, there were no ongoing critical sections or recoverRefresh running
            // and we are holding the exclusive CSR lock.

            // If the shard doesn't yet know its filtering metadata, recovery needs to be run
            const bool runRecover = (*scopedCsr)->getCurrentMetadataIfKnown() ? false : true;
            CancellationSource cancellationSource;
            CancellationToken cancellationToken = cancellationSource.token();
            (*scopedCsr)
                ->setPlacementVersionRecoverRefreshFuture(
                    _recoverRefreshCollectionPlacementVersion(
                        opCtx->getServiceContext(), nss, runRecover, std::move(cancellationToken)),
                    std::move(cancellationSource));
            inRecoverOrRefresh = (*scopedCsr)->getPlacementVersionRecoverRefreshFuture(opCtx);
        }

        if (!waitForRefreshToComplete(opCtx, *inRecoverOrRefresh)) {
            // The refresh was canceled by a 'clearFilteringMetadata'. Retry the refresh.
            continue;
        }
        break;
    }
}

}  // namespace mongo
