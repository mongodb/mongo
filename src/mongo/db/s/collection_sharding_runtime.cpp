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

#include "mongo/db/s/collection_sharding_runtime.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/sbe_plan_cache.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

class UnshardedCollection : public ScopedCollectionDescription::Impl {
public:
    UnshardedCollection() = default;

    const CollectionMetadata& get() override {
        return _metadata;
    }

private:
    CollectionMetadata _metadata;
};

const auto kUnshardedCollection = std::make_shared<UnshardedCollection>();

boost::optional<ShardVersion> getOperationReceivedVersion(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    // If there is a version attached to the OperationContext, use it as the received version.
    if (OperationShardingState::isComingFromRouter(opCtx)) {
        return OperationShardingState::get(opCtx).getShardVersion(nss);
    }

    // There is no shard version information on the 'opCtx'. This means that the operation
    // represented by 'opCtx' is unversioned, and the shard version is always OK for unversioned
    // operations.
    return boost::none;
}

// This shard version is used as the received version in StaleConfigInfo since we do not have
// information about the received version of the operation.
ShardVersion ShardVersionPlacementIgnoredNoIndexes() {
    return ShardVersionFactory::make(ChunkVersion::IGNORED(),
                                     boost::optional<CollectionIndexes>(boost::none));
}

}  // namespace

CollectionShardingRuntime::ScopedSharedCollectionShardingRuntime::
    ScopedSharedCollectionShardingRuntime(ScopedCollectionShardingState&& scopedCss)
    : _scopedCss(std::move(scopedCss)) {}
CollectionShardingRuntime::ScopedExclusiveCollectionShardingRuntime::
    ScopedExclusiveCollectionShardingRuntime(ScopedCollectionShardingState&& scopedCss)
    : _scopedCss(std::move(scopedCss)) {}

CollectionShardingRuntime::CollectionShardingRuntime(
    ServiceContext* service,
    NamespaceString nss,
    std::shared_ptr<executor::TaskExecutor> rangeDeleterExecutor)
    : _serviceContext(service),
      _nss(std::move(nss)),
      _rangeDeleterExecutor(std::move(rangeDeleterExecutor)),
      _metadataType(_nss.isNamespaceAlwaysUnsharded() ? MetadataType::kUnsharded
                                                      : MetadataType::kUnknown) {}

CollectionShardingRuntime::ScopedSharedCollectionShardingRuntime
CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(OperationContext* opCtx,
                                                                  const NamespaceString& nss) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));
    return ScopedSharedCollectionShardingRuntime(
        ScopedCollectionShardingState::acquireScopedCollectionShardingState(opCtx, nss, MODE_IS));
}

CollectionShardingRuntime::ScopedExclusiveCollectionShardingRuntime
CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(OperationContext* opCtx,
                                                                     const NamespaceString& nss) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));
    return ScopedExclusiveCollectionShardingRuntime(
        ScopedCollectionShardingState::acquireScopedCollectionShardingState(opCtx, nss, MODE_X));
}

CollectionShardingRuntime::ScopedExclusiveCollectionShardingRuntime
CollectionShardingRuntime::acquireExclusive(OperationContext* opCtx, const NamespaceString& nss) {
    return ScopedExclusiveCollectionShardingRuntime(
        ScopedCollectionShardingState::acquireScopedCollectionShardingState(opCtx, nss, MODE_X));
}

ScopedCollectionFilter CollectionShardingRuntime::getOwnershipFilter(
    OperationContext* opCtx,
    OrphanCleanupPolicy orphanCleanupPolicy,
    bool supportNonVersionedOperations) const {
    const boost::optional<ShardVersion> optReceivedShardVersion =
        getOperationReceivedVersion(opCtx, _nss);
    if (!supportNonVersionedOperations) {
        // No operations should be calling getOwnershipFilter without a shard version
        tassert(7032300,
                "getOwnershipFilter called by operation that doesn't specify shard version",
                optReceivedShardVersion);
    }

    auto metadata =
        _getMetadataWithVersionCheckAt(opCtx,
                                       repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime(),
                                       optReceivedShardVersion,
                                       supportNonVersionedOperations);

    if (!supportNonVersionedOperations) {
        tassert(7032301,
                "For sharded collections getOwnershipFilter cannot be relied on without a valid "
                "shard version",
                !ShardVersion::isPlacementVersionIgnored(*optReceivedShardVersion) ||
                    !metadata->get().allowMigrations() || !metadata->get().isSharded());
    }

    return {std::move(metadata)};
}

