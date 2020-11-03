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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_sharding_runtime.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

/**
 * Returns whether the specified namespace is used for sharding-internal purposes only and can never
 * be marked as anything other than UNSHARDED, because the call sites which reference these
 * collections are not prepared to handle StaleConfig errors.
 */
bool isNamespaceAlwaysUnsharded(const NamespaceString& nss) {
    // There should never be a case to mark as sharded collections which are on the config server
    if (serverGlobalParams.clusterRole != ClusterRole::ShardServer)
        return true;

    return nss.isNamespaceAlwaysUnsharded();
}

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

boost::optional<ChunkVersion> getOperationReceivedVersion(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    // If there is a version attached to the OperationContext, use it as the received version.
    if (OperationShardingState::isOperationVersioned(opCtx)) {
        return OperationShardingState::get(opCtx).getShardVersion(nss);
    }

    // There is no shard version information on the 'opCtx'. This means that the operation
    // represented by 'opCtx' is unversioned, and the shard version is always OK for unversioned
    // operations.
    return boost::none;
}

}  // namespace

CollectionShardingRuntime::CollectionShardingRuntime(
    ServiceContext* service,
    NamespaceString nss,
    std::shared_ptr<executor::TaskExecutor> rangeDeleterExecutor)
    : _serviceContext(service),
      _nss(std::move(nss)),
      _rangeDeleterExecutor(std::move(rangeDeleterExecutor)),
      _stateChangeMutex(_nss.toString()),
      _metadataType(isNamespaceAlwaysUnsharded(_nss) ? MetadataType::kUnsharded
                                                     : MetadataType::kUnknown) {}

CollectionShardingRuntime* CollectionShardingRuntime::get(OperationContext* opCtx,
                                                          const NamespaceString& nss) {
    auto* const css = CollectionShardingState::get(opCtx, nss);
    return checked_cast<CollectionShardingRuntime*>(css);
}

CollectionShardingRuntime* CollectionShardingRuntime::get_UNSAFE(ServiceContext* svcCtx,
                                                                 const NamespaceString& nss) {
    auto* const css = CollectionShardingState::get_UNSAFE(svcCtx, nss);
    return checked_cast<CollectionShardingRuntime*>(css);
}

ScopedCollectionFilter CollectionShardingRuntime::getOwnershipFilter(
    OperationContext* opCtx, OrphanCleanupPolicy orphanCleanupPolicy) {
    const auto optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);
    // TODO (SERVER-52764): No operations should be calling getOwnershipFilter without a shard
    // version
    //
    // invariant(optReceivedShardVersion,
    //          "getOwnershipFilter called by operation that doesn't specify shard version");
    if (!optReceivedShardVersion)
        return {kUnshardedCollection};

    auto metadata = _getMetadataWithVersionCheckAt(
        opCtx, repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime());
    invariant(!ChunkVersion::isIgnoredVersion(*optReceivedShardVersion) ||
                  !metadata->get().allowMigrations() || !metadata->get().isSharded(),
              "For sharded collections getOwnershipFilter cannot be relied on without a valid "
              "shard version");

    return {std::move(metadata)};
}

ScopedCollectionDescription CollectionShardingRuntime::getCollectionDescription(
    OperationContext* opCtx) {
    auto& oss = OperationShardingState::get(opCtx);
    // If the server has been started with --shardsvr, but hasn't been added to a cluster we should
    // consider all collections as unsharded. Also, return unsharded if no shard version or db
    // version is present on the context.
    if (!ShardingState::get(_serviceContext)->enabled() ||
        (!OperationShardingState::isOperationVersioned(opCtx) && !oss.hasDbVersion())) {
        return {kUnshardedCollection};
    }

    auto optMetadata = _getCurrentMetadataIfKnown(boost::none);
    uassert(
        StaleConfigInfo(_nss,
                        ChunkVersion::UNSHARDED(),
                        boost::none,
                        ShardingState::get(_serviceContext)->shardId()),
        str::stream() << "sharding status of collection " << _nss.ns()
                      << " is not currently available for description and needs to be recovered "
                      << "from the config server",
        optMetadata);

    return {std::move(optMetadata)};
}

