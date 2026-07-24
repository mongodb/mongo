// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_metadata_recoverer.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata_synchronizer.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_api_d_params_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/sharding_environment/stale_config_retry_attempt.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#include <memory>
#include <string_view>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

// Shared with non-authoritative refresh path (defined in shard_filtering_metadata_refresh.cpp).
extern FailPoint hangInRecoverRefreshThread;

MONGO_FAIL_POINT_DEFINE(forceWaitForVersionOnly);
MONGO_FAIL_POINT_DEFINE(forceNoopWriteToAdvanceConfigTimeToFail);

namespace shard_catalog_recoverer {
namespace {

/**
 * If the CSR needs database primary classification, drops the mutex it holds and waits for the DSR
 * critical section.
 *
 * Returns true when that wait ran, so the caller can kRetry. Otherwise re-acquires the CSR and
 * returns false so the caller can join via joinPlacementVersionOperations. No-ops when
 * needsDbPrimaryClassification is clear.
 */
template <typename ScopedShardingRuntime>
bool waitDbPrimaryCriticalSectionIfNeeded(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          boost::optional<ScopedShardingRuntime>& scopedCsr) {
    invariant(scopedCsr.has_value());
    if (!(*scopedCsr)->needsDbPrimaryClassification()) {
        return false;
    }

    // Must drop the scoped CSR before waiting on the DSR so we never block while holding both.
    scopedCsr.reset();

    {
        auto scopedDsr =
            boost::make_optional(DatabaseShardingRuntime::acquireShared(opCtx, nss.dbName()));
        if (refresh_util::waitForCriticalSectionIfNeeded(opCtx, scopedDsr)) {
            return true;
        }
    }

    // DSR write critical section is not held. Re-take the CSR so the caller can join next.
    scopedCsr.emplace(CollectionShardingRuntime::acquireExclusive(opCtx, nss));
    return false;
}

void maybeThrowStaleConfigOnNoopWriteFailure(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const ChunkVersion& receivedShardVersion,
                                             const repl::OpTime& timeToWaitFor,
                                             const Status& noopStatus) {
    const auto& routerStaleConfigRetryAttempt = staleConfigRetryAttempt(opCtx);
    if (!enableIncomparableShardVersionRouterBounce.load() || noopStatus.isOK() ||
        !routerStaleConfigRetryAttempt.has_value() || *routerStaleConfigRetryAttempt != 0) {
        return;
    }

    LOGV2_DEBUG(12503500,
                2,
                "Authoritative metadata recovery: noop write to advance configTime failed on the "
                "first router attempt; surfacing StaleConfig so the router refreshes and retries",
                logAttrs(nss),
                "shardVersionReceived"_attr = receivedShardVersion,
                "configTime"_attr = timeToWaitFor,
                "noopWriteError"_attr = noopStatus);

    uasserted(StaleConfigInfo(nss,
                              ShardVersionFactory::make(receivedShardVersion),
                              boost::none /* wantedVersion */,
                              ShardingState::get(opCtx)->shardId()),
              str::stream() << "Could not advance configTime to resolve an incomparable shard "
                               "version for "
                            << nss.toStringForErrorMsg()
                            << "; the noop write failed: " << noopStatus.toString());
}

void assertNoCollectionCriticalSection(const CollectionShardingRuntime& csr) {
    uassert(ErrorCodes::PlacementVersionRefreshCanceled,
            "Authoritative collection metadata recovery aborted: collection critical section "
            "is active",
            !csr.getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
}

void assertNoDatabaseCriticalSection(const DatabaseShardingRuntime& dsr) {
    uassert(ErrorCodes::PlacementVersionRefreshCanceled,
            "Authoritative collection metadata recovery aborted: database critical section "
            "is active",
            !dsr.getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite));
}

struct RecoveryOpCtx {
    ThreadClient tc;
    CancelableOperationContext holder;

    RecoveryOpCtx(ServiceContext* svc,
                  std::string_view name,
                  const CancellationToken& token,
                  ExecutorPtr executor)
        : tc(name, svc->getService()),
          holder(tc->makeOperationContext(), token, std::move(executor)) {}

