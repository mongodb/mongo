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

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(skipDatabaseVersionMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(skipShardFilteringMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(hangInRecoverRefreshThread);

void onDbVersionMismatch(OperationContext* opCtx,
                         const StringData dbName,
                         boost::optional<DatabaseVersion> clientDbVersion) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(ShardingState::get(opCtx)->canAcceptShardedCommands());

    Timer t{};
    ScopeGuard finishTiming([&] {
        CurOp::get(opCtx)->debug().databaseVersionRefreshMillis += Milliseconds(t.millis());
    });

    {
        // Take the DBLock directly rather than using AutoGetDb, to prevent a recursive call into
        // checkDbVersion().
        //
        // TODO: It is not safe here to read the DB version without checking for critical section
        //
        if (clientDbVersion) {
            // TODO SERVER-67440 Use dbName directly
            Lock::DBLock dbLock(opCtx, DatabaseName(boost::none, dbName), MODE_IS);
            const auto serverDbVersion = DatabaseHolder::get(opCtx)->getDbVersion(opCtx, dbName);
            if (clientDbVersion <= serverDbVersion) {
                // The client was stale
                return;
            }
        }
    }

    if (MONGO_unlikely(skipDatabaseVersionMetadataRefresh.shouldFail())) {
        return;
    }

    forceDatabaseRefresh(opCtx, dbName);
}

// Return true if joins a shard version update/recover/refresh (in that case, all locks are dropped)
bool joinShardVersionOperation(OperationContext* opCtx,
                               CollectionShardingRuntime* csr,
                               boost::optional<Lock::DBLock>* dbLock,
                               boost::optional<Lock::CollectionLock>* collLock,
                               boost::optional<CollectionShardingRuntime::CSRLock>* csrLock) {
    invariant(collLock->has_value());
    invariant(csrLock->has_value());

    // If another thread is currently holding the critical section or the shard version future, it
    // will be necessary to wait on one of the two variables to finish the update/recover/refresh.
    auto inRecoverOrRefresh = csr->getShardVersionRecoverRefreshFuture(opCtx);
    auto critSecSignal =
        csr->getCriticalSectionSignal(opCtx, ShardingMigrationCriticalSection::kWrite);

    if (inRecoverOrRefresh || critSecSignal) {
        // Drop the locks and wait for an ongoing shard version's recovery/refresh/update
        csrLock->reset();
        collLock->reset();
        dbLock->reset();

        if (critSecSignal) {
            uassertStatusOK(
                OperationShardingState::waitForCriticalSectionToComplete(opCtx, *critSecSignal));
        } else {
            try {
                inRecoverOrRefresh->get(opCtx);
            } catch (const ExceptionFor<ErrorCodes::ShardVersionRefreshCanceled>&) {
                // The ongoing refresh has finished, although it was canceled by a
                // 'clearFilteringMetadata'.
            }
        }

        return true;
    }

    return false;
}

SharedSemiFuture<void> recoverRefreshShardVersion(ServiceContext* serviceContext,
                                                  const NamespaceString nss,
                                                  bool runRecover,
                                                  CancellationToken cancellationToken) {
    auto executor = Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor();
    return ExecutorFuture<void>(executor)
        .then([=] {
            ThreadClient tc("RecoverRefreshThread", serviceContext);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }

            if (MONGO_unlikely(hangInRecoverRefreshThread.shouldFail())) {
                hangInRecoverRefreshThread.pauseWhileSet();
            }

            const auto opCtxHolder =
                CancelableOperationContext(tc->makeOperationContext(), cancellationToken, executor);
            auto const opCtx = opCtxHolder.get();

            boost::optional<CollectionMetadata> currentMetadataToInstall;

            ON_BLOCK_EXIT([&] {
                UninterruptibleLockGuard noInterrupt(opCtx->lockState());
                // A view can potentially be created after spawning a thread to recover nss's shard
                // version. It is then ok to lock views in order to clear filtering metadata.
                //
                // DBLock and CollectionLock must be used in order to avoid shard version checks
                Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
                Lock::CollectionLock collLock(opCtx, nss, MODE_IX);

                auto* const csr = CollectionShardingRuntime::get(opCtx, nss);

                auto csrLock = CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr);
                // cancellationToken needs to be checked under the CSR lock before overwriting the
                // filtering metadata to serialize with other threads calling
                // 'clearFilteringMetadata'
                if (currentMetadataToInstall && !cancellationToken.isCanceled()) {
                    csr->setFilteringMetadata_withLock(opCtx, *currentMetadataToInstall, csrLock);
                }

                csr->resetShardVersionRecoverRefreshFuture(csrLock);
            });

            if (runRecover) {
                auto* const replCoord = repl::ReplicationCoordinator::get(opCtx);
                if (!replCoord->isReplEnabled() || replCoord->getMemberState().primary()) {
                    migrationutil::recoverMigrationCoordinations(opCtx, nss, cancellationToken);
                }
            }

            auto currentMetadata = forceGetCurrentMetadata(opCtx, nss);

            if (currentMetadata.isSharded()) {
                // If migrations are disallowed for the namespace, join any migrations which may be
                // executing currently
                if (!currentMetadata.allowMigrations()) {
                    boost::optional<SharedSemiFuture<void>> waitForMigrationAbort;
                    {
                        // DBLock and CollectionLock must be used in order to avoid shard version
                        // checks
                        Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
                        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);

                        auto const& csr = CollectionShardingRuntime::get(opCtx, nss);
                        auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);
                        if (auto msm = MigrationSourceManager::get(csr, csrLock)) {
                            waitForMigrationAbort.emplace(msm->abort());
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

            // Only if all actions taken as part of refreshing the shard version completed
            // successfully do we want to install the current metadata.
            currentMetadataToInstall = std::move(currentMetadata);
        })
        .onCompletion([=](Status status) {
            // Check the cancellation token here to ensure we throw in all cancelation events,
            // including those where the cancelation was noticed on the ON_BLOCK_EXIT above (where
            // we cannot throw).
            if (cancellationToken.isCanceled() &&
                (status.isOK() || status == ErrorCodes::Interrupted)) {
                uasserted(ErrorCodes::ShardVersionRefreshCanceled,
                          "Shard version refresh canceled by a 'clearFilteringMetadata'");
            }
            return status;
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

    Timer t{};
    ScopeGuard finishTiming(
        [&] { CurOp::get(opCtx)->debug().shardVersionRefreshMillis += Milliseconds(t.millis()); });

    if (nss.isNamespaceAlwaysUnsharded()) {
        return;
    }

    LOGV2_DEBUG(22061,
                2,
                "Metadata refresh requested for {namespace} at shard version "
                "{shardVersionReceived}",
                "Metadata refresh requested for collection",
                "namespace"_attr = nss,
                "shardVersionReceived"_attr = shardVersionReceived);

    while (true) {
        boost::optional<SharedSemiFuture<void>> inRecoverOrRefresh;
        {
            boost::optional<Lock::DBLock> dbLock;
            boost::optional<Lock::CollectionLock> collLock;
            dbLock.emplace(opCtx, nss.dbName(), MODE_IS);
            collLock.emplace(opCtx, nss, MODE_IS);

            auto* const csr = CollectionShardingRuntime::get(opCtx, nss);
            boost::optional<CollectionShardingRuntime::CSRLock> csrLock =
                CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);

            if (joinShardVersionOperation(opCtx, csr, &dbLock, &collLock, &csrLock)) {
                continue;
            }

            auto metadata = csr->getCurrentMetadataIfKnown();
            if (metadata) {
                // Check if the current shard version is fresh enough
                if (shardVersionReceived) {
                    const auto currentShardVersion = metadata->getShardVersion();
                    // Don't need to remotely reload if the requested version is smaller than the
                    // known one. This means that the remote side is behind.
                    if (shardVersionReceived->isOlderOrEqualThan(currentShardVersion)) {
                        return;
                    }
                }
            }

            csrLock.reset();
            csrLock.emplace(CollectionShardingRuntime::CSRLock::lockExclusive(opCtx, csr));

            // If there is no ongoing shard version operation, initialize the RecoverRefreshThread
            // thread and associate it to the CSR.
            if (!joinShardVersionOperation(opCtx, csr, &dbLock, &collLock, &csrLock)) {
                // If the shard doesn't yet know its filtering metadata, recovery needs to be run
                const bool runRecover = metadata ? false : true;
                CancellationSource cancellationSource;
                CancellationToken cancellationToken = cancellationSource.token();
                csr->setShardVersionRecoverRefreshFuture(
                    recoverRefreshShardVersion(
                        opCtx->getServiceContext(), nss, runRecover, std::move(cancellationToken)),
                    std::move(cancellationSource),
                    *csrLock);
                inRecoverOrRefresh = csr->getShardVersionRecoverRefreshFuture(opCtx);
            } else {
                continue;
            }
        }

        try {
            inRecoverOrRefresh->get(opCtx);
        } catch (const ExceptionFor<ErrorCodes::ShardVersionRefreshCanceled>&) {
            // The refresh was canceled by a 'clearFilteringMetadata'. Retry the refresh.
            continue;
        }

        break;
    }
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

    try {
        const auto cm = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, nss));

        if (!cm.isSharded()) {
            return CollectionMetadata();
        }

        return CollectionMetadata(cm, shardingState->shardId());
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        LOGV2(505070,
              "Namespace {namespace} not found, collection may have been dropped",
              "Namespace not found, collection may have been dropped",
              "namespace"_attr = nss,
              "error"_attr = redact(ex));
        return CollectionMetadata();
    }
}

