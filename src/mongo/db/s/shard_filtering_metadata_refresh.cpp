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

#include "mongo/db/s/shard_filtering_metadata_refresh.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(skipDatabaseVersionMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(skipShardFilteringMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(hangInRecoverRefreshThread);

namespace {
void onDbVersionMismatch(OperationContext* opCtx,
                         const StringData dbName,
                         const DatabaseVersion& clientDbVersion,
                         const boost::optional<DatabaseVersion>& serverDbVersion) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    invariant(ShardingState::get(opCtx)->canAcceptShardedCommands());

    if (serverDbVersion && serverDbVersion->getUuid() == clientDbVersion.getUuid() &&
        serverDbVersion->getLastMod() >= clientDbVersion.getLastMod()) {
        // The client was stale; do not trigger server-side refresh.
        return;
    }

    // Ensure any ongoing movePrimary's have completed before trying to do the refresh. This wait is
    // just an optimization so that mongos does not exhaust its maximum number of
    // StaleDatabaseVersion retry attempts while the movePrimary is being committed.
    OperationShardingState::get(opCtx).waitForMovePrimaryCriticalSectionSignal(opCtx);

    if (MONGO_unlikely(skipDatabaseVersionMetadataRefresh.shouldFail())) {
        return;
    }

    forceDatabaseRefresh(opCtx, dbName);
}

SharedSemiFuture<void> recoverRefreshShardVersion(ServiceContext* serviceContext,
                                                  const NamespaceString nss,
                                                  bool runRecover) {
    return ExecutorFuture<void>(Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor())
        .then([=] {
            ThreadClient tc("RecoverRefreshThread", serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillable(lk);
            }

            if (MONGO_unlikely(hangInRecoverRefreshThread.shouldFail())) {
                hangInRecoverRefreshThread.pauseWhileSet();
            }

            auto opCtx = tc->makeOperationContext();

            ON_BLOCK_EXIT([&] {
                UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                AutoGetCollection autoColl(
                    opCtx.get(), nss, MODE_IX, AutoGetCollectionViewMode::kViewsForbidden);

                auto* const csr = CollectionShardingRuntime::get(opCtx.get(), nss);
                auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx.get(), csr);
                csr->resetShardVersionRecoverRefreshFuture(csrLock);
            });

            if (runRecover) {
                auto* const replCoord = repl::ReplicationCoordinator::get(opCtx.get());
                if (!replCoord->isReplEnabled() || replCoord->getMemberState().primary()) {
                    migrationutil::recoverMigrationCoordinations(opCtx.get(), nss);
                }
            }

            forceShardFilteringMetadataRefresh(opCtx.get(), nss, true);
        })
        .semi()
        .share();
}

}  // namespace