    OperationContext* opCtx() {
        return holder.get();
    }
};

void cleanupMetadataSynchronizer(ServiceContext* serviceContext, const NamespaceString& nss) {
    auto executor = Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor();
    RecoveryOpCtx recoveryOpCtx(serviceContext,
                                "AuthoritativeDiskRecoveryCleanup",
                                CancellationToken::uncancelable(),
                                executor);
    auto scopedCsr = CollectionShardingRuntime::acquireExclusive(recoveryOpCtx.opCtx(), nss);
    scopedCsr->setMetadataSynchronizer(nullptr);
}

/**
 * CSR exclusive held by caller. Asserts no collection critical section; if metadata is already
 * known, clears needsDbPrimaryClassification and returns nullptr. Otherwise creates the
 * synchronizer, publishes it on the CSR, and returns it (caller must drop the CSR lock before
 * start()).
 */
std::shared_ptr<CollectionMetadataSynchronizer> createMetadataSynchronizer(
    OperationContext* opCtx,
    CollectionShardingRuntime& csr,
    const NamespaceString& nss,
    const CancellationToken& recoveryCancellationToken) {
    assertNoCollectionCriticalSection(csr);

    if (csr.getCurrentMetadataIfKnown()) {
        csr.setNeedsDbPrimaryClassification(false);
        return nullptr;
    }

    ShardingStatistics::get(opCtx)
        .collectionShardingMetadataStatistics.registerMetadataSynchronizerCreated();

    auto synchronizer =
        std::make_shared<CollectionMetadataSynchronizer>(nss, recoveryCancellationToken);
    csr.setMetadataSynchronizer(synchronizer);
    return synchronizer;
}

CollectionMetadata drainSynchronizerOrAbort(
    OperationContext* opCtx,
    CollectionShardingRuntime& csr,
    const std::shared_ptr<CollectionMetadataSynchronizer>& synchronizer,
    const NamespaceString& nss) {
    auto metadata = synchronizer->drainAndApply(opCtx);
    if (!metadata) {
        uasserted(ErrorCodes::PlacementVersionRefreshCanceled,
                  "Authoritative collection metadata recovery aborted: invalidate during drain");
    }

    assertNoCollectionCriticalSection(csr);
    return std::move(*metadata);
}

void commitRecoveredMetadata(OperationContext* opCtx,
                             CollectionShardingRuntime& csr,
                             const CollectionMetadata& metadata,
                             CollectionShardingRuntime::NoRoutingTableAs noRoutingTableAs,
                             const CancellationToken& recoveryCancellationToken) {
    if (recoveryCancellationToken.isCanceled()) {
        return;
    }

    uassert(ErrorCodes::MetadataRefreshCanceledDueToFCVTransition,
            "Authoritative collection metadata refresh can't proceed: FCV has changed",
            sharding_ddl_util::getGrantedAuthoritativeMetadataAccessLevel(
                kVersionContextIgnored_UNSAFE,
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) ==
                AuthoritativeMetadataAccessLevelEnum::kWritesAndReadsAllowed);

    csr.setCollectionMetadata(opCtx, metadata, noRoutingTableAs);
    csr.setNeedsDbPrimaryClassification(false);
    csr.setMetadataSynchronizer(nullptr);

    ShardingStatistics::get(opCtx).collectionShardingMetadataStatistics.registerDiskRecovery();
}

bool needsEmptyCatalogClassification(ServiceContext* serviceContext, const NamespaceString& nss) {
    auto executor = Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor();
    RecoveryOpCtx recoveryOpCtx(serviceContext,
                                "AuthoritativeDiskRecoveryModeCheck",
                                CancellationToken::uncancelable(),
                                executor);
    auto scopedCsr = CollectionShardingRuntime::acquireShared(recoveryOpCtx.opCtx(), nss);
    return scopedCsr->needsDbPrimaryClassification() && !scopedCsr->getCurrentMetadataIfKnown();
}

template <typename PrepareFn, typename DrainFn>
SemiFuture<void> runSynchronizerRecovery(ServiceContext* serviceContext,
                                         const NamespaceString& nss,
                                         const CancellationToken& recoveryCancellationToken,
                                         PrepareFn&& prepare,
                                         DrainFn&& drain) {
    auto executor = Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor();
    std::shared_ptr<CollectionMetadataSynchronizer> synchronizer;

    {
        RecoveryOpCtx recoveryOpCtx(serviceContext,
                                    "AuthoritativeDiskRecoveryPrepare",
                                    recoveryCancellationToken,
                                    executor);
        auto* const opCtx = recoveryOpCtx.opCtx();
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
            synchronizer = prepare(opCtx, *scopedCsr);
            if (!synchronizer) {
                return Future<void>::makeReady().semi();
            }
        }

        hangInRecoverRefreshThread.pauseWhileSet(opCtx);

        synchronizer->start(opCtx, executor);
    }

