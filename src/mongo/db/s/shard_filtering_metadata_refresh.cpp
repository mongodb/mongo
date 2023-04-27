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

        uassertStatusOK(OperationShardingState::waitForCriticalSectionToComplete(opCtx, *critSect));
        return true;
    }

    if (auto refreshVersionFuture = (**scopedDss)->getDbMetadataRefreshFuture()) {
        LOGV2_DEBUG(6697202,
                    2,
                    "Waiting for completion of another database metadata refresh",
                    logAttrs((**scopedDss)->getDbName()));

        scopedDss->reset();
        dbLock->reset();

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
        // TODO (SERVER-71444): Fix to be interruptible or document exception.
        // Can be uninterruptible because the work done under it can never block.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());  // NOLINT.
        auto scopedDss = DatabaseShardingState::acquireExclusive(opCtx, dbName);
        scopedDss->resetDbMetadataRefreshFuture();
    });

    // Force a refresh of the cached database metadata from the config server.
    const auto swDbMetadata =
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, dbName.db());

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

SharedSemiFuture<void> recoverRefreshDbVersion(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               const CancellationToken& cancellationToken) {
    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    return ExecutorFuture<void>(executor)
        .then([=,
               serviceCtx = opCtx->getServiceContext(),
               forwardableOpMetadata = ForwardableOperationMetadata(opCtx)] {
            ThreadClient tc("DbMetadataRefreshThread", serviceCtx);
            const auto opCtxHolder =
                CancelableOperationContext(tc->makeOperationContext(), cancellationToken, executor);
            auto opCtx = opCtxHolder.get();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // Forward `users` and `roles` attributes from the original request.
            forwardableOpMetadata.setOn(opCtx);

            LOGV2_DEBUG(6697203, 2, "Started database metadata refresh", logAttrs(dbName));

            return refreshDbMetadata(opCtx, dbName, cancellationToken);
        })
        .onCompletion([=](Status status) {
            uassert(ErrorCodes::DatabaseMetadataRefreshCanceled,
                    str::stream() << "Canceled metadata refresh for database "
                                  << dbName.toStringForErrorMsg(),
                    !cancellationToken.isCanceled());

            if (status.isOK() || status == ErrorCodes::NamespaceNotFound) {
                LOGV2(6697204, "Refreshed database metadata", logAttrs(dbName));
            } else {
                LOGV2_ERROR(6697205,
                            "Failed database metadata refresh",
                            logAttrs(dbName),
                            "error"_attr = redact(status));
            }
        })
        .semi()
        .share();
}

void onDbVersionMismatch(OperationContext* opCtx,
                         const StringData dbName,
                         boost::optional<DatabaseVersion> receivedDbVersion) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(ShardingState::get(opCtx)->canAcceptShardedCommands());

    tassert(ErrorCodes::IllegalOperation,
            "Can't check version of {} database"_format(dbName),
            dbName != DatabaseName::kAdmin.db() && dbName != DatabaseName::kConfig.db());

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
            dbLock.emplace(opCtx, DatabaseNameUtil::deserialize(boost::none, dbName), MODE_IS);

            if (receivedDbVersion) {
                auto scopedDss =
                    boost::make_optional(DatabaseShardingState::assertDbLockedAndAcquireShared(
                        opCtx, DatabaseNameUtil::deserialize(boost::none, dbName)));

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

            auto scopedDss =
                boost::make_optional(DatabaseShardingState::assertDbLockedAndAcquireExclusive(
                    opCtx, DatabaseNameUtil::deserialize(boost::none, dbName)));

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
                    recoverRefreshDbVersion(opCtx,
                                            DatabaseNameUtil::deserialize(boost::none, dbName),
                                            cancellationToken),
                    std::move(cancellationSource));
            dbMetadataRefreshFuture = (*scopedDss)->getDbMetadataRefreshFuture();
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

        uassertStatusOK(
            OperationShardingState::waitForCriticalSectionToComplete(opCtx, *critSecSignal));

        return true;
    }

    if (auto inRecoverOrRefresh = (**scopedCsr)->getPlacementVersionRecoverRefreshFuture(opCtx)) {
        scopedCsr->reset();
        collLock->reset();
        dbLock->reset();

        try {
            inRecoverOrRefresh->get(opCtx);
        } catch (const ExceptionFor<ErrorCodes::PlacementVersionRefreshCanceled>&) {
            // The ongoing refresh has finished, although it was canceled by a
            // 'clearFilteringMetadata'.
        }

        return true;
    }

    return false;
}

