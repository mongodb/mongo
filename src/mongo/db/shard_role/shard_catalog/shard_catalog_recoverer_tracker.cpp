// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/shard_catalog_recoverer_tracker.h"

#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/version_context.h"
#include "mongo/util/fail_point.h"

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterCancelingIncompatibleRecoveries);

const auto getShardCatalogRecovererTracker =
    ServiceContext::declareDecoration<ShardCatalogRecovererTracker>();

}  // namespace

ShardCatalogRecovererTracker* ShardCatalogRecovererTracker::get(OperationContext* opCtx) {
    return &getShardCatalogRecovererTracker(opCtx->getServiceContext());
}

ShardCatalogRecovererTracker::Acquisition ShardCatalogRecovererTracker::acquire(
    OperationContext* opCtx) {
    std::lock_guard lk(_mutex);
    // Check the feature flag inside the mutex in order to serialize with calls to
    // `interruptIncompatibleRecoveries` during FCV transitions.
    const auto kind = feature_flags::gAuthoritativeShardsCRUD.isEnabled(
                          kVersionContextIgnored_UNSAFE,
                          serverGlobalParams.featureCompatibility.acquireFCVSnapshot())
        ? RecoveryKind::kAuthoritative
        : RecoveryKind::kNonAuthoritative;
    auto it = _activeRecoveries.emplace(_activeRecoveries.end(), kind, CancellationSource{});
    return Acquisition(this, it);
}

void ShardCatalogRecovererTracker::_release(ActiveList::iterator it) {
    std::lock_guard lk(_mutex);
    if (it->source.token().isCanceled()) {
        _canceled.notify_all();
    }
    _activeRecoveries.erase(it);
}

void ShardCatalogRecovererTracker::interruptIncompatibleRecoveries(OperationContext* opCtx) {
    bool authoritativeEnabled = feature_flags::gAuthoritativeShardsCRUD.isEnabled(
        kVersionContextIgnored_UNSAFE,
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    const auto incompatibleKind =
        authoritativeEnabled ? RecoveryKind::kNonAuthoritative : RecoveryKind::kAuthoritative;

    {
        std::unique_lock lk(_mutex);
        for (auto& active : _activeRecoveries) {
            if (active.kind == incompatibleKind) {
                active.source.cancel();
            }
        }
        hangAfterCancelingIncompatibleRecoveries.pauseWhileSet();
        // Wait for all incompatible recoveries to be canceled and drained.
        opCtx->waitForConditionOrInterrupt(_canceled, lk, [&] {
            return std::none_of(
                _activeRecoveries.begin(), _activeRecoveries.end(), [&](const auto& active) {
                    return active.kind == incompatibleKind;
                });
        });
    }

    // Interrupt in-flight loads on the ShardServerCatalogCacheLoader. (These loads use a separate
    // pool, so they do not get interrupted when the shard catalog recovery is).
    if (authoritativeEnabled) {
        FilteringMetadataCache::get(opCtx)->interruptLoaderAfterAuthoritativeShardsTransition();
    }
}

}  // namespace mongo