    return synchronizer->getMetadataFuture()
        .thenRunOn(executor)
        .ignoreValue()
        .then([=, drain = std::decay_t<DrainFn>(std::forward<DrainFn>(drain))] {
            RecoveryOpCtx recoveryOpCtx(serviceContext,
                                        "AuthoritativeDiskRecoveryDrain",
                                        recoveryCancellationToken,
                                        executor);
            auto* const opCtx = recoveryOpCtx.opCtx();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, nss);
            auto metadata = drainSynchronizerOrAbort(opCtx, *scopedCsr, synchronizer, nss);
            drain(opCtx, *scopedCsr, metadata);
        })
        .onCompletion([=](Status status) {
            cleanupMetadataSynchronizer(serviceContext, nss);
            return status;
        })
        .semi();
}

SemiFuture<void> recoverShardCatalogFromDisk(ServiceContext* serviceContext,
                                             const NamespaceString& nss,
                                             const CancellationToken& token) {
    return runSynchronizerRecovery(
        serviceContext,
        nss,
        token,
        [&](OperationContext* opCtx,
            CollectionShardingRuntime& csr) -> std::shared_ptr<CollectionMetadataSynchronizer> {
            if (csr.needsDbPrimaryClassification()) {
                return nullptr;
            }
            return createMetadataSynchronizer(opCtx, csr, nss, token);
        },
        [=](OperationContext* opCtx,
            CollectionShardingRuntime& csr,
            const CollectionMetadata& metadata) {
            // Empty catalog: hand off to untracked/unowned classification via
            // needsDbPrimaryClassification.
            if (!metadata.hasRoutingTable()) {
                csr.setNeedsDbPrimaryClassification(true);
                csr.setMetadataSynchronizer(nullptr);
                return;
            }

            // Routing table present: install as kTracked (noRoutingTableAs unused).
            commitRecoveredMetadata(
                opCtx, csr, metadata, CollectionShardingRuntime::NoRoutingTableAs::kUnowned, token);
        });
}

SemiFuture<void> recoverEmptyCatalogFromDisk(ServiceContext* serviceContext,
                                             const NamespaceString& nss,
                                             const CancellationToken& token) {
    const auto dbName = nss.dbName();

    // Snapshot the DB primary (and a mutation counter) before the async disk read, then re-check
    // under the CSR at install time. Untracked vs unowned depends on whether this shard is the DB
    // primary; if the DSR changed while we were off-CSR reading disk, abort and let the outer
    // mismatch loop retry.

    struct DbPrimarySnapshot {
        boost::optional<ShardId> shardId;
        uint64_t mutations{0};
    };
    auto dbPrimarySnapshot = std::make_shared<DbPrimarySnapshot>();

    return runSynchronizerRecovery(
        serviceContext,
        nss,
        token,
        [=](OperationContext* opCtx,
            CollectionShardingRuntime& csr) -> std::shared_ptr<CollectionMetadataSynchronizer> {
            {
                auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, dbName);
                assertNoDatabaseCriticalSection(*scopedDsr);
                dbPrimarySnapshot->shardId = scopedDsr->getDbPrimaryShard(opCtx);
                dbPrimarySnapshot->mutations = scopedDsr->getNumMetadataMutations();
            }

            return createMetadataSynchronizer(opCtx, csr, nss, token);
        },
        [=](OperationContext* opCtx,
            CollectionShardingRuntime& csr,
            const CollectionMetadata& metadata) {
            auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, dbName);
            assertNoDatabaseCriticalSection(*scopedDsr);

            if (dbPrimarySnapshot->shardId != scopedDsr->getDbPrimaryShard(opCtx) ||
                dbPrimarySnapshot->mutations != scopedDsr->getNumMetadataMutations()) {
                LOGV2(12383201,
                      "Authoritative metadata recovery aborted: DSR changed during drain",
                      logAttrs(nss),
                      "snapshottedPrimary"_attr = dbPrimarySnapshot->shardId,
                      "currentPrimary"_attr = scopedDsr->getDbPrimaryShard(opCtx),
                      "snapshottedMutations"_attr = dbPrimarySnapshot->mutations,
                      "currentMutations"_attr = scopedDsr->getNumMetadataMutations());

                uasserted(ErrorCodes::PlacementVersionRefreshCanceled,
                          "Authoritative collection metadata recovery aborted: DSR changed "
                          "during drain");
            }

            const auto noRoutingTableAs =
                dbPrimarySnapshot->shardId == ShardingState::get(opCtx)->shardId()
                ? CollectionShardingRuntime::NoRoutingTableAs::kUntracked
                : CollectionShardingRuntime::NoRoutingTableAs::kUnowned;

            commitRecoveredMetadata(opCtx, csr, metadata, noRoutingTableAs, token);
        });
}