SharedSemiFuture<void> recoverRefreshCollectionPlacementVersion(
    ServiceContext* serviceContext,
    const NamespaceString& nss,
    bool runRecover,
    CancellationToken cancellationToken) {
    auto executor = Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor();
    return ExecutorFuture<void>(executor)
        .then([=] {
            ThreadClient tc("RecoverRefreshThread", serviceContext);

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
                UninterruptibleLockGuard noInterrupt(opCtx->lockState());  // NOLINT.
                auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
                scopedCsr->resetPlacementVersionRecoverRefreshFuture();
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
                        Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IS);
                        Lock::CollectionLock collLock(opCtx, nss, MODE_IS);

                        const auto scopedCsr =
                            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx,
                                                                                              nss);

                        if (auto msm = MigrationSourceManager::get(*scopedCsr)) {
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

            // Only if all actions taken as part of refreshing the placement version completed
            // successfully do we want to install the current metadata.
            // A view can potentially be created after spawning a thread to recover nss's shard
            // version. It is then ok to lock views in order to clear filtering metadata.
            //
            // DBLock and CollectionLock must be used in order to avoid placement version checks
            Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
            Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
            auto scopedCsr =
                CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss);

            // cancellationToken needs to be checked under the CSR lock before overwriting the
            // filtering metadata to serialize with other threads calling 'clearFilteringMetadata'.
            if (!cancellationToken.isCanceled()) {
                scopedCsr->setFilteringMetadata(opCtx, currentMetadata);
            }

            scopedCsr->resetPlacementVersionRecoverRefreshFuture();
            resetRefreshFutureOnError.dismiss();
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

}  // namespace

void onCollectionPlacementVersionMismatch(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          boost::optional<ChunkVersion> chunkVersionReceived) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(ShardingState::get(opCtx)->canAcceptShardedCommands());

    Timer t{};
    ScopeGuard finishTiming([&] {
        CurOp::get(opCtx)->debug().placementVersionRefreshMillis += Milliseconds(t.millis());
    });

    if (nss.isNamespaceAlwaysUnsharded()) {
        return;
    }

    LOGV2_DEBUG(22061,
                2,
                "Metadata refresh requested for {namespace} at chunk version "
                "{chunkVersionReceived}",
                "Metadata refresh requested for collection",
                logAttrs(nss),
                "chunkVersionReceived"_attr = chunkVersionReceived);

    while (true) {
        boost::optional<SharedSemiFuture<void>> inRecoverOrRefresh;

        {
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
                    recoverRefreshCollectionPlacementVersion(
                        opCtx->getServiceContext(), nss, runRecover, std::move(cancellationToken)),
                    std::move(cancellationSource));
            inRecoverOrRefresh = (*scopedCsr)->getPlacementVersionRecoverRefreshFuture(opCtx);
        }

        try {
            inRecoverOrRefresh->get(opCtx);
        } catch (const ExceptionFor<ErrorCodes::PlacementVersionRefreshCanceled>&) {
            // The refresh was canceled by a 'clearFilteringMetadata'. Retry the refresh.
            continue;
        }

        break;
    }
}

Status onCollectionPlacementVersionMismatchNoExcept(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<ChunkVersion> chunkVersionReceived) noexcept {
    try {
        onCollectionPlacementVersionMismatch(opCtx, nss, chunkVersionReceived);
        return Status::OK();
    } catch (const DBException& ex) {
        LOGV2(22062,
              "Failed to refresh metadata for {namespace} due to {error}",
              "Failed to refresh metadata for collection",
              logAttrs(nss),
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
        const auto [cm, _] = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithPlacementRefresh(opCtx,
                                                                                           nss));

        if (!cm.isSharded()) {
            return CollectionMetadata();
        }

        return CollectionMetadata(cm, shardingState->shardId());
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        LOGV2(505070,
              "Namespace {namespace} not found, collection may have been dropped",
              "Namespace not found, collection may have been dropped",
              logAttrs(nss),
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

    const auto [cm, _] = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithPlacementRefresh(opCtx, nss));

    if (!cm.isSharded()) {
        // DBLock and CollectionLock are used here to avoid throwing further recursive stale
        // config errors, as well as a possible InvalidViewDefinition error if an invalid view
        // is in the 'system.views' collection.
        Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss);
        scopedCsr->setFilteringMetadata(opCtx, CollectionMetadata());

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
        const auto scopedCsr =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);
        if (auto optMetadata = scopedCsr->getCurrentMetadataIfKnown()) {
            const auto& metadata = *optMetadata;
            if (metadata.isSharded() &&
                (cm.getVersion().isOlderOrEqualThan(metadata.getCollPlacementVersion()))) {
                LOGV2_DEBUG(22063,
                            1,
                            "Skipping metadata refresh because collection already is up-to-date",
                            logAttrs(nss),
                            "latestCollectionPlacementVersion"_attr =
                                metadata.getCollPlacementVersion(),
                            "refreshedCollectionPlacementVersion"_attr = cm.getVersion());
                return metadata.getShardPlacementVersion();
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
    auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss);
    if (auto optMetadata = scopedCsr->getCurrentMetadataIfKnown()) {
        const auto& metadata = *optMetadata;
        if (metadata.isSharded() &&
            (cm.getVersion().isOlderOrEqualThan(metadata.getCollPlacementVersion()))) {
            LOGV2_DEBUG(22064,
                        1,
                        "Skipping metadata refresh because collection already is up-to-date",
                        logAttrs(nss),
                        "latestCollectionPlacementVersion"_attr =
                            metadata.getCollPlacementVersion(),
                        "refreshedCollectionPlacementVersion"_attr = cm.getVersion());
            return metadata.getShardPlacementVersion();
        }
    }

    CollectionMetadata metadata(cm, shardingState->shardId());
    auto newPlacementVersion = metadata.getShardPlacementVersion();

    scopedCsr->setFilteringMetadata(opCtx, std::move(metadata));
    return newPlacementVersion;
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
