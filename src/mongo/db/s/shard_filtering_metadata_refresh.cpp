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
#include "mongo/db/s/forwardable_operation_metadata.h"
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

/**
 * If the critical section associate with the database is entered by another thread (e.g., a move
 * primary or a drop database is in progress), it releases the acquired locks and waits for the
 * latter to exit. In this case the function returns true, otherwise false.
 */
bool checkAndWaitIfCriticalSectionIsEntered(
    OperationContext* opCtx,
    DatabaseShardingState* dss,
    boost::optional<Lock::DBLock>& dbLock,
    boost::optional<DatabaseShardingState::DSSLock>& dssLock) {
    invariant(dbLock);
    invariant(dssLock);

    if (auto critSect =
            dss->getCriticalSectionSignal(ShardingMigrationCriticalSection::kRead, *dssLock)) {
        LOGV2_DEBUG(6697201,
                    2,
                    "Waiting for exit from the critical section",
                    "db"_attr = dss->getDbName(),
                    "reason"_attr = dss->getCriticalSectionReason(*dssLock));

        dbLock = boost::none;
        dssLock = boost::none;

        uassertStatusOK(OperationShardingState::waitForCriticalSectionToComplete(opCtx, *critSect));

        return true;
    }

    return false;
}

/**
 * If another thread is refreshing the database metadata, it releases the acquired locks and waits
 * for that refresh to complete. In this case the function returns true, otherwise false.
 */
bool checkAndWaitIfAnotherRefreshIsRunning(
    OperationContext* opCtx,
    DatabaseShardingState* dss,
    boost::optional<Lock::DBLock>& dbLock,
    boost::optional<DatabaseShardingState::DSSLock>& dssLock) {
    invariant(dbLock);
    invariant(dssLock);

    if (auto refreshVersionFuture = dss->getDbMetadataRefreshFuture(*dssLock)) {
        LOGV2_DEBUG(6697202,
                    2,
                    "Waiting for completion of another database metadata refresh",
                    "db"_attr = dss->getDbName());

        dbLock = boost::none;
        dssLock = boost::none;

        try {
            refreshVersionFuture->get(opCtx);
        } catch (const ExceptionFor<ErrorCodes::DatabaseMetadataRefreshCanceled>&) {
            // The refresh was canceled by another thread that entered the critical section.
        }

        return true;
    }

    return false;
}

/**
 * Unconditionally refreshes the database metadata from the config server.
 *
 * NOTE: Does network I/O and acquires the database lock in X mode.
 */
Status refreshDbMetadata(OperationContext* opCtx,
                         const DatabaseName& dbName,
                         CancellationToken cancellationToken) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(ShardingState::get(opCtx)->canAcceptShardedCommands());

    ScopeGuard resetRefreshFutureOnError([&] {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());

        Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
        auto* dss = DatabaseShardingState::get(opCtx, dbName.db());
        const auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);

        dss->resetDbMetadataRefreshFuture(dssLock);
    });

    // Force a refresh of the cached database metadata from the config server.
    const auto swDbMetadata =
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, dbName.db());

    Lock::DBLock dbLock(opCtx, dbName, MODE_X);
    auto* dss = DatabaseShardingState::get(opCtx, dbName.db());
    const auto dssLock = DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss);

    if (!cancellationToken.isCanceled()) {
        if (swDbMetadata.isOK()) {
            // Set the refreshed database metadata in the local catalog.
            DatabaseHolder::get(opCtx)->openDb(opCtx, dbName);
            DatabaseHolder::get(opCtx)->setDbInfo(opCtx, dbName, *swDbMetadata.getValue());
        } else if (swDbMetadata == ErrorCodes::NamespaceNotFound) {
            // The database has been dropped, so clear its metadata in the local catalog.
            DatabaseHolder::get(opCtx)->clearDbInfo(opCtx, dbName);
        }
    }

    // Reset the future reference to allow any other thread to refresh the database metadata.
    dss->resetDbMetadataRefreshFuture(dssLock);
    resetRefreshFutureOnError.dismiss();

    return swDbMetadata.getStatus();
}

SharedSemiFuture<void> asyncDbMetadataRefresh(OperationContext* opCtx,
                                              const DatabaseName& dbName,
                                              const CancellationToken& cancellationToken) {
    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    return ExecutorFuture<void>(executor)
        .then([=,
               serviceCtx = opCtx->getServiceContext(),
               forwardableOpMetadata = ForwardableOperationMetadata(opCtx)] {
            ThreadClient tc("DbMetadataRefreshThread", serviceCtx);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }

            const auto opCtxHolder =
                CancelableOperationContext(tc->makeOperationContext(), cancellationToken, executor);
            auto opCtx = opCtxHolder.get();

            // Forward `users` and `roles` attributes from the original request.
            forwardableOpMetadata.setOn(opCtx);

            LOGV2_DEBUG(6697203, 2, "Started database metadata refresh", "db"_attr = dbName);

            return refreshDbMetadata(opCtx, dbName, cancellationToken);
        })
        .onCompletion([=](Status status) {
            uassert(ErrorCodes::DatabaseMetadataRefreshCanceled,
                    str::stream() << "Canceled metadata refresh for database " << dbName,
                    !cancellationToken.isCanceled());

            if (status.isOK()) {
                LOGV2(6697204, "Refreshed database metadata", "db"_attr = dbName);
            } else {
                LOGV2_ERROR(6697205,
                            "Failed database metadata refresh",
                            "db"_attr = dbName,
                            "error"_attr = redact(status));
            }
        })
        .semi()
        .share();
}