SharedSemiFuture<void> spawnDiskRecovery(ServiceContext* serviceContext,
                                         const NamespaceString& nss,
                                         CancellationToken token) {
    auto executor = Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor();
    auto diskRecoveryTimer = std::make_shared<Timer>();

    return ExecutorFuture<void>(executor)
        .then([=] { return recoverShardCatalogFromDisk(serviceContext, nss, token); })
        .then([=]() -> SemiFuture<void> {
            if (!needsEmptyCatalogClassification(serviceContext, nss)) {
                return Status::OK();
            }
            return recoverEmptyCatalogFromDisk(serviceContext, nss, token);
        })
        .onCompletion([=](Status status) {
            {
                RecoveryOpCtx recoveryOpCtx(serviceContext,
                                            "AuthoritativeDiskRecoveryComplete",
                                            CancellationToken::uncancelable(),
                                            executor);
                auto scopedCsr =
                    CollectionShardingRuntime::acquireExclusive(recoveryOpCtx.opCtx(), nss);
                scopedCsr->resetPlacementVersionRecoverRefreshFuture();
            }

            if (token.isCanceled() &&
                (status.isOK() || status == ErrorCodes::Interrupted ||
                 status == ErrorCodes::CallbackCanceled)) {
                uasserted(ErrorCodes::PlacementVersionRefreshCanceled,
                          "Authoritative collection metadata recovery from disk canceled");
            }

            if (status.isOK()) {
                ShardingStatistics::get(serviceContext)
                    .collectionShardingMetadataStatistics.registerDiskRecoveryMillis(
                        diskRecoveryTimer->millis());
            }

            return status;
        })
        .semi()
        .share();
}

