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
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/log.h"

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

    // Local and admin never have sharded collections
    if (nss.db() == NamespaceString::kLocalDb || nss.db() == NamespaceString::kAdminDb)
        return true;

    // Certain config collections can never be sharded
    if (nss == NamespaceString::kSessionTransactionsTableNamespace)
        return true;

    if (nss.isSystemDotProfile())
        return true;

    return false;
}

}  // namespace

CollectionShardingRuntime::CollectionShardingRuntime(ServiceContext* sc,
                                                     NamespaceString nss,
                                                     executor::TaskExecutor* rangeDeleterExecutor)
    : CollectionShardingState(nss),
      _nss(std::move(nss)),
      _metadataManager(std::make_shared<MetadataManager>(sc, _nss, rangeDeleterExecutor)) {
    if (isNamespaceAlwaysUnsharded(_nss)) {
        _metadataManager->setFilteringMetadata(CollectionMetadata());
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

void CollectionShardingRuntime::setFilteringMetadata(OperationContext* opCtx,
                                                     CollectionMetadata newMetadata) {
    invariant(!newMetadata.isSharded() || !isNamespaceAlwaysUnsharded(_nss),
              str::stream() << "Namespace " << _nss.ns() << " must never be sharded.");

    auto csrLock = CollectionShardingState::CSRLock::lockExclusive(opCtx, this);

    _metadataManager->setFilteringMetadata(std::move(newMetadata));
}

void CollectionShardingRuntime::clearFilteringMetadata() {
    if (!isNamespaceAlwaysUnsharded(_nss)) {
        _metadataManager->clearFilteringMetadata();
    }
}

auto CollectionShardingRuntime::beginReceive(ChunkRange const& range) -> CleanupNotification {
    return _metadataManager->beginReceive(range);
}

void CollectionShardingRuntime::forgetReceive(const ChunkRange& range) {
    _metadataManager->forgetReceive(range);
}

auto CollectionShardingRuntime::cleanUpRange(ChunkRange const& range, CleanWhen when)
    -> CleanupNotification {
    Date_t time =
        (when == kNow) ? Date_t{} : Date_t::now() + Seconds(orphanCleanupDelaySecs.load());
    return _metadataManager->cleanUpRange(range, time);
}

Status CollectionShardingRuntime::waitForClean(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               OID const& epoch,
                                               ChunkRange orphanRange) {
    while (true) {
        boost::optional<CleanupNotification> stillScheduled;

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            auto* const self = CollectionShardingRuntime::get(opCtx, nss);

            {
                // First, see if collection was dropped, but do it in a separate scope in order to
                // not hold reference on it, which would make it appear in use
                const auto optMetadata =
                    self->_metadataManager->getActiveMetadata(self->_metadataManager, boost::none);
                if (!optMetadata)
                    return {ErrorCodes::ConflictingOperationInProgress,
                            "Collection being migrated had its metadata reset"};

                const auto& metadata = *optMetadata;
                if (!metadata->isSharded() || metadata->getCollVersion().epoch() != epoch) {
                    return {ErrorCodes::ConflictingOperationInProgress,
                            "Collection being migrated was dropped"};
                }
            }

            stillScheduled = self->trackOrphanedDataCleanup(orphanRange);
            if (!stillScheduled) {
                log() << "Finished deleting " << nss.ns() << " range "
                      << redact(orphanRange.toString());
                return Status::OK();
            }
        }

        log() << "Waiting for deletion of " << nss.ns() << " range " << orphanRange;

        Status result = stillScheduled->waitStatus(opCtx);
        if (!result.isOK()) {
            return result.withContext(str::stream() << "Failed to delete orphaned " << nss.ns()
                                                    << " range " << orphanRange.toString());
        }
    }

    MONGO_UNREACHABLE;
}

auto CollectionShardingRuntime::trackOrphanedDataCleanup(ChunkRange const& range)
    -> boost::optional<CleanupNotification> {
    return _metadataManager->trackOrphanedDataCleanup(range);
}

boost::optional<ChunkRange> CollectionShardingRuntime::getNextOrphanRange(BSONObj const& from) {
    return _metadataManager->getNextOrphanRange(from);
}

boost::optional<ScopedCollectionMetadata> CollectionShardingRuntime::_getMetadata(
    const boost::optional<mongo::LogicalTime>& atClusterTime) {
    return _metadataManager->getActiveMetadata(_metadataManager, atClusterTime);
}

CollectionCriticalSection::CollectionCriticalSection(OperationContext* opCtx, NamespaceString ns)
    : _nss(std::move(ns)), _opCtx(opCtx) {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_IX,
                               MODE_X,
                               AutoGetCollection::ViewMode::kViewsForbidden,
                               opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingState::CSRLock::lockExclusive(_opCtx, csr);

    csr->enterCriticalSectionCatchUpPhase(_opCtx, csrLock);
}

CollectionCriticalSection::~CollectionCriticalSection() {
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    AutoGetCollection autoColl(_opCtx, _nss, MODE_IX, MODE_IX);
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);

    csr->exitCriticalSection(_opCtx, csrLock);
}

void CollectionCriticalSection::enterCommitPhase() {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_IX,
                               MODE_X,
                               AutoGetCollection::ViewMode::kViewsForbidden,
                               _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);

    csr->enterCriticalSectionCommitPhase(_opCtx, csrLock);
}

}  // namespace mongo