void onShardVersionMismatch(OperationContext* opCtx,
                            const NamespaceString& nss,
                            boost::optional<ChunkVersion> shardVersionReceived) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(ShardingState::get(opCtx)->canAcceptShardedCommands());

    if (nss.isNamespaceAlwaysUnsharded()) {
        return;
    }

    ShardingStatistics::get(opCtx).countStaleConfigErrors.addAndFetch(1);

    LOGV2_DEBUG(22061,
                2,
                "Metadata refresh requested for {namespace} at shard version "
                "{shardVersionReceived}",
                "Metadata refresh requested for collection",
                "namespace"_attr = nss,
                "shardVersionReceived"_attr = shardVersionReceived);

    while (true) {
        // If another thread is currently holding the critical section or the shard version future,
        // it will be necessary to wait on one of the following variables to finish the
        // update/recover/refresh.
        std::shared_ptr<Notification<void>> critSecSignal;
        boost::optional<SharedSemiFuture<void>> inRecoverOrRefresh;

        // Flag indicating wether the current thread has triggered a recover/refresh
        bool triggeredRecoverRefresh = false;

        {
            AutoGetCollection autoColl(
                opCtx, nss, MODE_IS, AutoGetCollectionViewMode::kViewsForbidden);

            auto* const csr = CollectionShardingRuntime::get(opCtx, nss);

            inRecoverOrRefresh = csr->getShardVersionRecoverRefreshFuture(opCtx);
            critSecSignal =
                csr->getCriticalSectionSignal(opCtx, ShardingMigrationCriticalSection::kWrite);

            if (!inRecoverOrRefresh && !critSecSignal) {
                const auto collDesc = csr->getCurrentMetadataIfKnown();

                // Check if the current shard version is fresh enough
                if (collDesc) {
                    if (shardVersionReceived) {
                        const auto currentShardVersion = collDesc->getShardVersion();
                        // Don't need to remotely reload if we're in the same epoch and the
                        // requested version is smaller than the one we know about. This means that
                        // the remote side is behind.
                        if (currentShardVersion.epoch() == shardVersionReceived->epoch() &&
                            currentShardVersion.majorVersion() >=
                                shardVersionReceived->majorVersion())
                            return;
                    }
                }

                // If the shard doesn't yet know its filtering metadata, recovery needs to be run
                const bool runRecover = collDesc ? false : true;

                // If the critical section is not busy and no recover/refresh is ongoing,
                // initialize the RecoverRefreshThread thread and associate it to the CSR.
                auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);

                inRecoverOrRefresh = csr->getShardVersionRecoverRefreshFuture(opCtx);
                critSecSignal =
                    csr->getCriticalSectionSignal(opCtx, ShardingMigrationCriticalSection::kWrite);

                if (!inRecoverOrRefresh && !critSecSignal) {
                    csr->setShardVersionRecoverRefreshFuture(
                        recoverRefreshShardVersion(opCtx->getServiceContext(), nss, runRecover),
                        csrLock);
                    inRecoverOrRefresh = csr->getShardVersionRecoverRefreshFuture(opCtx);
                    triggeredRecoverRefresh = true;
                }
            }
        }

        // Wait for an ongoing shard version's recovery/refresh/update
        if (critSecSignal) {
            critSecSignal->get(opCtx);
        } else {
            inRecoverOrRefresh->get(opCtx);
            if (triggeredRecoverRefresh) {
                return;
            }
        }
    }
}

ScopedShardVersionCriticalSection::ScopedShardVersionCriticalSection(OperationContext* opCtx,
                                                                     NamespaceString nss)
    : _opCtx(opCtx), _nss(std::move(nss)) {

    while (true) {
        // If another thread is currently holding the critical section or the shard version future,
        // it will be necessary to wait on one of the following variables to finish the
        // update/recover/refresh.
        std::shared_ptr<Notification<void>> critSecSignal;
        boost::optional<SharedSemiFuture<void>> inRecoverOrRefresh;

        {
            // This acquisition is performed with collection lock MODE_S in order to ensure that any
            // ongoing writes have completed and become visible
            AutoGetCollection autoColl(_opCtx,
                                       _nss,
                                       MODE_S,
                                       AutoGetCollectionViewMode::kViewsForbidden,
                                       _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                           Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));

            auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);

            inRecoverOrRefresh = csr->getShardVersionRecoverRefreshFuture(_opCtx);
            critSecSignal =
                csr->getCriticalSectionSignal(_opCtx, ShardingMigrationCriticalSection::kWrite);

            if (!inRecoverOrRefresh && !critSecSignal) {
                auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);
                inRecoverOrRefresh = csr->getShardVersionRecoverRefreshFuture(_opCtx);
                critSecSignal =
                    csr->getCriticalSectionSignal(_opCtx, ShardingMigrationCriticalSection::kWrite);

                if (!inRecoverOrRefresh && !critSecSignal) {
                    CollectionShardingRuntime::get(_opCtx, _nss)
                        ->enterCriticalSectionCatchUpPhase(csrLock);
                    break;
                }
            }
        }

        // Wait for an ongoing shard version's recovery/refresh/update
        if (critSecSignal) {
            critSecSignal->get(opCtx);
        } else {
            inRecoverOrRefresh->get(opCtx);
        }
    }

    // Holding the critical section ensures that a shard version recover/refresh will be perfomed by
    // just one thread at a time
    auto* const replCoord = repl::ReplicationCoordinator::get(_opCtx);
    if (!replCoord->isReplEnabled() || replCoord->getMemberState().primary()) {
        migrationutil::recoverMigrationCoordinations(_opCtx, _nss);
    }

    forceShardFilteringMetadataRefresh(_opCtx, _nss, true);
}

