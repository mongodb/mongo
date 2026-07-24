// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/critical_section_signal.h"
#include "mongo/db/shard_role/shard_catalog/shard_catalog_recoverer_tracker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <utility>

#include <boost/optional.hpp>

namespace mongo {

extern FailPoint hangBeforePlacementVersionCriticalSectionWait;

namespace refresh_util {

/**
 * Best-effort wait for a refresh to complete. Returns false instead of throwing when the refresh
 * was canceled. Other errors still propagate.
 *
 * All waits on refreshes in a shard should go through this code path, because it also accounts for
 * transactions and locking.
 */
[[MONGO_MOD_PRIVATE]] bool waitForRefreshToComplete(OperationContext* opCtx,
                                                    const SharedSemiFuture<void>& refresh);

/**
 * This method implements a best-effort attempt to wait for the critical section to complete
 * before returning to the router at the previous step in order to prevent it from busy spinning
 * while the critical section is in progress.
 *
 * All waits for migration critical section should go through this code path, because it also
 * accounts for transactions and locking.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status waitForCriticalSectionToComplete(
    OperationContext* opCtx, const CriticalSectionSignal& critSecSignal) noexcept;

/**
 * Blocking method, which will wait for any critical section to be released.
 *
 * Returns 'true' if there were concurrent metadata refreshes that had to be waited (in which case
 * the sharding runtime mutex is released). If there were none, returns 'false' and the sharding
 * runtime mutex continues to be held.
 */
template <typename ScopedShardingRuntime>
[[MONGO_MOD_PRIVATE]] bool waitForCriticalSectionIfNeeded(
    OperationContext* opCtx, boost::optional<ScopedShardingRuntime>& scopedShardingRuntime) {
    invariant(scopedShardingRuntime.has_value());

    if (auto critSect = (*scopedShardingRuntime)
                            ->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite)) {
        scopedShardingRuntime.reset();

        hangBeforePlacementVersionCriticalSectionWait.pauseWhileSet(opCtx);
        uassertStatusOK(waitForCriticalSectionToComplete(opCtx, *critSect));

        return true;
    }

    return false;
}

/**
 * Blocking method, which will wait for any concurrent database or collection metadata refresh to
 * complete.
 *
 * Returns 'true' if there were concurrent metadata refreshes that had to be joined (in which case
 * the sharding runtime mutex is released). If there were none, returns 'false' and the sharding
 * runtime mutex continues to be held.
 */
template <typename ScopedShardingRuntime>
[[MONGO_MOD_PRIVATE]] bool waitForOngoingMetadataRefreshToComplete(
    OperationContext* opCtx, boost::optional<ScopedShardingRuntime>& scopedShardingRuntime) {
    invariant(scopedShardingRuntime.has_value());

    if (auto refreshVersionFuture = (*scopedShardingRuntime)->getMetadataRefreshFuture()) {
        scopedShardingRuntime.reset();
        (void)waitForRefreshToComplete(opCtx, *refreshVersionFuture);
        return true;
    }

    return false;
}

/**
 * Blocking method that waits for any concurrent operations which could change the placement version
 * to complete.
 *
 * Returns 'true' if there were concurrent operations that had to be joined (in which case the
 * sharding runtime mutex is released). If there were none, returns 'false' and the sharding runtime
 * mutex continues to be held.
 */
template <typename ScopedShardingRuntime>
[[MONGO_MOD_PRIVATE]] bool joinPlacementVersionOperations(
    OperationContext* opCtx, boost::optional<ScopedShardingRuntime>& scopedShardingRuntime) {
    if (waitForCriticalSectionIfNeeded(opCtx, scopedShardingRuntime)) {
        return true;
    }

    return waitForOngoingMetadataRefreshToComplete(opCtx, scopedShardingRuntime);
}

/**
 * Acquires a tracker registration and installs the spawned recovery future. The acquisition is the
 * sole cancel handle for the in-flight recovery.
 *
 * Returns false if the live FCV no longer matches 'kind'.
 */
template <typename ScopedCsr, typename SpawnFn>
[[MONGO_MOD_PRIVATE]] bool spawnTrackedCollectionRecovery(OperationContext* opCtx,
                                                          ScopedCsr& scopedCsr,
                                                          RecoveryKind kind,
                                                          SpawnFn&& spawn) {
    auto recovererTrackerAcquisition = ShardCatalogRecovererTracker::get(opCtx)->acquire(opCtx);
    if (recovererTrackerAcquisition.kind() != kind) {
        return false;
    }
    const auto cancellationToken = recovererTrackerAcquisition.cancellationToken();
    scopedCsr->setPlacementVersionRecoverRefreshFuture(spawn(cancellationToken),
                                                       std::move(recovererTrackerAcquisition));
    return true;
}

/**
 * Like spawnTrackedCollectionRecovery, but installs the future on a DatabaseShardingRuntime.
 */
template <typename ScopedDsr, typename SpawnFn>
[[MONGO_MOD_PRIVATE]] bool spawnTrackedDbRecovery(OperationContext* opCtx,
                                                  ScopedDsr& scopedDsr,
                                                  RecoveryKind kind,
                                                  SpawnFn&& spawn) {
    auto recovererTrackerAcquisition = ShardCatalogRecovererTracker::get(opCtx)->acquire(opCtx);
    if (recovererTrackerAcquisition.kind() != kind) {
        return false;
    }
    const auto cancellationToken = recovererTrackerAcquisition.cancellationToken();
    scopedDsr->setDbMetadataRefreshFuture_DEPRECATED(spawn(cancellationToken),
                                                     std::move(recovererTrackerAcquisition));
    return true;
}

}  // namespace refresh_util

}  // namespace mongo