boost::optional<CollectionMetadata> CollectionShardingRuntime::getCurrentMetadataIfKnown() {
    auto optMetadata = _getCurrentMetadataIfKnown(boost::none);
    if (!optMetadata)
        return boost::none;
    return optMetadata->get();
}

void CollectionShardingRuntime::checkShardVersionOrThrow(OperationContext* opCtx) {
    (void)_getMetadataWithVersionCheckAt(opCtx, boost::none);
}

void CollectionShardingRuntime::enterCriticalSectionCatchUpPhase(const CSRLock&) {
    invariant(_metadataType != MetadataType::kUnknown);
    _critSec.enterCriticalSectionCatchUpPhase();
}

void CollectionShardingRuntime::enterCriticalSectionCommitPhase(const CSRLock&) {
    invariant(_metadataType != MetadataType::kUnknown);
    _critSec.enterCriticalSectionCommitPhase();
}

void CollectionShardingRuntime::exitCriticalSection(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_IX));
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, this);
    _critSec.exitCriticalSection();
}

boost::optional<SharedSemiFuture<void>> CollectionShardingRuntime::getCriticalSectionSignal(
    OperationContext* opCtx, ShardingMigrationCriticalSection::Operation op) {
    auto csrLock = CSRLock::lockShared(opCtx, this);
    return _critSec.getSignal(op);
}

void CollectionShardingRuntime::setFilteringMetadata(OperationContext* opCtx,
                                                     CollectionMetadata newMetadata) {
    invariant(!newMetadata.isSharded() || !isNamespaceAlwaysUnsharded(_nss),
              str::stream() << "Namespace " << _nss.ns() << " must never be sharded.");

    auto csrLock = CSRLock::lockExclusive(opCtx, this);
    stdx::lock_guard lk(_metadataManagerLock);

    if (!newMetadata.isSharded()) {
        LOGV2(21917,
              "Marking collection {namespace} as unsharded",
              "Marking collection as unsharded",
              "namespace"_attr = _nss.ns());
        _metadataType = MetadataType::kUnsharded;
        _metadataManager.reset();
        ++_numMetadataManagerChanges;
    } else if (!_metadataManager ||
               !newMetadata.uuidMatches(_metadataManager->getCollectionUuid())) {
        _metadataType = MetadataType::kSharded;
        _metadataManager = std::make_shared<MetadataManager>(
            opCtx->getServiceContext(), _nss, _rangeDeleterExecutor, newMetadata);
        ++_numMetadataManagerChanges;
    } else {
        _metadataManager->setFilteringMetadata(std::move(newMetadata));
    }
}

void CollectionShardingRuntime::clearFilteringMetadata(OperationContext* opCtx) {
    const auto csrLock = CSRLock::lockExclusive(opCtx, this);
    stdx::lock_guard lk(_metadataManagerLock);
    if (!isNamespaceAlwaysUnsharded(_nss)) {
        LOGV2_DEBUG(4798530,
                    1,
                    "Clearing metadata for collection {namespace}",
                    "Clearing collection metadata",
                    "namespace"_attr = _nss);
        _metadataType = MetadataType::kUnknown;
        _metadataManager.reset();
    }
}

SharedSemiFuture<void> CollectionShardingRuntime::cleanUpRange(ChunkRange const& range,
                                                               boost::optional<UUID> migrationId,
                                                               CleanWhen when) {
    stdx::lock_guard lk(_metadataManagerLock);
    invariant(_metadataType == MetadataType::kSharded);
    return _metadataManager->cleanUpRange(range, std::move(migrationId), when == kDelayed);
}