ScopedShardVersionCriticalSection::~ScopedShardVersionCriticalSection() {
    UninterruptibleLockGuard noInterrupt(_opCtx->lockState());
    AutoGetCollection autoColl(_opCtx, _nss, MODE_IX);
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    csr->exitCriticalSection(_opCtx);
}

void ScopedShardVersionCriticalSection::enterCommitPhase() {
    AutoGetCollection autoColl(_opCtx,
                               _nss,
                               MODE_IS,
                               AutoGetCollectionViewMode::kViewsForbidden,
                               _opCtx->getServiceContext()->getPreciseClockSource()->now() +
                                   Milliseconds(migrationLockAcquisitionMaxWaitMS.load()));
    auto* const csr = CollectionShardingRuntime::get(_opCtx, _nss);
    auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(_opCtx, csr);
    csr->enterCriticalSectionCommitPhase(csrLock);
}

Status onShardVersionMismatchNoExcept(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      boost::optional<ChunkVersion> shardVersionReceived) noexcept {
    try {
        onShardVersionMismatch(opCtx, nss, shardVersionReceived);
        return Status::OK();
    } catch (const DBException& ex) {
        LOGV2(22062,
              "Failed to refresh metadata for {namespace} due to {error}",
              "Failed to refresh metadata for collection",
              "namespace"_attr = nss,
              "error"_attr = redact(ex));
        return ex.toStatus();
    }
}

CollectionMetadata forceGetCurrentMetadata(OperationContext* opCtx, const NamespaceString& nss) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    auto* const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->canAcceptShardedCommands());

    auto routingInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, nss, true));

    if (!routingInfo.cm()) {
        return CollectionMetadata();
    }

    return CollectionMetadata(*routingInfo.cm(), shardingState->shardId());
}

ChunkVersion forceShardFilteringMetadataRefresh(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                bool forceRefreshFromThisThread) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    auto* const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->canAcceptShardedCommands());

    auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
            opCtx, nss, forceRefreshFromThisThread));
    auto cm = routingInfo.cm();

    if (!cm) {
        // No chunk manager, so unsharded. Avoid using AutoGetCollection() as it returns the
        // InvalidViewDefinition error code if an invalid view is in the 'system.views' collection.
        AutoGetDb autoDb(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        CollectionShardingRuntime::get(opCtx, nss)
            ->setFilteringMetadata(opCtx, CollectionMetadata());

        return ChunkVersion::UNSHARDED();
    }

    // Optimistic check with only IS lock in order to avoid threads piling up on the collection X
    // lock below
    {
        // Avoid using AutoGetCollection() as it returns the InvalidViewDefinition error code
        // if an invalid view is in the 'system.views' collection.
        AutoGetDb autoDb(opCtx, nss.db(), MODE_IS);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
        auto optMetadata = CollectionShardingRuntime::get(opCtx, nss)->getCurrentMetadataIfKnown();

        // We already have newer version
        if (optMetadata) {
            const auto& metadata = *optMetadata;
            if (metadata.isSharded() &&
                metadata.getCollVersion().epoch() == cm->getVersion().epoch() &&
                metadata.getCollVersion() >= cm->getVersion()) {
                LOGV2_DEBUG(
                    22063,
                    1,
                    "Skipping refresh of metadata for {namespace} {latestCollectionVersion} with "
                    "an older {refreshedCollectionVersion}",
                    "Skipping metadata refresh because collection already has at least as recent "
                    "metadata",
                    "namespace"_attr = nss,
                    "latestCollectionVersion"_attr = metadata.getCollVersion(),
                    "refreshedCollectionVersion"_attr = cm->getVersion());
                return metadata.getShardVersion();
            }
        }
    }

    // Exclusive collection lock needed since we're now changing the metadata. Avoid using
    // AutoGetCollection() as it returns the InvalidViewDefinition error code if an invalid view is
    // in the 'system.views' collection.
    AutoGetDb autoDb(opCtx, nss.db(), MODE_IX);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
    auto* const csr = CollectionShardingRuntime::get(opCtx, nss);

    {
        auto optMetadata = csr->getCurrentMetadataIfKnown();

        // We already have newer version
        if (optMetadata) {
            const auto& metadata = *optMetadata;
            if (metadata.isSharded() &&
                metadata.getCollVersion().epoch() == cm->getVersion().epoch() &&
                metadata.getCollVersion() >= cm->getVersion()) {
                LOGV2_DEBUG(
                    22064,
                    1,
                    "Skipping refresh of metadata for {namespace} {latestCollectionVersion} with "
                    "an older {refreshedCollectionVersion}",
                    "Skipping metadata refresh because collection already has at least as recent "
                    "metadata",
                    "namespace"_attr = nss,
                    "latestCollectionVersion"_attr = metadata.getCollVersion(),
                    "refreshedCollectionVersion"_attr = cm->getVersion());
                return metadata.getShardVersion();
            }
        }
    }

    CollectionMetadata metadata(*cm, shardingState->shardId());
    const auto newShardVersion = metadata.getShardVersion();

    csr->setFilteringMetadata(opCtx, std::move(metadata));
    return newShardVersion;
}

