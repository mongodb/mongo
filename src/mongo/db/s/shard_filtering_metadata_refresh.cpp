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
#include <string>
#include <tuple>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

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
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

    ScopeGuard resetRefreshFutureOnError([&] {
        // TODO (SERVER-71444): Fix to be interruptible or document exception.
        // Can be uninterruptible because the work done under it can never block.
        UninterruptibleLockGuard noInterrupt(shard_role_details::getLocker(opCtx));  // NOLINT.
        auto scopedDss = DatabaseShardingState::acquireExclusive(opCtx, dbName);
        scopedDss->resetDbMetadataRefreshFuture();
    });

    // Force a refresh of the cached database metadata from the config server.
    const auto swDbMetadata =
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, dbName);

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
            ThreadClient tc("DbMetadataRefreshThread",
                            serviceCtx->getService(ClusterRole::ShardServer));
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

void onDbVersionMismatch(OperationContext* opCtx,
                         const DatabaseName& dbName,
                         boost::optional<DatabaseVersion> receivedDbVersion) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

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
                    recoverRefreshDbVersion(opCtx, dbName, cancellationToken),
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
                UninterruptibleLockGuard noInterrupt(  // NOLINT.
                    shard_role_details::getLocker(opCtx));
                auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
                scopedCsr->resetPlacementVersionRecoverRefreshFuture();
            });

            if (runRecover) {
                auto* const replCoord = repl::ReplicationCoordinator::get(opCtx);
                if (!replCoord->getSettings().isReplSet() ||
                    replCoord->getMemberState().primary()) {
                    migrationutil::recoverMigrationCoordinations(opCtx, nss, cancellationToken);
                }
            }

            auto currentMetadata = forceGetCurrentMetadata(opCtx, nss);

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

}  // namespace

void onCollectionPlacementVersionMismatch(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          boost::optional<ChunkVersion> chunkVersionReceived) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());
    ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

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
            ScopedAdmissionPriorityForLock skipAdmissionControl(
                shard_role_details::getLocker(opCtx), AdmissionContext::Priority::kImmediate);

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
              "Failed to refresh metadata for collection",
              logAttrs(nss),
              "error"_attr = redact(ex));
        return ex.toStatus();
    }
}

CollectionMetadata forceGetCurrentMetadata(OperationContext* opCtx, const NamespaceString& nss) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    auto* const shardingState = ShardingState::get(opCtx);
    shardingState->assertCanAcceptShardedCommands();

    try {
        const auto [cm, _] = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithPlacementRefresh(opCtx,
                                                                                           nss));

        if (!cm.hasRoutingTable()) {
            return CollectionMetadata();
        }

        return CollectionMetadata(cm, shardingState->shardId());
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>& ex) {
        LOGV2(505070,
              "Namespace not found, collection may have been dropped",
              logAttrs(nss),
              "error"_attr = redact(ex));
        return CollectionMetadata();
    }
}

ChunkVersion forceShardFilteringMetadataRefresh(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    if (MONGO_unlikely(skipShardFilteringMetadataRefresh.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "skipShardFilteringMetadataRefresh failpoint");
    }

    auto* const shardingState = ShardingState::get(opCtx);
    shardingState->assertCanAcceptShardedCommands();

    const auto [cm, _] = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithPlacementRefresh(opCtx, nss));

    if (!cm.hasRoutingTable()) {
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
            if (metadata.hasRoutingTable() &&
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
        if (metadata.hasRoutingTable() &&
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
                                   const DatabaseName& dbName,
                                   boost::optional<DatabaseVersion> clientDbVersion) noexcept {
    try {
        onDbVersionMismatch(opCtx, dbName, clientDbVersion);
        return Status::OK();
    } catch (const DBException& ex) {
        LOGV2(22065,
              "Failed to refresh databaseVersion",
              "db"_attr = dbName,
              "error"_attr = redact(ex));
        return ex.toStatus();
    }
}

}  // namespace mongo