Status CollectionShardingRuntime::waitForClean(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const UUID& collectionUuid,
                                               ChunkRange orphanRange) {
    while (true) {
        boost::optional<SharedSemiFuture<void>> stillScheduled;

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            auto* const self = CollectionShardingRuntime::get(opCtx, nss);
            stdx::lock_guard lk(self->_metadataManagerLock);

            // If the metadata was reset, or the collection was dropped and recreated since the
            // metadata manager was created, return an error.
            if (!self->_metadataManager ||
                (collectionUuid != self->_metadataManager->getCollectionUuid())) {
                return {ErrorCodes::ConflictingOperationInProgress,
                        "Collection being migrated was dropped and created or otherwise had its "
                        "metadata reset"};
            }

            stillScheduled = self->_metadataManager->trackOrphanedDataCleanup(orphanRange);
            if (!stillScheduled) {
                LOGV2_OPTIONS(21918,
                              {logv2::LogComponent::kShardingMigration},
                              "Finished waiting for deletion of {namespace} range {orphanRange}",
                              "Finished waiting for deletion of orphans",
                              "namespace"_attr = nss.ns(),
                              "orphanRange"_attr = redact(orphanRange.toString()));
                return Status::OK();
            }
        }

        LOGV2_OPTIONS(21919,
                      {logv2::LogComponent::kShardingMigration},
                      "Waiting for deletion of {namespace} range {orphanRange}",
                      "Waiting for deletion of orphans",
                      "namespace"_attr = nss.ns(),
                      "orphanRange"_attr = orphanRange);

        Status result = stillScheduled->getNoThrow(opCtx);

        // Swallow RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist error since the
        // collection could either never exist or get dropped directly from the shard after
        // the range deletion task got scheduled.
        if (!result.isOK() &&
            result != ErrorCodes::RangeDeletionAbandonedBecauseCollectionWithUUIDDoesNotExist) {
            return result.withContext(str::stream() << "Failed to delete orphaned " << nss.ns()
                                                    << " range " << orphanRange.toString());
        }
    }

    MONGO_UNREACHABLE;
}

std::shared_ptr<ScopedCollectionDescription::Impl>
CollectionShardingRuntime::_getCurrentMetadataIfKnown(
    const boost::optional<LogicalTime>& atClusterTime) {
    stdx::lock_guard lk(_metadataManagerLock);
    switch (_metadataType) {
        case MetadataType::kUnknown:
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
    OperationContext* opCtx, const boost::optional<mongo::LogicalTime>& atClusterTime) {
    const auto optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);
    if (!optReceivedShardVersion)
        return kUnshardedCollection;

    const auto& receivedShardVersion = *optReceivedShardVersion;

    // An operation with read concern 'available' should never have shardVersion set.
    invariant(repl::ReadConcernArgs::get(opCtx).getLevel() !=
              repl::ReadConcernLevel::kAvailableReadConcern);

    auto csrLock = CSRLock::lockShared(opCtx, this);

    auto optCurrentMetadata = _getCurrentMetadataIfKnown(atClusterTime);
    uassert(StaleConfigInfo(
                _nss, receivedShardVersion, boost::none, ShardingState::get(opCtx)->shardId()),
            str::stream() << "sharding status of collection " << _nss.ns()
                          << " is not currently known and needs to be recovered",
            optCurrentMetadata);

    const auto& currentMetadata = optCurrentMetadata->get();
    auto wantedShardVersion = currentMetadata.getShardVersion();

    {
        auto criticalSectionSignal = _critSec.getSignal(
            opCtx->lockState()->isWriteLocked() ? ShardingMigrationCriticalSection::kWrite
                                                : ShardingMigrationCriticalSection::kRead);

        uassert(StaleConfigInfo(_nss,
                                receivedShardVersion,
                                wantedShardVersion,
                                ShardingState::get(opCtx)->shardId(),
                                std::move(criticalSectionSignal)),
                str::stream() << "migration commit in progress for " << _nss.ns(),
                !criticalSectionSignal);
    }

    if (wantedShardVersion.isWriteCompatibleWith(receivedShardVersion) ||
        ChunkVersion::isIgnoredVersion(receivedShardVersion))
        return optCurrentMetadata;

    StaleConfigInfo sci(
        _nss, receivedShardVersion, wantedShardVersion, ShardingState::get(opCtx)->shardId());

    uassert(std::move(sci),
            str::stream() << "epoch mismatch detected for " << _nss.ns(),
            wantedShardVersion.epoch() == receivedShardVersion.epoch());

    if (!wantedShardVersion.isSet() && receivedShardVersion.isSet()) {
        uasserted(std::move(sci),
                  str::stream() << "this shard no longer contains chunks for " << _nss.ns() << ", "
                                << "the collection may have been dropped");
    }

    if (wantedShardVersion.isSet() && !receivedShardVersion.isSet()) {
        uasserted(std::move(sci),
                  str::stream() << "this shard contains chunks for " << _nss.ns() << ", "
                                << "but the client expects unsharded collection");
    }

    if (wantedShardVersion.majorVersion() != receivedShardVersion.majorVersion()) {
        // Could be > or < - wanted is > if this is the source of a migration, wanted < if this is
        // the target of a migration
        uasserted(std::move(sci), str::stream() << "version mismatch detected for " << _nss.ns());
    }

    // Those are all the reasons the versions can mismatch
    MONGO_UNREACHABLE;
}

