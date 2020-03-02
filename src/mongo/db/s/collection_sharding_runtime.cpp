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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/collection_sharding_runtime.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
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
    auto& oss = OperationShardingState::get(opCtx);

    // If there is a version attached to the OperationContext, use it as the received version,
    // otherwise get the received version from the ShardedConnectionInfo
    if (oss.hasShardVersion()) {
        return oss.getShardVersion(nss);
    } else if (auto const info = ShardedConnectionInfo::get(opCtx->getClient(), false)) {
        auto connectionShardVersion = info->getVersion(nss.ns());

        // For backwards compatibility with map/reduce, which can access up to 2 sharded collections
        // in a single call, the lack of version for a namespace on the collection must be treated
        // as UNSHARDED
        return connectionShardVersion.value_or(ChunkVersion::UNSHARDED());
    }

    // There is no shard version information on either 'opCtx' or 'client'. This means that the
    // operation represented by 'opCtx' is unversioned, and the shard version is always OK for
    // unversioned operations
    return boost::none;
}

}  // namespace

CollectionShardingRuntime::CollectionShardingRuntime(
    ServiceContext* sc,
    NamespaceString nss,
    std::shared_ptr<executor::TaskExecutor> rangeDeleterExecutor)
    : _nss(std::move(nss)),
      _rangeDeleterExecutor(rangeDeleterExecutor),
      _stateChangeMutex(nss.toString()) {
    if (isNamespaceAlwaysUnsharded(_nss)) {
        _metadataType = MetadataType::kUnsharded;
    }
}

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

ScopedCollectionFilter CollectionShardingRuntime::getOwnershipFilter(OperationContext* opCtx) {
    const auto optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);
    if (!optReceivedShardVersion)
        return {kUnshardedCollection};

    const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    auto optMetadata = _getMetadataWithVersionCheckAt(opCtx, atClusterTime);

    uassert(StaleConfigInfo(_nss,
                            *optReceivedShardVersion,
                            getCurrentShardVersionIfKnown(),
                            ShardingState::get(opCtx)->shardId()),
            str::stream() << "sharding status of collection " << _nss.ns()
                          << " is not currently available for filtering and needs to be recovered "
                          << "from the config server",
            optMetadata);

    return {std::move(*optMetadata)};
}

ScopedCollectionDescription CollectionShardingRuntime::getCollectionDescription() {
    auto optMetadata = _getCurrentMetadataIfKnown(boost::none);
    if (!optMetadata)
        return {kUnshardedCollection};

    return *optMetadata;
}

boost::optional<ScopedCollectionDescription>
CollectionShardingRuntime::getCurrentMetadataIfKnown() {
    return _getCurrentMetadataIfKnown(boost::none);
}

boost::optional<ChunkVersion> CollectionShardingRuntime::getCurrentShardVersionIfKnown() {
    stdx::lock_guard lk(_metadataManagerLock);
    switch (_metadataType) {
        case MetadataType::kUnknown:
            return boost::none;
        case MetadataType::kUnsharded:
            return ChunkVersion::UNSHARDED();
        case MetadataType::kSharded:
            return _metadataManager->getActiveShardVersion();
    };
    MONGO_UNREACHABLE;
}

void CollectionShardingRuntime::checkShardVersionOrThrow(OperationContext* opCtx) {
    (void)_getMetadataWithVersionCheckAt(opCtx, boost::none);
}

Status CollectionShardingRuntime::checkShardVersionNoThrow(OperationContext* opCtx) noexcept {
    try {
        checkShardVersionOrThrow(opCtx);
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void CollectionShardingRuntime::enterCriticalSectionCatchUpPhase(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_X));
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, this);
    _critSec.enterCriticalSectionCatchUpPhase();
}

void CollectionShardingRuntime::enterCriticalSectionCommitPhase(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_X));
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, this);
    _critSec.enterCriticalSectionCommitPhase();
}

void CollectionShardingRuntime::exitCriticalSection(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_IX));
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, this);
    _critSec.exitCriticalSection();
}

std::shared_ptr<Notification<void>> CollectionShardingRuntime::getCriticalSectionSignal(
    ShardingMigrationCriticalSection::Operation op) const {
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
              "Marking collection {nss_ns} as {newMetadata_Basic}",
              "nss_ns"_attr = _nss.ns(),
              "newMetadata_Basic"_attr = newMetadata.toStringBasic());
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

void CollectionShardingRuntime::clearFilteringMetadata() {
    stdx::lock_guard lk(_metadataManagerLock);
    if (!isNamespaceAlwaysUnsharded(_nss)) {
        _metadataType = MetadataType::kUnknown;
        _metadataManager.reset();
    }
}

SharedSemiFuture<void> CollectionShardingRuntime::beginReceive(ChunkRange const& range) {
    stdx::lock_guard lk(_metadataManagerLock);
    invariant(_metadataType == MetadataType::kSharded);
    return _metadataManager->beginReceive(range);
}