ChunkVersion forceShardFilteringMetadataRefresh(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    auto* const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->canAcceptShardedCommands());

    const auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, nss));

    if (!cm.isSharded()) {
        // DBLock and CollectionLock are used here to avoid throwing further recursive stale
        // config errors, as well as a possible InvalidViewDefinition error if an invalid view
        // is in the 'system.views' collection.
        Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        CollectionShardingRuntime::get(opCtx, nss)
            ->setFilteringMetadata(opCtx, CollectionMetadata());

        return ChunkVersion::UNSHARDED();
    }

    // Optimistic check with only IS lock in order to avoid threads piling up on the collection
    // X lock below
    {
        // DBLock and CollectionLock are used here to avoid throwing further recursive stale
        // config errors, as well as a possible InvalidViewDefinition error if an invalid view
        // is in the 'system.views' collection.
        Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IS);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
        auto optMetadata = CollectionShardingRuntime::get(opCtx, nss)->getCurrentMetadataIfKnown();

        // We already have newer version
        if (optMetadata) {
            const auto& metadata = *optMetadata;
            if (metadata.isSharded() &&
                (cm.getVersion().isOlderOrEqualThan(metadata.getCollVersion()))) {
                LOGV2_DEBUG(
                    22063,
                    1,
                    "Skipping refresh of metadata for {namespace} {latestCollectionVersion} with "
                    "an older {refreshedCollectionVersion}",
                    "Skipping metadata refresh because collection already has at least as recent "
                    "metadata",
                    "namespace"_attr = nss,
                    "latestCollectionVersion"_attr = metadata.getCollVersion(),
                    "refreshedCollectionVersion"_attr = cm.getVersion());
                return metadata.getShardVersion();
            }
        }
    }

    // Exclusive collection lock needed since we're now changing the metadata.
    //
    // DBLock and CollectionLock are used here to avoid throwing further recursive stale config
    // errors, as well as a possible InvalidViewDefinition error if an invalid view is in the
    // 'system.views' collection.
    Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
    auto* const csr = CollectionShardingRuntime::get(opCtx, nss);

    {
        auto optMetadata = csr->getCurrentMetadataIfKnown();

        // We already have newer version
        if (optMetadata) {
            const auto& metadata = *optMetadata;
            if (metadata.isSharded() &&
                (cm.getVersion().isOlderOrEqualThan(metadata.getCollVersion()))) {
                LOGV2_DEBUG(
                    22064,
                    1,
                    "Skipping refresh of metadata for {namespace} {latestCollectionVersion} with "
                    "an older {refreshedCollectionVersion}",
                    "Skipping metadata refresh because collection already has at least as recent "
                    "metadata",
                    "namespace"_attr = nss,
                    "latestCollectionVersion"_attr = metadata.getCollVersion(),
                    "refreshedCollectionVersion"_attr = cm.getVersion());
                return metadata.getShardVersion();
            }
        }
    }

    CollectionMetadata metadata(cm, shardingState->shardId());
    const auto newShardVersion = metadata.getShardVersion();

    csr->setFilteringMetadata(opCtx, std::move(metadata));
    return newShardVersion;
}