Status onDbVersionMismatchNoExcept(
    OperationContext* opCtx,
    const StringData dbName,
    const DatabaseVersion& clientDbVersion,
    const boost::optional<DatabaseVersion>& serverDbVersion) noexcept {
    try {
        onDbVersionMismatch(opCtx, dbName, clientDbVersion, serverDbVersion);
        return Status::OK();
    } catch (const DBException& ex) {
        LOGV2(22065,
              "Failed to refresh databaseVersion for database {db} {error}",
              "Failed to refresh databaseVersion",
              "db"_attr = dbName,
              "error"_attr = redact(ex));
        return ex.toStatus();
    }
}

void forceDatabaseRefresh(OperationContext* opCtx, const StringData dbName) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    auto const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->canAcceptShardedCommands());

    DatabaseVersion refreshedDbVersion;
    try {
        refreshedDbVersion =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, dbName))
                .databaseVersion();
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // db has been dropped, set the db version to boost::none
        Lock::DBLock dbLock(opCtx, dbName, MODE_X);
        auto dss = DatabaseShardingState::get(opCtx, dbName);
        auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);

        dss->setDbVersion(opCtx, boost::none, dssLock);
        return;
    }

    // First, check under a shared lock if another thread already updated the cached version.
    // This is a best-effort optimization to make as few threads as possible to convoy on the
    // exclusive lock below.
    {
        // Take the DBLock directly rather than using AutoGetDb, to prevent a recursive call
        // into checkDbVersion().
        Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
        auto dss = DatabaseShardingState::get(opCtx, dbName);
        auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);

        const auto cachedDbVersion = dss->getDbVersion(opCtx, dssLock);
        if (cachedDbVersion && cachedDbVersion->getUuid() == refreshedDbVersion.getUuid() &&
            cachedDbVersion->getLastMod() >= refreshedDbVersion.getLastMod()) {
            LOGV2_DEBUG(22066,
                        2,
                        "Skipping setting cached databaseVersion for {db} to refreshed version "
                        "{refreshedDbVersion} because current cached databaseVersion is already "
                        "{cachedDbVersion}",
                        "Skipping setting cached databaseVersion to refreshed version "
                        "because current cached databaseVersion is more recent",
                        "db"_attr = dbName,
                        "refreshedDbVersion"_attr = refreshedDbVersion.toBSON(),
                        "cachedDbVersion"_attr = cachedDbVersion->toBSON());
            return;
        }
    }

    // The cached version is older than the refreshed version; update the cached version.
    Lock::DBLock dbLock(opCtx, dbName, MODE_X);
    auto dss = DatabaseShardingState::get(opCtx, dbName);
    auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);

    dss->setDbVersion(opCtx, std::move(refreshedDbVersion), dssLock);
}

}  // namespace mongo