void CollectionShardingRuntime::forgetReceive(const ChunkRange& range) {
    stdx::lock_guard lk(_metadataManagerLock);
    invariant(_metadataType == MetadataType::kSharded);
    _metadataManager->forgetReceive(range);
}
SharedSemiFuture<void> CollectionShardingRuntime::cleanUpRange(ChunkRange const& range,
                                                               CleanWhen when) {
    stdx::lock_guard lk(_metadataManagerLock);
    invariant(_metadataType == MetadataType::kSharded);
    return _metadataManager->cleanUpRange(range, when == kDelayed);
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
                              "Finished waiting for deletion of {nss_ns} range {orphanRange}",
                              "nss_ns"_attr = nss.ns(),
                              "orphanRange"_attr = redact(orphanRange.toString()));
                return Status::OK();
            }
        }

        LOGV2_OPTIONS(21919,
                      {logv2::LogComponent::kShardingMigration},
                      "Waiting for deletion of {nss_ns} range {orphanRange}",
                      "nss_ns"_attr = nss.ns(),
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

boost::optional<ChunkRange> CollectionShardingRuntime::getNextOrphanRange(BSONObj const& from) {
    stdx::lock_guard lk(_metadataManagerLock);
    invariant(_metadataType == MetadataType::kSharded);
    return _metadataManager->getNextOrphanRange(from);
}

boost::optional<ScopedCollectionDescription> CollectionShardingRuntime::_getCurrentMetadataIfKnown(
    const boost::optional<LogicalTime>& atClusterTime) {
    stdx::lock_guard lk(_metadataManagerLock);
    switch (_metadataType) {
        case MetadataType::kUnknown:
            return boost::none;
        case MetadataType::kUnsharded:
            return ScopedCollectionDescription{kUnshardedCollection};
        case MetadataType::kSharded:
            return _metadataManager->getActiveMetadata(atClusterTime);
    };
    MONGO_UNREACHABLE;
}

boost::optional<ScopedCollectionDescription>
CollectionShardingRuntime::_getMetadataWithVersionCheckAt(
    OperationContext* opCtx, const boost::optional<mongo::LogicalTime>& atClusterTime) {
    const auto optReceivedShardVersion = getOperationReceivedVersion(opCtx, _nss);
    if (!optReceivedShardVersion) {
        return boost::none;
    }

    auto const shardId = ShardingState::get(opCtx)->shardId();

    const auto& receivedShardVersion = *optReceivedShardVersion;

    // An operation with read concern 'available' should never have shardVersion set.
    invariant(repl::ReadConcernArgs::get(opCtx).getLevel() !=
              repl::ReadConcernLevel::kAvailableReadConcern);

    auto csrLock = CSRLock::lockShared(opCtx, this);

    auto wantedShardVersion = [&] {
        auto optionalWantedShardVersion = getCurrentShardVersionIfKnown();
        return optionalWantedShardVersion ? *optionalWantedShardVersion : ChunkVersion::UNSHARDED();
    }();

    auto criticalSectionSignal = [&] {
        return _critSec.getSignal(opCtx->lockState()->isWriteLocked()
                                      ? ShardingMigrationCriticalSection::kWrite
                                      : ShardingMigrationCriticalSection::kRead);
    }();

    if (criticalSectionSignal) {
        uasserted(
            StaleConfigInfo(
                _nss, receivedShardVersion, wantedShardVersion, shardId, criticalSectionSignal),
            str::stream() << "migration commit in progress for " << _nss.ns());
    }

    //
    // Figure out exactly why not compatible, send appropriate error message
    // The versions themselves are returned in the error, so not needed in messages here
    //

    StaleConfigInfo sci(_nss, receivedShardVersion, wantedShardVersion, shardId);

    if (ChunkVersion::isIgnoredVersion(receivedShardVersion)) {
        uassert(std::move(sci),
                str::stream()
                    << "sharding status of collection " << _nss.ns()
                    << " is not currently known and needs to be recovered from the config server",
                !receivedShardVersion.getCanThrowSSVOnIgnored() ||
                    _getCurrentMetadataIfKnown(atClusterTime));
        return boost::none;
    }

    if (receivedShardVersion.isWriteCompatibleWith(wantedShardVersion)) {
        return _getCurrentMetadataIfKnown(atClusterTime);
    }

    uassert(std::move(sci),
            str::stream() << "epoch mismatch detected for " << _nss.ns() << ", "
                          << "the collection may have been dropped and recreated",
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

void CollectionShardingRuntime::appendInfoForServerStatus(BSONArrayBuilder* builder) {
    stdx::lock_guard lk(_metadataManagerLock);
    if (_metadataManager) {
        _metadataManager->appendForServerStatus(builder);
    }
}

CollectionCriticalSection::CollectionCriticalSection(OperationContext* opCtx, NamespaceString ns)
    : _nss(std::move(ns)), _opCtx(opCtx) {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_X,
                               AutoGetCollection::ViewMode::kViewsForbidden,
                               opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    csr->enterCriticalSectionCatchUpPhase(_opCtx);
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
                               AutoGetCollection::ViewMode::kViewsForbidden,
                               _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    csr->enterCriticalSectionCommitPhase(_opCtx);
}

}  // namespace mongo