AttemptResult waitForConfigTimeOrShardVersionChange(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    const ChunkVersion& receivedShardVersion) {
    if (storageGlobalParams.queryableBackupMode) {
        LOGV2_DEBUG(
            12744400,
            2,
            "Authoritative metadata recovery: skipping configTime wait in queryable backup mode",
            logAttrs(nss),
            "shardVersionReceived"_attr = receivedShardVersion);
        return AttemptResult::kDone;
    }

    // Use configTime as the upper bound for the wait. Once configTime is reached with majority
    // read concern, all DDL critical sections that could have changed the shard version have been
    // applied on this node.
    const auto vectorClock = VectorClock::get(opCtx)->getTime();
    const auto configTime = vectorClock.configTime();
    const auto timeToWaitFor =
        repl::OpTime{configTime.asTimestamp(), repl::OpTime::kUninitializedTerm};

    LOGV2_DEBUG(12307905,
                2,
                "Authoritative metadata recovery: waiting for shard version match or configTime",
                logAttrs(nss),
                "shardVersionReceived"_attr = receivedShardVersion,
                "configTime"_attr = timeToWaitFor);

    // Best-effort: the shared helper batches concurrent callers, waits briefly for replication to
    // catch up on secondaries, and retries on failure. The majority commit point follows the
    // last-written OpTime, so advancing it is sufficient to unblock the majority read concern wait
    // below.
    Status noopStatus = makeNoopWriteToAdvanceClusterTime(opCtx, configTime);
    if (MONGO_unlikely(forceNoopWriteToAdvanceConfigTimeToFail.shouldFail())) {
        noopStatus = Status(ErrorCodes::InternalError,
                            "forceNoopWriteToAdvanceConfigTimeToFail fail point is enabled");
    }
    maybeThrowStaleConfigOnNoopWriteFailure(
        opCtx, nss, receivedShardVersion, timeToWaitFor, noopStatus);

    // Race two futures: (1) majority read concern reaching configTime, which proves all DDLs have
    // been applied and the router must be stale, or (2) the shard version matching the router's
    // via oplog application. Whichever completes first allows the caller to return authoritatively.
    Timer postRecoveryWaitTimer;
    auto majorityFuture =
        repl::ReplicationCoordinator::get(opCtx)->registerWaiterForMajorityReadOpTime(
            opCtx, timeToWaitFor);
    auto versionFuture =
        CollectionShardingRuntime::acquireShared(opCtx, nss)
            ->registerWaiterForChunkVersion(opCtx, ShardVersionFactory::make(receivedShardVersion));

    const auto fixedExecutor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    Status status = [&] {
        if (MONGO_unlikely(forceWaitForVersionOnly.shouldFail())) {
            return versionFuture.getNoThrow();
        }
        return whenAny(versionFuture.thenRunOn(fixedExecutor),
                       majorityFuture.thenRunOn(fixedExecutor))
            .get(opCtx)
            .result;
    }();

    if (!status.isOK()) {
        // clearCollectionMetadata cancels the CSS version waiter with CallbackCanceled. When that
        // happens the metadata we recovered is now stale, so the caller must retry the recovery.
        if (status.code() == ErrorCodes::CallbackCanceled) {
            return AttemptResult::kRetry;
        }
        uassertStatusOK(status);
    }

    auto& stats = ShardingStatistics::get(opCtx).collectionShardingMetadataStatistics;
    stats.registerPostRecoveryWaitMillis(postRecoveryWaitTimer.millis());
    if (versionFuture.isReady()) {
        stats.registerPostRecoveryWaitResolvedByVersionChange();
    } else {
        stats.registerPostRecoveryWaitResolvedByConfigTime();
    }
    return AttemptResult::kDone;
}

bool isRecoveredShardVersionSufficient(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& installedShardVersion,
    bool isUnowned,
    const boost::optional<ChunkVersion>& optReceivedShardVersion) {
    // No specific version requested. The caller just wanted a forced recovery from disk, which
    // has already completed in the previous step.
    if (!optReceivedShardVersion) {
        LOGV2(12307912,
              "Authoritative collection metadata recovery completed successfully",
              logAttrs(nss),
              "outcome"_attr = "forcedDiskRecovery",
              "durationMillis"_attr = durationCount<Milliseconds>(opCtx->getElapsedTime()));
        return true;
    }

    const auto receivedShardVersion = *optReceivedShardVersion;

    // If the received version is ignored and the shard metadata is known, the shard is
    // recovered. The database version check is handled separately by the authoritative DB
    // versioning protocol.
    if (receivedShardVersion == ChunkVersion::IGNORED()) {
        LOGV2_DEBUG(12307906,
                    3,
                    "Authoritative metadata recovery: received version is ignored and the "
                    "shard metadata is known",
                    logAttrs(nss),
                    "receivedShardVersion"_attr = receivedShardVersion);
        return true;
    }

    // UNOWNED means this shard holds nothing. Any router view also reporting 0 chunks is
    // compatible even if the collection generations differ, because we cannot tell whether the
    // collection is tracked elsewhere.
    //
    // A router targeting a shard with a "tracked + 0 chunks" version (`{e,t,0,0}`) does not
    // really make sense in practice. If the router knows the shard owns nothing it should not
    // route to it, but if it does happen, the two views are still consistent, so we accept.
    if (isUnowned && !receivedShardVersion.isSet()) {
        LOGV2_DEBUG(12383202,
                    3,
                    "Authoritative metadata recovery: shard is UNOWNED and router also reports "
                    "no chunks on this shard, versions are compatible",
                    logAttrs(nss),
                    "installedShardVersion"_attr = installedShardVersion,
                    "receivedShardVersion"_attr = receivedShardVersion);
        return true;
    }

    // Both untracked: the shard is recovered. The database version check is handled separately
    // by the authoritative DB versioning protocol.
    if (receivedShardVersion == ChunkVersion::UNTRACKED() &&
        installedShardVersion == ChunkVersion::UNTRACKED()) {
        LOGV2_DEBUG(12307907,
                    3,
                    "Authoritative metadata recovery: both router and shard versions are "
                    "untracked, versions are comparable",
                    logAttrs(nss));
        return true;
    }

    // Both tracked: accept when received <= installed (shard up to date, or router stale). If the
    // comparison yields unordered (e.g. one tracked and one untracked) or received > installed,
    // fall through and return false so the configTime wait runs.
    auto compareResult = receivedShardVersion <=> installedShardVersion;
    if (compareResult == std::partial_ordering::less ||
        compareResult == std::partial_ordering::equivalent) {
        LOGV2_DEBUG(
            12307908,
            3,
            "Authoritative metadata recovery: shard version is up to date or router is stale",
            logAttrs(nss),
            "installedShardVersion"_attr = installedShardVersion,
            "receivedShardVersion"_attr = receivedShardVersion);
        return true;
    }

    LOGV2_DEBUG(12307909,
                3,
                "Authoritative metadata recovery: shard and router versions are not "
                "comparable, configTime wait required",
                logAttrs(nss),
                "installedShardVersion"_attr = installedShardVersion,
                "receivedShardVersion"_attr = receivedShardVersion);

    return false;
}

}  // namespace

