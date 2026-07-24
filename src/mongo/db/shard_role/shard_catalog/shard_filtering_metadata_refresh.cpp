// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/ensure_chunk_version_is_greater_than_gen.h"
#include "mongo/db/global_catalog/ddl/shard_key_util.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/router_role/routing_cache/read_only_catalog_cache_loader.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata_recoverer.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/database_metadata_recoverer.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/shard_catalog_recoverer_tracker.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_util.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_api_d_params_gen.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/sharding_environment/stale_config_retry_attempt.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
using namespace std::literals::string_view_literals;

using shard_catalog_recoverer::AttemptResult;
using shard_catalog_recoverer::onDbVersionMismatchAuthoritative;
using shard_catalog_recoverer::onShardVersionMismatchAuthoritative;

MONGO_FAIL_POINT_DEFINE(hangInRefreshFilteringMetadataUntilSuccessInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRecoverRefreshThread);

namespace {

MONGO_FAIL_POINT_DEFINE(skipDatabaseVersionMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(skipShardFilteringMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(hangInEnsureChunkVersionIsGreaterThanInterruptible);
MONGO_FAIL_POINT_DEFINE(hangInEnsureChunkVersionIsGreaterThanThenSimulateErrorUninterruptible);
MONGO_FAIL_POINT_DEFINE(hangInRecoverRefreshDbVersionThread);

const auto getDecoration = ServiceContext::declareDecoration<FilteringMetadataCache>();

bool isAuthoritativeRecoveryEnabled() {
    return feature_flags::gAuthoritativeShardsCRUD.isEnabled(
        kVersionContextIgnored_UNSAFE,
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}

void ensureChunkVersionIsGreaterThan(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const UUID& collUUID,
                                     const ChunkRange& range,
                                     const ChunkVersion& preMigrationChunkVersion) {
    tassert(12598400,
            "Legacy migration recovery must not run when shards are authoritative",
            !feature_flags::gAuthoritativeShardsDDL.isEnabled(
                kVersionContextIgnored_UNSAFE,
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

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
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
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
    invariant(serviceCtx->getService() != nullptr);

    loader->initializeReplicaSetRole(isPrimary);

    auto decoration = FilteringMetadataCache::get(serviceCtx);
    invariant(decoration->_loader == nullptr);
    decoration->_loader = loader;

    if (feature_flags::gDualCatalogCache.isEnabled() ||
        feature_flags::gDatabaseDualCatalogCache.isEnabled()) {
        decoration->_cache =
            std::make_unique<CatalogCache>(serviceCtx, loader, "FilteringMetadata"sv);
    }
}

void FilteringMetadataCache::initForTesting(ServiceContext* serviceCtx,
                                            std::shared_ptr<ShardServerCatalogCacheLoader> loader) {
    invariant(serviceCtx->getService() != nullptr);

    auto decoration = FilteringMetadataCache::get(serviceCtx);
    invariant(decoration->_loader == nullptr);
    decoration->_loader = loader;

    if (feature_flags::gDualCatalogCache.isEnabled() ||
        feature_flags::gDatabaseDualCatalogCache.isEnabled()) {
        decoration->_cache =
            std::make_unique<CatalogCache>(serviceCtx, loader, "FilteringMetadata"sv);
    }
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

void FilteringMetadataCache::interruptLoaderAfterAuthoritativeShardsTransition() {
    tassert(10727901,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->interruptAfterAuthoritativeShardsTransition();
}

void FilteringMetadataCache::waitForAllLoaderFlushes(OperationContext* opCtx) {
    tassert(10727900,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->waitForAllFlushes(opCtx);
}

void FilteringMetadataCache::onStepDown() {
    tassert(9539100,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->onStepDown();
}

void FilteringMetadataCache::onStepUp() {
    tassert(9539101,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->onStepUp();
}

void FilteringMetadataCache::onReplicationRollback() {
    tassert(9539102,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->onReplicationRollback();
}

void FilteringMetadataCache::notifyOfCollectionRefreshEndMarkerSeen(const NamespaceString& nss,
                                                                    const Timestamp& commitTime) {
    tassert(9539103,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->notifyOfCollectionRefreshEndMarkerSeen(nss, commitTime);
}

void FilteringMetadataCache::waitForCollectionFlush(OperationContext* opCtx,
                                                    const NamespaceString& nss) {
    tassert(9539104,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->waitForCollectionFlush(opCtx, nss);
}

void FilteringMetadataCache::waitForDatabaseFlush(OperationContext* opCtx,
                                                  const DatabaseName& dbName) {
    tassert(9539105,
            "FilteringMetadataCache has not yet been initialized with a CatalogCacheLoader",
            _loader);

    _loader->waitForDatabaseFlush(opCtx, dbName);
}

void FilteringMetadataCache::report(BSONObjBuilder* builder) const {
    if (_cache)
        _cache->report(builder);
}

Status FilteringMetadataCache::onShardVersionMismatch(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<ChunkVersion> chunkVersionReceived) noexcept try {
    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    if (nss.isNamespaceAlwaysUntracked()) {
        return Status::OK();
    }

    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

    LOGV2_DEBUG(22061,
                2,
                "Metadata refresh requested for collection",
                logAttrs(nss),
                "chunkVersionReceived"_attr = chunkVersionReceived,
                "isAuthoritative"_attr = isAuthoritativeRecoveryEnabled());

    Timer t;
    ScopeGuard finishTiming([&] {
        CurOp::get(opCtx)->debug().placementVersionRefreshMillis += Milliseconds(t.millis());
    });

    while ((isAuthoritativeRecoveryEnabled()
                ? onShardVersionMismatchAuthoritative(opCtx, nss, chunkVersionReceived) ==
                    AttemptResult::kRetry
                : _onShardVersionMismatchNonAuthoritative(opCtx, nss, chunkVersionReceived) ==
                    MismatchAttemptResult::kRetry)) {
    }

    return Status::OK();
} catch (const DBException& ex) {
    LOGV2(22062,
          "Failed to refresh metadata for collection",
          logAttrs(nss),
          "error"_attr = redact(ex));
    return ex.toStatus();
}

void FilteringMetadataCache::forceCollectionMetadataRefresh_DEPRECATED(OperationContext* opCtx,
                                                                       const NamespaceString& nss) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    const auto catalogCache = [&]() {
        if (!feature_flags::gDualCatalogCache.isEnabled()) {
            return Grid::get(opCtx)->catalogCache();
        }

        tassert(10429300,
                "Expected to find the CatalogCache used for filtering purposes initialized",
                _cache);

        return _cache.get();
    }();

    const auto cm =
        uassertStatusOK(catalogCache->getCollectionPlacementInfoWithRefresh(opCtx, nss));

    if (!cm.hasRoutingTable()) {
        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
        scopedCsr->setCollectionMetadata(opCtx, CollectionMetadata::UNTRACKED());
        return;
    }

    auto isCollectionPlacementUpToDate = [&](boost::optional<CollectionMetadata> optMetadata) {
        if (optMetadata) {
            const auto& metadata = *optMetadata;
            if (metadata.hasRoutingTable()) {
                auto compareResult = cm.getVersion() <=> metadata.getCollPlacementVersion();
                if (compareResult == std::partial_ordering::less ||
                    compareResult == std::partial_ordering::equivalent) {
                    LOGV2_DEBUG(
                        22063,
                        1,
                        "Skipping metadata refresh because collection already is up-to-date",
                        logAttrs(nss),
                        "latestCollectionPlacementVersion"_attr =
                            metadata.getCollPlacementVersion(),
                        "refreshedCollectionPlacementVersion"_attr = cm.getVersion());
                    return true;
                }
            }
        }
        return false;
    };

    auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);

    if (isCollectionPlacementUpToDate(scopedCsr->getCurrentMetadataIfKnown() /* optMetadata */)) {
        return;
    }

    CollectionMetadata metadata(cm, ShardingState::get(opCtx)->shardId());
    scopedCsr->setCollectionMetadata(opCtx, std::move(metadata));
}

Status FilteringMetadataCache::onDbVersionMismatch(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   const DatabaseVersion& clientDbVersion) noexcept
    try {
    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    tassert(ErrorCodes::IllegalOperation,
            fmt::format("Can't check version of {} database", dbName.toStringForErrorMsg()),
            !dbName.isAdminDB() && !dbName.isConfigDB());

    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

    LOGV2_DEBUG(6697200,
                2,
                "Handle database version mismatch",
                "db"_attr = dbName,
                "receivedDbVersion"_attr = clientDbVersion,
                "isAuthoritative"_attr = isAuthoritativeRecoveryEnabled());

    Timer t;
    ScopeGuard finishTiming([&] {
        CurOp::get(opCtx)->debug().databaseVersionRefreshMillis += Milliseconds(t.millis());
    });

    while ((isAuthoritativeRecoveryEnabled()
                ? onDbVersionMismatchAuthoritative(opCtx, dbName, clientDbVersion) ==
                    AttemptResult::kRetry
                : _onDbVersionMismatchNonAuthoritative(opCtx, dbName, clientDbVersion) ==
                    MismatchAttemptResult::kRetry)) {
    }

    return Status::OK();
} catch (const DBException& ex) {
    LOGV2(
        22065, "Failed to refresh databaseVersion", "db"_attr = dbName, "error"_attr = redact(ex));
    return ex.toStatus();
}

Status FilteringMetadataCache::forceDatabaseMetadataRefresh_DEPRECATED(
    OperationContext* opCtx, const DatabaseName& dbName) noexcept try {
    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    tassert(ErrorCodes::IllegalOperation,
            fmt::format("Can't check version of {} database", dbName.toStringForErrorMsg()),
            !dbName.isAdminDB() && !dbName.isConfigDB());

    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

    LOGV2_DEBUG(13119102,
                2,
                "Handle database version mismatch",
                "db"_attr = dbName,
                "receivedDbVersion"_attr = boost::none,
                "isAuthoritative"_attr = false);

    while (_onDbVersionMismatchNonAuthoritative(opCtx, dbName, boost::none) !=
           MismatchAttemptResult::kDone) {
        uassert(ErrorCodes::MetadataRefreshCanceledDueToFCVTransition,
                "Database metadata refresh was canceled due to an FCV transition",
                !isAuthoritativeRecoveryEnabled());
    }
    return Status::OK();
} catch (const DBException& ex) {
    LOGV2(10250102,
          "Failed to refresh databaseVersion",
          "db"_attr = dbName,
          "error"_attr = redact(ex));
    return ex.toStatus();
}

CollectionMetadata FilteringMetadataCache::_forceGetCurrentCollectionMetadata(
    OperationContext* opCtx, const NamespaceString& nss) {
    try {
        const auto catalogCache = [&]() {
            if (!feature_flags::gDualCatalogCache.isEnabled()) {
                return Grid::get(opCtx)->catalogCache();
            }

            tassert(10429301,
                    "Expected to find the CatalogCache used for filtering purposes initialized",
                    _cache);

            return _cache.get();
        }();

        const auto cm =
            uassertStatusOK(catalogCache->getCollectionPlacementInfoWithRefresh(opCtx, nss));

        if (!cm.hasRoutingTable()) {
            return CollectionMetadata::UNTRACKED();
        }

        return CollectionMetadata(cm, ShardingState::get(opCtx)->shardId());
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        LOGV2(505070,
              "Namespace not found, collection may have been dropped",
              logAttrs(nss),
              "error"_attr = redact(ex));
        return CollectionMetadata::UNTRACKED();
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
            // A MoveRangeCoordinator recovers its own migrations. Skip them here so an unrelated
            // filtering-metadata refresh does not complete the migration prematurely.
            if (doc.getManagementMode() == ManagementModeEnum::kMoveRangeCoordinator) {
                LOGV2_DEBUG(12795322,
                            2,
                            "Skipping legacy migration recovery for coordinator-owned migration",
                            "migrationCoordinatorDocument"_attr = redact(doc.toBSON()));
                return true;
            }

            LOGV2_DEBUG(4798502,
                        2,
                        "Recovering migration",
                        "migrationCoordinatorDocument"_attr = redact(doc.toBSON()));

            tassert(12796804,
                    "Legacy migration recovery must not run when shards are authoritative",
                    sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ==
                        AuthoritativeMetadataAccessLevelEnum::kNone);

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
                coordinator.completeMigration(opCtx, true /* clearShardCatalogCache */);
                return true;
            }

            // The decision is not known. Recover the decision from the config server.

            ensureChunkVersionIsGreaterThan(opCtx,
                                            doc.getNss(),
                                            doc.getCollectionUuid(),
                                            doc.getRange(),
                                            doc.getPreMigrationChunkVersion());

            hangInRefreshFilteringMetadataUntilSuccessInterruptible.pauseWhileSet(opCtx);

            auto currentMetadata = _forceGetCurrentCollectionMetadata(opCtx, doc.getNss());

            if (hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .shouldFail()) {
                hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible
                    .pauseWhileSet();
                uasserted(ErrorCodes::InternalError,
                          "simulate an error response for forceGetCurrentMetadata");
            }

            auto setCollectionMetadata = [&opCtx, &currentMetadata, &doc, &cancellationToken]() {
                auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, doc.getNss());

                if (!cancellationToken.isCanceled()) {
                    auto optMetadata = scopedCsr->getCurrentMetadataIfKnown();
                    invariant(!optMetadata);

                    scopedCsr->setCollectionMetadata(opCtx, std::move(currentMetadata));
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
                    opCtx,
                    doc.getCollectionUuid(),
                    doc.getRange(),
                    defaultMajorityWriteConcernDoNotUse());
                coordinator.forgetMigration(opCtx);
                setCollectionMetadata();
                return true;
            }

            // Note this should only extend the range boundaries (if there has been a shard key
            // refine since the migration began) and never truncate them.
            auto chunkRangeToCompareToMetadata =
                shardkeyutil::extendOrTruncateBoundsForMetadata(currentMetadata, doc.getRange());
            if (currentMetadata.keyBelongsToMe(chunkRangeToCompareToMetadata.getMin())) {
                coordinator.setMigrationDecision(DecisionEnum::kAborted);
            } else {
                coordinator.setMigrationDecision(DecisionEnum::kCommitted);
                bool noMoreCollectionChunksOnDonor =
                    !currentMetadata.getChunkManager()->getVersion(doc.getDonorShardId()).isSet();
                notifyChangeStreamsOnChunkMigrated(
                    opCtx,
                    doc.getNss(),
                    doc.getCollectionUuid(),
                    doc.getDonorShardId(),
                    doc.getRecipientShardId(),
                    noMoreCollectionChunksOnDonor,
                    doc.getTransfersFirstCollectionChunkToRecipient().value_or(false));
            }

            coordinator.setShardKeyPattern(KeyPattern(currentMetadata.getKeyPattern()));
            coordinator.completeMigration(opCtx, true /* clearShardCatalogCache */);
            setCollectionMetadata();
            return true;
        });
}


Status FilteringMetadataCache::_refreshDbMetadata(OperationContext* opCtx,
                                                  const DatabaseName& dbName,
                                                  CancellationToken cancellationToken) {
    ScopeGuard resetRefreshFutureOnError([&] {
        auto scopedDsr = DatabaseShardingRuntime::acquireExclusive(opCtx, dbName);
        scopedDsr->resetDbMetadataRefreshFuture_DEPRECATED();
    });

    const auto catalogCache = [&]() {
        if (!feature_flags::gDatabaseDualCatalogCache.isEnabled()) {
            return Grid::get(opCtx)->catalogCache();
        }

        tassert(10429302,
                "Expected to find the CatalogCache used for filtering purposes initialized",
                _cache);

        return _cache.get();
    }();

    // Force a refresh of the cached database metadata from the config server.
    catalogCache->onStaleDatabaseVersion(dbName, boost::none /* wantedVersion */);
    const auto swDbMetadata = catalogCache->getDatabase(opCtx, dbName);

    Lock::DBLock dbLock(
        opCtx,
        dbName,
        MODE_IX,
        Date_t::max(),
        Lock::DBLockSkipOptions{
            false, false, false, rss::consensus::IntentRegistry::Intent::LocalWrite});

    auto scopedDsr = DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(opCtx, dbName);

    if (!cancellationToken.isCanceled()) {
        // Before committing, make sure the feature flag hasn't changed.
        uassert(ErrorCodes::MetadataRefreshCanceledDueToFCVTransition,
                "Non-authoritative database metadata refresh can't proceed: FCV has changed",
                sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                    kVersionContextIgnored_UNSAFE,
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) !=
                    AuthoritativeMetadataAccessLevelEnum::kWritesAndReadsAllowed);

        if (swDbMetadata.isOK()) {
            // Set the refreshed database metadata in the local catalog.
            scopedDsr->setDbInfo_DEPRECATED(opCtx, *swDbMetadata.getValue());
        } else if (swDbMetadata == ErrorCodes::NamespaceNotFound) {
            // The non-authoritative database model stores metadata from other shards in the DSS to
            // respond to stale routers without requiring a refresh each time. While clearing
            // database information at this stage can optimize the old protocol, it is not strictly
            // necessary since this shard does not own the database.

            // Because the authoritative and non-authoritative models must coexist during the
            // upgrade transition, it is safer to not clear the database metadata.
        }
    }

    // Reset the future reference to allow any other thread to refresh the database metadata.
    scopedDsr->resetDbMetadataRefreshFuture_DEPRECATED();
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
            ThreadClient tc("DbMetadataRefreshThread", serviceCtx->getService());
            const auto opCtxHolder =
                CancelableOperationContext(tc->makeOperationContext(), cancellationToken, executor);
            auto opCtx = opCtxHolder.get();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // Forward `users` and `roles` attributes from the original request.
            forwardableOpMetadata.setOn(opCtx);

            LOGV2_DEBUG(6697203, 2, "Started database metadata refresh", logAttrs(dbName));

            hangInRecoverRefreshDbVersionThread.pauseWhileSet();

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

FilteringMetadataCache::MismatchAttemptResult
FilteringMetadataCache::_onDbVersionMismatchNonAuthoritative(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    boost::optional<DatabaseVersion> receivedDbVersion) {
    if (receivedDbVersion) {
        auto scopedDsr =
            boost::make_optional(DatabaseShardingRuntime::acquireShared(opCtx, dbName));

        if (refresh_util::joinPlacementVersionOperations(opCtx, scopedDsr)) {
            // Waited for another thread to exit from the critical section or to complete an
            // ongoing refresh, so retry.
            return MismatchAttemptResult::kRetry;
        }

        // From now until the end of this block [1] no thread is in the critical section or
        // can enter it (would require to exclusive lock the DSS) and [2] no metadata
        // refresh is in progress or can start (would require to exclusive lock the DSS).
        // Therefore, the database version can be accessed safely.

        const auto dbVersion = (*scopedDsr)->getDbVersion(opCtx);
        if (dbVersion && receivedDbVersion <= *dbVersion) {
            // No need to refresh the database metadata as the wanted version is newer than the
            // one received.
            return MismatchAttemptResult::kDone;
        }
    }

    if (MONGO_unlikely(skipDatabaseVersionMetadataRefresh.shouldFail())) {
        return MismatchAttemptResult::kDone;
    }

    boost::optional<SharedSemiFuture<void>> dbMetadataRefreshFuture;

    {
        auto scopedDsr =
            boost::make_optional(DatabaseShardingRuntime::acquireExclusive(opCtx, dbName));

        if (refresh_util::joinPlacementVersionOperations(opCtx, scopedDsr)) {
            // Waited for another thread to exit from the critical section or to complete an
            // ongoing refresh, so retry.
            return MismatchAttemptResult::kRetry;
        }

        // From now until the end of this block [1] no thread is in the critical section or can
        // enter it (would require to exclusive lock the DSS) and [2] this is the only metadata
        // refresh in progress (holding the exclusive lock on the DSS).
        // Therefore, the future to refresh the database metadata can be set.

        if (!refresh_util::spawnTrackedDbRecovery(opCtx,
                                                  *scopedDsr,
                                                  RecoveryKind::kNonAuthoritative,
                                                  [&](const CancellationToken& cancellationToken) {
                                                      return _recoverRefreshDbVersion(
                                                          opCtx, dbName, cancellationToken);
                                                  })) {
            return MismatchAttemptResult::kRetry;
        }
        dbMetadataRefreshFuture = (*scopedDsr)->getMetadataRefreshFuture();
    }

    // No other metadata refresh for this database can run in parallel. If another thread enters
    // the critical section, the ongoing refresh would be interrupted and subsequently
    // re-queued.
    if (!refresh_util::waitForRefreshToComplete(opCtx, *dbMetadataRefreshFuture)) {
        return MismatchAttemptResult::kRetry;
    }

    return MismatchAttemptResult::kDone;
}


SharedSemiFuture<void> FilteringMetadataCache::_recoverRefreshCollectionPlacementVersion(
    ServiceContext* serviceContext,
    const NamespaceString& nss,
    bool runRecover,
    CancellationToken cancellationToken) {
    auto executor = Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor();
    return ExecutorFuture<void>(executor)
        .then([=, this] {
            ThreadClient tc("RecoverRefreshThread", serviceContext->getService());

            if (MONGO_unlikely(hangInRecoverRefreshThread.shouldFail())) {
                hangInRecoverRefreshThread.pauseWhileSet();
            }

            const auto opCtxHolder =
                CancelableOperationContext(tc->makeOperationContext(), cancellationToken, executor);
            auto const opCtx = opCtxHolder.get();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            ScopeGuard resetRefreshFutureOnError([&] {
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

            auto currentMetadata = _forceGetCurrentCollectionMetadata(opCtx, nss);

            if (currentMetadata.hasRoutingTable()) {
                // Abort and join any ongoing migration if migrations are disallowed for the
                // namespace.
                if (!currentMetadata.allowMigrations()) {
                    boost::optional<SharedSemiFuture<void>> waitForMigrationAbort;
                    {
                        const auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, nss);
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
                // to lock views in order to clear filtering metadata.
                auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);

                // cancellationToken needs to be checked under the CSR lock before overwriting the
                // filtering metadata to serialize with other threads calling
                // 'clearCollectionMetadata'.
                if (!cancellationToken.isCanceled()) {
                    // Before committing, make sure the feature flag hasn't changed.
                    uassert(ErrorCodes::MetadataRefreshCanceledDueToFCVTransition,
                            "Non-authoritative collection metadata refresh can't proceed: FCV has "
                            "changed",
                            sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                                kVersionContextIgnored_UNSAFE,
                                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) !=
                                AuthoritativeMetadataAccessLevelEnum::kWritesAndReadsAllowed);

                    // Atomically set the new filtering metadata and check if there is a migration
                    // that must be aborted.
                    scopedCsr->setCollectionMetadata(opCtx, currentMetadata);

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

            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
            scopedCsr->resetPlacementVersionRecoverRefreshFuture();
            resetRefreshFutureOnError.dismiss();
        })
        .onCompletion([=](Status status) {
            // Check the cancellation token here to ensure we throw in all cancelation events.
            if (cancellationToken.isCanceled() &&
                (status.isOK() || status == ErrorCodes::Interrupted)) {
                uasserted(ErrorCodes::PlacementVersionRefreshCanceled,
                          "Collection placement version refresh canceled by an interruption, "
                          "probably due to a 'clearCollectionMetadata'");
            }
            return status;
        })
        .semi()
        .share();
}

FilteringMetadataCache::MismatchAttemptResult
FilteringMetadataCache::_onShardVersionMismatchNonAuthoritative(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<ChunkVersion> chunkVersionReceived) {
    boost::optional<SharedSemiFuture<void>> inRecoverOrRefresh;

    {
        // The refresh threads do not perform any data reads themselves, therefore they don't
        // need to go through admission control.
        ScopedAdmissionPriority<ExecutionAdmissionContext> skipAdmissionControl(
            opCtx, AdmissionContext::Priority::kExempt);

        if (chunkVersionReceived) {
            auto scopedCsr =
                boost::make_optional(CollectionShardingRuntime::acquireShared(opCtx, nss));

            if (refresh_util::joinPlacementVersionOperations(opCtx, scopedCsr)) {
                return MismatchAttemptResult::kRetry;
            }

            if (auto metadata = (*scopedCsr)->getCurrentMetadataIfKnown()) {
                const auto currentCollectionPlacementVersion = metadata->getShardPlacementVersion();
                // Don't need to remotely reload if the requested version is smaller than the
                // known one. This means that the remote side is behind.
                auto compareResult = *chunkVersionReceived <=> currentCollectionPlacementVersion;
                if (compareResult == std::partial_ordering::less ||
                    compareResult == std::partial_ordering::equivalent) {
                    return MismatchAttemptResult::kDone;
                }
            }
        }

        auto scopedCsr =
            boost::make_optional(CollectionShardingRuntime::acquireExclusive(opCtx, nss));

        if (refresh_util::joinPlacementVersionOperations(opCtx, scopedCsr)) {
            return MismatchAttemptResult::kRetry;
        }

        // If we reached here, there were no ongoing critical sections or recoverRefresh running
        // and we are holding the exclusive CSR lock.

        // If the shard doesn't yet know its filtering metadata, recovery needs to be run
        const bool runRecover = (*scopedCsr)->getCurrentMetadataIfKnown() ? false : true;
        if (!refresh_util::spawnTrackedCollectionRecovery(
                opCtx,
                *scopedCsr,
                RecoveryKind::kNonAuthoritative,
                [&](const CancellationToken& cancellationToken) {
                    return _recoverRefreshCollectionPlacementVersion(
                        opCtx->getServiceContext(), nss, runRecover, cancellationToken);
                })) {
            return MismatchAttemptResult::kRetry;
        }
        inRecoverOrRefresh = (*scopedCsr)->getMetadataRefreshFuture();
    }

    if (!refresh_util::waitForRefreshToComplete(opCtx, *inRecoverOrRefresh)) {
        return MismatchAttemptResult::kRetry;
    }
    return MismatchAttemptResult::kDone;
}

}  // namespace mongo