void CollectionShardingRuntime::appendShardVersion(BSONObjBuilder* builder) {
    auto optCollDescr = getCurrentMetadataIfKnown();
    if (optCollDescr) {
        builder->appendTimestamp(_nss.ns(), optCollDescr->getShardVersion().toLong());
    }
}

size_t CollectionShardingRuntime::numberOfRangesScheduledForDeletion() const {
    stdx::lock_guard lk(_metadataManagerLock);
    if (_metadataManager) {
        return _metadataManager->numberOfRangesScheduledForDeletion();
    }
    return 0;
}


void CollectionShardingRuntime::setShardVersionRecoverRefreshFuture(SharedSemiFuture<void> future,
                                                                    const CSRLock&) {
    invariant(!_shardVersionInRecoverOrRefresh);
    _shardVersionInRecoverOrRefresh.emplace(std::move(future));
}

boost::optional<SharedSemiFuture<void>>
CollectionShardingRuntime::getShardVersionRecoverRefreshFuture(OperationContext* opCtx) {
    auto csrLock = CSRLock::lockShared(opCtx, this);
    return _shardVersionInRecoverOrRefresh;
}

void CollectionShardingRuntime::resetShardVersionRecoverRefreshFuture(const CSRLock&) {
    invariant(_shardVersionInRecoverOrRefresh);
    _shardVersionInRecoverOrRefresh = boost::none;
}

CollectionCriticalSection::CollectionCriticalSection(OperationContext* opCtx, NamespaceString nss)
    : _opCtx(opCtx), _nss(std::move(nss)) {
    // This acquisition is performed with collection lock MODE_S in order to ensure that any ongoing
    // writes have completed and become visible
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_S,
                               AutoGetCollectionViewMode::kViewsForbidden,
                               _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
    invariant(csr->getCurrentMetadataIfKnown());
    csr->enterCriticalSectionCatchUpPhase(csrLock);
}

CollectionCriticalSection::~CollectionCriticalSection() {
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    AutoGetCollection autoColl(_opCtx, _nss, MODE_IX);
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    csr->exitCriticalSection(_opCtx);
}

void CollectionCriticalSection::enterCommitPhase() {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_X,
                               AutoGetCollectionViewMode::kViewsForbidden,
                               _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);
    invariant(csr->getCurrentMetadataIfKnown());
    csr->enterCriticalSectionCommitPhase(csrLock);
}

}  // namespace mongo