ScopedCollectionFilter CollectionShardingRuntime::getOwnershipFilter(
    OperationContext* opCtx,
    OrphanCleanupPolicy orphanCleanupPolicy,
    const ShardVersion& receivedShardVersion) const {
    return _getMetadataWithVersionCheckAt(
        opCtx, repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime(), receivedShardVersion);
}

ScopedCollectionDescription CollectionShardingRuntime::getCollectionDescription(
    OperationContext* opCtx) const {
    const bool operationIsVersioned = OperationShardingState::isComingFromRouter(opCtx);
    return getCollectionDescription(opCtx, operationIsVersioned);
}

ScopedCollectionDescription CollectionShardingRuntime::getCollectionDescription(
    OperationContext* opCtx, bool operationIsVersioned) const {
    // If the server has been started with --shardsvr, but hasn't been added to a cluster we should
    // consider all collections as unsharded
    if (!ShardingState::get(opCtx)->enabled())
        return {kUnshardedCollection};

    // Present the collection as unsharded to internal or direct commands against shards
    if (!operationIsVersioned)
        return {kUnshardedCollection};

    auto& oss = OperationShardingState::get(opCtx);

    auto optMetadata = _getCurrentMetadataIfKnown(boost::none);
    const auto receivedShardVersion{oss.getShardVersion(_nss)};
    uassert(
        StaleConfigInfo(_nss,
                        receivedShardVersion ? *receivedShardVersion
                                             : ShardVersionPlacementIgnoredNoIndexes(),
                        boost::none /* wantedVersion */,
                        ShardingState::get(_serviceContext)->shardId()),
        str::stream() << "sharding status of collection " << _nss.ns()
                      << " is not currently available for description and needs to be recovered "
                      << "from the config server",
        optMetadata);

    return {std::move(optMetadata)};
}

boost::optional<ShardingIndexesCatalogCache> CollectionShardingRuntime::getIndexesInCritSec(
    OperationContext* opCtx) const {
    return _shardingIndexesCatalogInfo;
}

boost::optional<CollectionMetadata> CollectionShardingRuntime::getCurrentMetadataIfKnown() const {
    auto optMetadata = _getCurrentMetadataIfKnown(boost::none);
    if (!optMetadata)
        return boost::none;
    return optMetadata->get();
}

void CollectionShardingRuntime::checkShardVersionOrThrow(OperationContext* opCtx) const {
    const auto optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);
    if (optReceivedShardVersion) {
        checkShardVersionOrThrow(opCtx, *optReceivedShardVersion);
    }
}

void CollectionShardingRuntime::checkShardVersionOrThrow(
    OperationContext* opCtx, const ShardVersion& receivedShardVersion) const {
    (void)_getMetadataWithVersionCheckAt(opCtx, boost::none, receivedShardVersion);
}

void CollectionShardingRuntime::enterCriticalSectionCatchUpPhase(const BSONObj& reason) {
    _critSec.enterCriticalSectionCatchUpPhase(reason);

    if (_placementVersionInRecoverOrRefresh) {
        _placementVersionInRecoverOrRefresh->cancellationSource.cancel();
    }
}

void CollectionShardingRuntime::enterCriticalSectionCommitPhase(const BSONObj& reason) {
    _critSec.enterCriticalSectionCommitPhase(reason);
}

void CollectionShardingRuntime::rollbackCriticalSectionCommitPhaseToCatchUpPhase(
    const BSONObj& reason) {
    _critSec.rollbackCriticalSectionCommitPhaseToCatchUpPhase(reason);
}

void CollectionShardingRuntime::exitCriticalSection(const BSONObj& reason) {
    _critSec.exitCriticalSection(reason);
}

void CollectionShardingRuntime::exitCriticalSectionNoChecks() {
    _critSec.exitCriticalSectionNoChecks();
}

boost::optional<SharedSemiFuture<void>> CollectionShardingRuntime::getCriticalSectionSignal(
    OperationContext* opCtx, ShardingMigrationCriticalSection::Operation op) const {
    return _critSec.getSignal(op);
}

