// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/ddl/direct_connection_ddl_hook.h"

#include "mongo/db/shard_role/direct_connection_util.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"

namespace mongo {

void DirectConnectionDDLHook::create(ServiceContext* serviceContext) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        ReplicaSetDDLTracker::get(serviceContext)
            ->registerHook(std::make_unique<DirectConnectionDDLHook>());
    }
}


void DirectConnectionDDLHook::onBeginDDL(OperationContext* opCtx,
                                         const std::vector<NamespaceString>& namespaces) {
    // The "config.system.session" collection is managed internally, so direct DDL operations
    // (representing the only way to modify such collection) do not introduce any correctness issue.
    // This exception allows, for example, to drop the collection in support scenarios.
    if (namespaces.size() == 1 &&
        namespaces.front() == NamespaceString::kLogicalSessionsNamespace) {
        return;
    }

    const auto& opId = opCtx->getOpID();

    std::lock_guard lk(_mutex);
    // If this op was already registered for a prior acquisition, skip it now.
    auto ongoingOpIt = _ongoingOps.find(opId);
    if (ongoingOpIt != _ongoingOps.end()) {
        ongoingOpIt->second++;
        return;
    }

    // Checks if the operation is allowed to proceed and throws ErrorCodes::Unauthorized if not.
    // Skip these checks if the feature flag is disabled.
    if (feature_flags::gFeatureFlagPreventDirectShardDDLsDuringPromotion
            .isEnabledUseLastLTSFCVWhenUninitialized(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        if (namespaces.empty()) {
            direct_connection_util::checkDirectShardDDLAllowed(opCtx, NamespaceString{});
        }
        for (const auto& nss : namespaces) {
            direct_connection_util::checkDirectShardDDLAllowed(opCtx, nss);
        }
    }

    // Register the operation for later draining if needed. We skip registering the operation if
    // draining has started to ensure draining is possible once direct connections are disallowed -
    // this is needed because our method of checking direct operations doesn't tell us if something
    // is a direct operation only if it is an authorized direct operation or not. Thus authorized
    // direct DDLs and sharded DDLs will still pass through here after we block direct DDLs.
    if (!_drainingPromise.has_value()) {
        _ongoingOps.emplace(opId, 1);
    }
}

void DirectConnectionDDLHook::onEndDDL(OperationContext* opCtx,
                                       const std::vector<NamespaceString>& namespaces) {
    const auto& opId = opCtx->getOpID();
    std::lock_guard lk(_mutex);
    auto ongoingOpIt = _ongoingOps.find(opId);
    if (ongoingOpIt == _ongoingOps.end()) {
        return;
    }
    // Decrement the number of ongoing ops for this opId, if we have reached 0 then remove the entry
    // from the map.
    ongoingOpIt->second--;
    if (ongoingOpIt->second == 0) {
        _ongoingOps.erase(ongoingOpIt);
        // If this op was de-registered for the final time then we should check if there is a
        // promise to fulfill and this is the last ongoing op.
        if (_drainingPromise.has_value() && _ongoingOps.empty()) {
            _drainingPromise->emplaceValue();
        }
    }
}

SharedSemiFuture<void> DirectConnectionDDLHook::getWaitForDrainedFuture(OperationContext* opCtx) {
    std::lock_guard lk(_mutex);

    // If there are no ongoing ops, then return immediately a ready future.
    if (_ongoingOps.empty()) {
        return SemiFuture<void>::makeReady().share();
    }
    // If there is not already a future from a prior call, create one now.
    if (!_drainingPromise.has_value()) {
        _drainingPromise.emplace();
    }
    // Return the future to allow the caller to wait until drained.
    return _drainingPromise->getFuture().semi().share();
}

stdx::unordered_map<OperationId, int> DirectConnectionDDLHook::getOngoingOperations() const {
    std::lock_guard lk(_mutex);

    return _ongoingOps;
}

}  // namespace mongo