AttemptResult onShardVersionMismatchAuthoritative(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<ChunkVersion> receivedShardVersion) {
    // Step 1: Recover the shard's current metadata from the authoritative on-disk catalog. This
    // ensures the CSS reflects the current disk durable state.
    ChunkVersion installedShardVersion;
    bool isUnowned;
    {
        auto scopedCsr =
            boost::make_optional(CollectionShardingRuntime::acquireExclusive(opCtx, nss));

        if (waitDbPrimaryCriticalSectionIfNeeded(opCtx, nss, scopedCsr)) {
            return AttemptResult::kRetry;
        }

        if (refresh_util::joinPlacementVersionOperations(opCtx, scopedCsr)) {
            return AttemptResult::kRetry;
        }

        const auto knownMetadata = (*scopedCsr)->getCurrentMetadataIfKnown();
        if (!knownMetadata) {
            // Whether spawn succeeds or FCV rejects it, retry. The outer loop re-samples FCV, and a
            // successful spawn is joined via joinPlacementVersionOperations on the next attempt.
            (void)refresh_util::spawnTrackedCollectionRecovery(
                opCtx,
                *scopedCsr,
                RecoveryKind::kAuthoritative,
                [&](const CancellationToken& cancellationToken) {
                    return spawnDiskRecovery(opCtx->getServiceContext(), nss, cancellationToken);
                });
            return AttemptResult::kRetry;
        }

        installedShardVersion = knownMetadata->getShardPlacementVersion();
        isUnowned = (*scopedCsr)->isUnowned();
    }

    // Step 2: If both the router's version and the shard's version are comparable (both tracked
    // or both untracked), we can use the partial ordering to determine whether the shard is
    // already up to date or the router is stale. In either case the caller can return to the
    // retry loop.
    if (isRecoveredShardVersionSufficient(
            opCtx, nss, installedShardVersion, isUnowned, receivedShardVersion)) {
        ShardingStatistics::get(opCtx)
            .collectionShardingMetadataStatistics.registerVersionMismatchResolved();

        LOGV2(12307913,
              "Authoritative collection metadata recovery completed successfully",
              logAttrs(nss),
              "outcome"_attr = "versionMatched",
              "durationMillis"_attr = durationCount<Milliseconds>(opCtx->getElapsedTime()));

        return AttemptResult::kDone;
    }

    // Step 3: The versions are not comparable (e.g. one is tracked and the other is untracked).
    // Wait until the shard version matches the router's or configTime is reached.
    invariant(receivedShardVersion);
    if (waitForConfigTimeOrShardVersionChange(opCtx, nss, *receivedShardVersion) ==
        AttemptResult::kRetry) {
        return AttemptResult::kRetry;
    }

    ShardingStatistics::get(opCtx)
        .collectionShardingMetadataStatistics.registerVersionMismatchResolved();

    LOGV2(12307914,
          "Authoritative collection metadata recovery completed successfully",
          logAttrs(nss),
          "outcome"_attr = "shardVersionMatchOrConfigTimeWait",
          "durationMillis"_attr = durationCount<Milliseconds>(opCtx->getElapsedTime()));

    return AttemptResult::kDone;
}

}  // namespace shard_catalog_recoverer
}  // namespace mongo