void CollectionShardingRuntime::setFilteringMetadata(OperationContext* opCtx,
                                                     CollectionMetadata newMetadata) {
    tassert(7032302,
            str::stream() << "Namespace " << _nss.ns() << " must never be sharded.",
            !newMetadata.isSharded() || !_nss.isNamespaceAlwaysUnsharded());

    stdx::lock_guard lk(_metadataManagerLock);

    // If the collection was sharded and the new metadata represents a new collection we might need
    // to clean up some sharding-related state
    if (_metadataManager) {
        const auto oldShardPlacementVersion = _metadataManager->getActivePlacementVersion();
        const auto newShardPlacementVersion = newMetadata.getShardPlacementVersion();
        if (!oldShardPlacementVersion.isSameCollection(newShardPlacementVersion))
            _cleanupBeforeInstallingNewCollectionMetadata(lk, opCtx);
    }

    if (!newMetadata.isSharded()) {
        LOGV2(21917,
              "Marking collection {namespace} as unsharded",
              "Marking collection as unsharded",
              logAttrs(_nss));
        _metadataType = MetadataType::kUnsharded;
        _metadataManager.reset();
        ++_numMetadataManagerChanges;
        return;
    }

    // At this point we know that the new metadata is associated to a sharded collection.
    _metadataType = MetadataType::kSharded;

    if (!_metadataManager || !newMetadata.uuidMatches(_metadataManager->getCollectionUuid())) {
        _metadataManager = std::make_shared<MetadataManager>(
            opCtx->getServiceContext(), _nss, _rangeDeleterExecutor, newMetadata);
        ++_numMetadataManagerChanges;
    } else {
        _metadataManager->setFilteringMetadata(std::move(newMetadata));
    }
}

void CollectionShardingRuntime::_clearFilteringMetadata(OperationContext* opCtx,
                                                        bool collIsDropped) {
    if (_placementVersionInRecoverOrRefresh) {
        _placementVersionInRecoverOrRefresh->cancellationSource.cancel();
    }

    stdx::lock_guard lk(_metadataManagerLock);
    if (!_nss.isNamespaceAlwaysUnsharded()) {
        LOGV2_DEBUG(4798530,
                    1,
                    "Clearing metadata for collection {namespace}",
                    "Clearing collection metadata",
                    logAttrs(_nss),
                    "collIsDropped"_attr = collIsDropped);

        // If the collection is sharded and it's being dropped we might need to clean up some state.
        if (collIsDropped)
            _cleanupBeforeInstallingNewCollectionMetadata(lk, opCtx);

        _metadataType = MetadataType::kUnknown;
        if (collIsDropped)
            _metadataManager.reset();
    }
}

void CollectionShardingRuntime::clearFilteringMetadata(OperationContext* opCtx) {
    _clearFilteringMetadata(opCtx, /* collIsDropped */ false);
}

void CollectionShardingRuntime::clearFilteringMetadataForDroppedCollection(
    OperationContext* opCtx) {
    _clearFilteringMetadata(opCtx, /* collIsDropped */ true);
}

SharedSemiFuture<void> CollectionShardingRuntime::cleanUpRange(ChunkRange const& range,
                                                               CleanWhen when) const {
    // (Ignore FCV check): This feature doesn't have any upgrade/downgrade concerns. The feature
    // flag is used to turn on new range deleter on startup.
    if (!feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCVUnsafe()) {
        stdx::lock_guard lk(_metadataManagerLock);
        invariant(_metadataType == MetadataType::kSharded);
        return _metadataManager->cleanUpRange(range, when == kDelayed);
    }

    // This method must never be called if the range deleter service feature flag is enabled
    MONGO_UNREACHABLE;
}