Status onDbVersionMismatchNoExcept(OperationContext* opCtx,
                                   const StringData dbName,
                                   boost::optional<DatabaseVersion> clientDbVersion) noexcept {
    try {
        onDbVersionMismatch(opCtx, dbName, clientDbVersion);
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

    const auto swRefreshedDbInfo =
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, dbName);

    if (swRefreshedDbInfo == ErrorCodes::NamespaceNotFound) {
        // db has been dropped, set the db version to boost::none
        // TODO SERVER-67440 Use dbName directly
        Lock::DBLock dbLock(opCtx, DatabaseName(boost::none, dbName), MODE_X);
        DatabaseHolder::get(opCtx)->clearDbInfo(opCtx, dbName);
        return;
    }

    const auto refreshedDbInfo = uassertStatusOK(std::move(swRefreshedDbInfo));
    const auto& refreshedDBVersion = refreshedDbInfo->getVersion();

    // First, check under a shared lock if another thread already updated the cached version.
    // This is a best-effort optimization to make as few threads as possible to convoy on the
    // exclusive lock below.
    {
        // Take the DBLock directly rather than using AutoGetDb, to prevent a recursive call
        // into checkDbVersion().
        // TODO SERVER-67440 Use dbName directly
        Lock::DBLock dbLock(opCtx, DatabaseName(boost::none, dbName), MODE_IS);
        const auto cachedDbVersion = DatabaseHolder::get(opCtx)->getDbVersion(opCtx, dbName);
        if (cachedDbVersion && *cachedDbVersion >= refreshedDBVersion) {
            LOGV2_DEBUG(5369130,
                        2,
                        "Skipping updating cached database info from refreshed version "
                        "because the one currently cached is more recent",
                        "db"_attr = dbName,
                        "refreshedDbVersion"_attr = refreshedDBVersion,
                        "cachedDbVersion"_attr = *cachedDbVersion);
            return;
        }
    }

    // The cached version is older than the refreshed version; update the cached version.
    // TODO SERVER-67440 Use dbName directly
    Lock::DBLock dbLock(opCtx, DatabaseName(boost::none, dbName), MODE_X);
    DatabaseHolder::get(opCtx)->openDb(opCtx, dbName);
    DatabaseHolder::get(opCtx)->setDbInfo(opCtx, dbName, *refreshedDbInfo);
}

}  // namespace mongo