void onDbVersionMismatch(OperationContext* opCtx,
                         const StringData dbName,
                         boost::optional<DatabaseVersion> receivedVersion) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(ShardingState::get(opCtx)->canAcceptShardedCommands());

    tassert(ErrorCodes::IllegalOperation,
            "Can't check version of {} database"_format(dbName),
            dbName != NamespaceString::kAdminDb && dbName != NamespaceString::kConfigDb);

    Timer t{};
    ScopeGuard finishTiming([&] {
        CurOp::get(opCtx)->debug().databaseVersionRefreshMillis += Milliseconds(t.millis());
    });

    LOGV2_DEBUG(6697200,
                2,
                "Handle database version mismatch",
                "db"_attr = dbName,
                "receivedVersion"_attr = receivedVersion);

    while (true) {
        boost::optional<SharedSemiFuture<void>> dbMetadataRefreshFuture;

        // Set the database metadata refresh future after waiting for another thread to exit from
        // the critical section or to complete an ongoing refresh.
        {
            auto dbLock = boost::make_optional(Lock::DBLock(opCtx, dbName, MODE_IS));
            auto* dss = DatabaseShardingState::get(opCtx, dbName);

            if (receivedVersion) {
                auto dssLock =
                    boost::make_optional(DatabaseShardingState::DSSLock::lockShared(opCtx, dss));

                if (checkAndWaitIfCriticalSectionIsEntered(opCtx, dss, dbLock, dssLock) ||
                    checkAndWaitIfAnotherRefreshIsRunning(opCtx, dss, dbLock, dssLock)) {
                    // Waited for another thread to exit from the critical section or to complete an
                    // ongoing refresh, so reacquire the locks.
                    continue;
                }

                // From now until the end of this block [1] no thread is in the critical section or
                // can enter it (would require to X-lock the database) and [2] no metadata refresh
                // is in progress or can start (would require to exclusive lock the DSS).
                // Therefore, the database version can be accessed safely.

                const auto wantedVersion = DatabaseHolder::get(opCtx)->getDbVersion(opCtx, dbName);
                if (receivedVersion <= wantedVersion) {
                    // No need to refresh the database metadata as the wanted version is newer
                    // than the one received.
                    return;
                }
            }

            if (MONGO_unlikely(skipDatabaseVersionMetadataRefresh.shouldFail())) {
                return;
            }

            auto dssLock =
                boost::make_optional(DatabaseShardingState::DSSLock::lockExclusive(opCtx, dss));

            if (checkAndWaitIfCriticalSectionIsEntered(opCtx, dss, dbLock, dssLock) ||
                checkAndWaitIfAnotherRefreshIsRunning(opCtx, dss, dbLock, dssLock)) {
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
            dss->setDbMetadataRefreshFuture(
                asyncDbMetadataRefresh(opCtx, dbName, cancellationToken),
                std::move(cancellationSource),
                *dssLock);
            dbMetadataRefreshFuture = dss->getDbMetadataRefreshFuture(*dssLock);
        }

        // No other metadata refresh for this database can run in parallel. If another thread enters
        // the critical section, the ongoing refresh would be interrupted and subsequently
        // re-queued.

        try {
            dbMetadataRefreshFuture->get(opCtx);
        } catch (const ExceptionFor<ErrorCodes::DatabaseMetadataRefreshCanceled>&) {
            // The refresh was canceled by another thread that entered the critical section.
            continue;
        }

        break;
    }
}

/**
 * Blocking method, which will wait for any concurrent operations that could change the shard
 * version to complete (namely critical section and concurrent onShardVersionMismatch invocations).
 *
 * Returns 'true' if there were concurrent operations that had to be joined (in which case all locks
 * will be dropped). If there were none, returns false and the locks continue to be held.
 */
bool joinShardVersionOperation(OperationContext* opCtx,
                               CollectionShardingRuntime* csr,
                               boost::optional<Lock::DBLock>* dbLock,
                               boost::optional<Lock::CollectionLock>* collLock,
                               boost::optional<CollectionShardingRuntime::CSRLock>* csrLock) {
    invariant(dbLock->has_value());
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
                            boost::optional<ShardVersion> shardVersionReceived) {
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

            if (auto metadata = csr->getCurrentMetadataIfKnown()) {
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

            if (joinShardVersionOperation(opCtx, csr, &dbLock, &collLock, &csrLock)) {
                continue;
            }

            // If we reached here, there were no ongoing critical sections or recoverRefresh running
            // and we are holding the exclusive CSR lock.

            // If the shard doesn't yet know its filtering metadata, recovery needs to be run
            const bool runRecover = csr->getCurrentMetadataIfKnown() ? false : true;
            CancellationSource cancellationSource;
            CancellationToken cancellationToken = cancellationSource.token();
            csr->setShardVersionRecoverRefreshFuture(
                recoverRefreshShardVersion(
                    opCtx->getServiceContext(), nss, runRecover, std::move(cancellationToken)),
                std::move(cancellationSource),
                *csrLock);
            inRecoverOrRefresh = csr->getShardVersionRecoverRefreshFuture(opCtx);
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
                                      boost::optional<ShardVersion> shardVersionReceived) noexcept {
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

}  // namespace mongo