Status CollectionShardingRuntime::waitForClean(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const UUID& collectionUuid,
                                               ChunkRange orphanRange,
                                               Date_t deadline) {
    while (true) {
        const StatusWith<SharedSemiFuture<void>> swOrphanCleanupFuture =
            [&]() -> StatusWith<SharedSemiFuture<void>> {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            const auto self =
                CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
            stdx::lock_guard lk(self->_metadataManagerLock);

            // If the metadata was reset, or the collection was dropped and recreated since the
            // metadata manager was created, return an error.
            if (self->_metadataType != MetadataType::kSharded ||
                (collectionUuid != self->_metadataManager->getCollectionUuid())) {
                return {ErrorCodes::ConflictingOperationInProgress,
                        "Collection being migrated was dropped and created or otherwise had its "
                        "metadata reset"};
            }

            // (Ignore FCV check): This feature doesn't have any upgrade/downgrade concerns. The
            // feature flag is used to turn on new range deleter on startup.
            if (feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCVUnsafe()) {
                return RangeDeleterService::get(opCtx)->getOverlappingRangeDeletionsFuture(
                    self->_metadataManager->getCollectionUuid(), orphanRange);
            } else {
                return self->_metadataManager->trackOrphanedDataCleanup(orphanRange);
            }
        }();

        if (!swOrphanCleanupFuture.isOK()) {
            return swOrphanCleanupFuture.getStatus();
        }

        auto orphanCleanupFuture = std::move(swOrphanCleanupFuture.getValue());
        if (orphanCleanupFuture.isReady()) {
            LOGV2_OPTIONS(21918,
                          {logv2::LogComponent::kShardingMigration},
                          "Finished waiting for deletion of {namespace} range {orphanRange}",
                          "Finished waiting for deletion of orphans",
                          logAttrs(nss),
                          "orphanRange"_attr = redact(orphanRange.toString()));
            return Status::OK();
        }

        LOGV2_OPTIONS(21919,
                      {logv2::LogComponent::kShardingMigration},
                      "Waiting for deletion of {namespace} range {orphanRange}",
                      "Waiting for deletion of orphans",
                      logAttrs(nss),
                      "orphanRange"_attr = orphanRange);
        try {
            opCtx->runWithDeadline(
                deadline, ErrorCodes::ExceededTimeLimit, [&] { orphanCleanupFuture.get(opCtx); });
        } catch (const DBException& ex) {
            auto result = ex.toStatus();
            // Swallow RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist error since the
            // collection could either never exist or get dropped directly from the shard after the
            // range deletion task got scheduled.
            if (result != ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist) {
                return result.withContext(str::stream() << "Failed to delete orphaned " << nss.ns()
                                                        << " range " << orphanRange.toString());
            }
        }
    }

    MONGO_UNREACHABLE;
}

SharedSemiFuture<void> CollectionShardingRuntime::getOngoingQueriesCompletionFuture(
    const UUID& collectionUuid, ChunkRange const& range) const {
    stdx::lock_guard lk(_metadataManagerLock);

    if (!_metadataManager || _metadataManager->getCollectionUuid() != collectionUuid) {
        return SemiFuture<void>::makeReady().share();
    }
    return _metadataManager->getOngoingQueriesCompletionFuture(range);
}


std::shared_ptr<ScopedCollectionDescription::Impl>
CollectionShardingRuntime::_getCurrentMetadataIfKnown(
    const boost::optional<LogicalTime>& atClusterTime) const {
    stdx::lock_guard lk(_metadataManagerLock);
    switch (_metadataType) {
        case MetadataType::kUnknown:
            // Until user collections can be sharded in serverless, the sessions collection will be
            // the only sharded collection.
            if (getGlobalReplSettings().isServerless() &&
                _nss != NamespaceString::kLogicalSessionsNamespace) {
                return kUnshardedCollection;
            }
            return nullptr;
        case MetadataType::kUnsharded:
            return kUnshardedCollection;
        case MetadataType::kSharded:
            return _metadataManager->getActiveMetadata(atClusterTime);
    };
    MONGO_UNREACHABLE;
}

std::shared_ptr<ScopedCollectionDescription::Impl>
CollectionShardingRuntime::_getMetadataWithVersionCheckAt(
    OperationContext* opCtx,
    const boost::optional<mongo::LogicalTime>& atClusterTime,
    const boost::optional<ShardVersion>& optReceivedShardVersion,
    bool supportNonVersionedOperations) const {
    // If the server has been started with --shardsvr, but hasn't been added to a cluster we should
    // consider all collections as unsharded
    if (!ShardingState::get(opCtx)->enabled())
        return kUnshardedCollection;

    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kAvailableReadConcern)
        return kUnshardedCollection;

    if (!optReceivedShardVersion && !supportNonVersionedOperations)
        return kUnshardedCollection;

    // Assume that the received shard version was IGNORED if the current operation wasn't versioned
    const auto& receivedShardVersion = optReceivedShardVersion
        ? *optReceivedShardVersion
        : ShardVersionPlacementIgnoredNoIndexes();

    {
        auto criticalSectionSignal = _critSec.getSignal(
            opCtx->lockState()->isWriteLocked() ? ShardingMigrationCriticalSection::kWrite
                                                : ShardingMigrationCriticalSection::kRead);
        std::string reason = _critSec.getReason() ? _critSec.getReason()->toString() : "unknown";
        uassert(StaleConfigInfo(_nss,
                                receivedShardVersion,
                                boost::none /* wantedVersion */,
                                ShardingState::get(opCtx)->shardId(),
                                std::move(criticalSectionSignal),
                                opCtx->lockState()->isWriteLocked()
                                    ? StaleConfigInfo::OperationType::kWrite
                                    : StaleConfigInfo::OperationType::kRead),
                str::stream() << "The critical section for " << _nss.ns()
                              << " is acquired with reason: " << reason,
                !criticalSectionSignal);
    }

    auto optCurrentMetadata = _getCurrentMetadataIfKnown(atClusterTime);
    uassert(StaleConfigInfo(_nss,
                            receivedShardVersion,
                            boost::none /* wantedVersion */,
                            ShardingState::get(opCtx)->shardId()),
            str::stream() << "sharding status of collection " << _nss.ns()
                          << " is not currently known and needs to be recovered",
            optCurrentMetadata);

    const auto& currentMetadata = optCurrentMetadata->get();

    const auto indexFeatureFlag = feature_flags::gGlobalIndexesShardingCatalog.isEnabled(
        serverGlobalParams.featureCompatibility);
    const auto wantedPlacementVersion = currentMetadata.getShardPlacementVersion();
    const auto wantedCollectionIndexes =
        indexFeatureFlag ? getCollectionIndexes(opCtx) : boost::none;
    const auto wantedIndexVersion = wantedCollectionIndexes
        ? boost::make_optional(wantedCollectionIndexes->indexVersion())
        : boost::none;
    const auto wantedShardVersion =
        ShardVersionFactory::make(currentMetadata, wantedCollectionIndexes);

    const ChunkVersion receivedPlacementVersion = receivedShardVersion.placementVersion();
    const bool isPlacementVersionIgnored =
        ShardVersion::isPlacementVersionIgnored(receivedShardVersion);
    const boost::optional<Timestamp> receivedIndexVersion = receivedShardVersion.indexVersion();

    if ((wantedPlacementVersion.isWriteCompatibleWith(receivedPlacementVersion) &&
         (!indexFeatureFlag || receivedIndexVersion == wantedIndexVersion)) ||
        (isPlacementVersionIgnored &&
         (!wantedPlacementVersion.isSet() || !indexFeatureFlag ||
          receivedIndexVersion == wantedIndexVersion)))
        return optCurrentMetadata;

    StaleConfigInfo sci(
        _nss, receivedShardVersion, wantedShardVersion, ShardingState::get(opCtx)->shardId());

    uassert(std::move(sci),
            str::stream() << "timestamp mismatch detected for " << _nss.ns(),
            isPlacementVersionIgnored ||
                wantedPlacementVersion.isSameCollection(receivedPlacementVersion));

    if (isPlacementVersionIgnored ||
        (!wantedPlacementVersion.isSet() && receivedPlacementVersion.isSet())) {
        uasserted(std::move(sci),
                  str::stream() << "this shard no longer contains chunks for " << _nss.ns() << ", "
                                << "the collection may have been dropped");
    }

    if (isPlacementVersionIgnored ||
        (wantedPlacementVersion.isSet() && !receivedPlacementVersion.isSet())) {
        uasserted(std::move(sci),
                  str::stream() << "this shard contains chunks for " << _nss.ns() << ", "
                                << "but the client expects unsharded collection");
    }

    if (isPlacementVersionIgnored ||
        (wantedPlacementVersion.majorVersion() != receivedPlacementVersion.majorVersion())) {
        // Could be > or < - wanted is > if this is the source of a migration, wanted < if this is
        // the target of a migration
        uasserted(std::move(sci),
                  str::stream() << "placement version mismatch detected for " << _nss.ns());
    }

    if (indexFeatureFlag && wantedIndexVersion != receivedIndexVersion) {
        uasserted(std::move(sci),
                  str::stream() << "index version mismatch detected for " << _nss.ns());
    }

    // Those are all the reasons the versions can mismatch
    MONGO_UNREACHABLE;
}

void CollectionShardingRuntime::appendShardVersion(BSONObjBuilder* builder) const {
    auto optCollDescr = getCurrentMetadataIfKnown();
    if (optCollDescr) {
        BSONObjBuilder versionBuilder(builder->subobjStart(_nss.ns()));
        versionBuilder.appendTimestamp("placementVersion",
                                       optCollDescr->getShardPlacementVersion().toLong());
        versionBuilder.append("timestamp", optCollDescr->getShardPlacementVersion().getTimestamp());
    }
}

size_t CollectionShardingRuntime::numberOfRangesScheduledForDeletion() const {
    stdx::lock_guard lk(_metadataManagerLock);
    if (_metadataType == MetadataType::kSharded) {
        return _metadataManager->numberOfRangesScheduledForDeletion();
    }
    return 0;
}


void CollectionShardingRuntime::setPlacementVersionRecoverRefreshFuture(
    SharedSemiFuture<void> future, CancellationSource cancellationSource) {
    invariant(!_placementVersionInRecoverOrRefresh);
    _placementVersionInRecoverOrRefresh.emplace(std::move(future), std::move(cancellationSource));
}

boost::optional<SharedSemiFuture<void>>
CollectionShardingRuntime::getPlacementVersionRecoverRefreshFuture(OperationContext* opCtx) const {
    return _placementVersionInRecoverOrRefresh
        ? boost::optional<SharedSemiFuture<void>>(_placementVersionInRecoverOrRefresh->future)
        : boost::none;
}

void CollectionShardingRuntime::resetPlacementVersionRecoverRefreshFuture() {
    invariant(_placementVersionInRecoverOrRefresh);
    _placementVersionInRecoverOrRefresh = boost::none;
}

boost::optional<CollectionIndexes> CollectionShardingRuntime::getCollectionIndexes(
    OperationContext* opCtx) const {
    _checkCritSecForIndexMetadata(opCtx);

    return _shardingIndexesCatalogInfo
        ? boost::make_optional(_shardingIndexesCatalogInfo->getCollectionIndexes())
        : boost::none;
}

boost::optional<ShardingIndexesCatalogCache> CollectionShardingRuntime::getIndexes(
    OperationContext* opCtx) const {
    _checkCritSecForIndexMetadata(opCtx);
    return _shardingIndexesCatalogInfo;
}

void CollectionShardingRuntime::addIndex(OperationContext* opCtx,
                                         const IndexCatalogType& index,
                                         const CollectionIndexes& collectionIndexes) {
    if (_shardingIndexesCatalogInfo) {
        _shardingIndexesCatalogInfo->add(index, collectionIndexes);
    } else {
        IndexCatalogTypeMap indexMap;
        indexMap.emplace(index.getName(), index);
        _shardingIndexesCatalogInfo.emplace(collectionIndexes, std::move(indexMap));
    }
}

void CollectionShardingRuntime::removeIndex(OperationContext* opCtx,
                                            const std::string& name,
                                            const CollectionIndexes& collectionIndexes) {
    tassert(7019500,
            "Index information does not exist on CSR",
            _shardingIndexesCatalogInfo.is_initialized());
    _shardingIndexesCatalogInfo->remove(name, collectionIndexes);
}

void CollectionShardingRuntime::clearIndexes(OperationContext* opCtx) {
    _shardingIndexesCatalogInfo = boost::none;
}

void CollectionShardingRuntime::replaceIndexes(OperationContext* opCtx,
                                               const std::vector<IndexCatalogType>& indexes,
                                               const CollectionIndexes& collectionIndexes) {
    if (_shardingIndexesCatalogInfo) {
        _shardingIndexesCatalogInfo = boost::none;
    }
    IndexCatalogTypeMap indexMap;
    for (const auto& index : indexes) {
        indexMap.emplace(index.getName(), index);
    }
    _shardingIndexesCatalogInfo.emplace(collectionIndexes, std::move(indexMap));
}

CollectionCriticalSection::CollectionCriticalSection(OperationContext* opCtx,
                                                     NamespaceString nss,
                                                     BSONObj reason)
    : _opCtx(opCtx), _nss(std::move(nss)), _reason(std::move(reason)) {
    // This acquisition is performed with collection lock MODE_S in order to ensure that any ongoing
    // writes have completed and become visible
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_S,
                               AutoGetCollection::Options{}.deadline(
                                   _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load())));
    auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(_opCtx, _nss);
    tassert(7032305,
            "Collection metadata unknown when entering critical section",
            scopedCsr->getCurrentMetadataIfKnown());
    scopedCsr->enterCriticalSectionCatchUpPhase(_reason);
}

CollectionCriticalSection::~CollectionCriticalSection() {
    // TODO (SERVER-71444): Fix to be interruptible or document exception.
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());  // NOLINT.
    AutoGetCollection autoColl(_opCtx, _nss, MODE_IX);
    auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(_opCtx, _nss);
    scopedCsr->exitCriticalSection(_reason);
}

void CollectionCriticalSection::enterCommitPhase() {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_X,
                               AutoGetCollection::Options{}.deadline(
                                   _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load())));
    auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(_opCtx, _nss);
    tassert(7032304,
            "Collection metadata unknown when entering critical section commit phase",
            scopedCsr->getCurrentMetadataIfKnown());
    scopedCsr->enterCriticalSectionCommitPhase(_reason);
}

void CollectionShardingRuntime::_cleanupBeforeInstallingNewCollectionMetadata(
    WithLock, OperationContext* opCtx) {
    if (!_metadataManager) {
        // The old collection metadata was unsharded, nothing to cleanup so far.
        return;
    }

    const auto oldUUID = _metadataManager->getCollectionUuid();
    const auto oldShardVersion = _metadataManager->getActivePlacementVersion();
    ExecutorFuture<void>{Grid::get(opCtx)->getExecutorPool()->getFixedExecutor()}
        .then([svcCtx{opCtx->getServiceContext()}, oldUUID, oldShardVersion] {
            ThreadClient tc{"CleanUpShardedMetadata", svcCtx};
            {
                stdx::lock_guard<Client> lk{*tc.get()};
                tc->setSystemOperationKillableByStepdown(lk);
            }
            auto uniqueOpCtx{tc->makeOperationContext()};
            auto opCtx{uniqueOpCtx.get()};

            try {
                auto& planCache = sbe::getPlanCache(opCtx);
                planCache.removeIf(
                    [&](const sbe::PlanCacheKey& key, const sbe::PlanCacheEntry& entry) -> bool {
                        const auto matchingCollState =
                            [&](const sbe::PlanCacheKeyCollectionState& entryCollState) {
                                return entryCollState.uuid == oldUUID &&
                                    entryCollState.collectionGeneration &&
                                    entryCollState.collectionGeneration->epoch ==
                                    oldShardVersion.epoch() &&
                                    entryCollState.collectionGeneration->ts ==
                                    oldShardVersion.getTimestamp();
                            };

                        // Check whether the main collection of this plan is the one being removed
                        if (matchingCollState(key.getMainCollectionState()))
                            return true;

                        // Check whether a secondary collection is the one being removed
                        for (const auto& secCollState : key.getSecondaryCollectionStates()) {
                            if (matchingCollState(secCollState))
                                return true;
                        }

                        return false;
                    });
            } catch (const DBException& ex) {
                LOGV2(6549200,
                      "Interrupted deferred clean up of sharded metadata",
                      "error"_attr = redact(ex));
            }
        })
        .getAsync([](auto) {});
}

void CollectionShardingRuntime::_checkCritSecForIndexMetadata(OperationContext* opCtx) const {
    if (!ShardingState::get(opCtx)->enabled())
        return;

    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kAvailableReadConcern)
        return;

    const auto optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);

    // Assume that the received shard version was IGNORED if the current operation wasn't
    // versioned
    const auto& receivedShardVersion = optReceivedShardVersion
        ? *optReceivedShardVersion
        : ShardVersionPlacementIgnoredNoIndexes();
    auto criticalSectionSignal = _critSec.getSignal(opCtx->lockState()->isWriteLocked()
                                                        ? ShardingMigrationCriticalSection::kWrite
                                                        : ShardingMigrationCriticalSection::kRead);
    std::string reason = _critSec.getReason() ? _critSec.getReason()->toString() : "unknown";
    uassert(StaleConfigInfo(_nss,
                            receivedShardVersion,
                            boost::none /* wantedVersion */,
                            ShardingState::get(opCtx)->shardId(),
                            std::move(criticalSectionSignal),
                            opCtx->lockState()->isWriteLocked()
                                ? StaleConfigInfo::OperationType::kWrite
                                : StaleConfigInfo::OperationType::kRead),
            str::stream() << "The critical section for " << _nss.ns()
                          << " is acquired with reason: " << reason,
            !criticalSectionSignal);
}

}  // namespace mongo
