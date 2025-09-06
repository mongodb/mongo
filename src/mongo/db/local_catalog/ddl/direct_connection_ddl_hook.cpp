/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/ddl/direct_connection_ddl_hook.h"

#include "mongo/db/local_catalog/shard_role_api/direct_connection_util.h"
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

    stdx::lock_guard lk(_mutex);
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
    stdx::lock_guard lk(_mutex);
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
    stdx::lock_guard lk(_mutex);
    tassert(10920801,
            "Cannot drain direct DDL operations when "
            "featureFlagPreventDirectShardDDLsDuringPromotion is disabled",
            feature_flags::gFeatureFlagPreventDirectShardDDLsDuringPromotion.isEnabled(
                VersionContext::getDecoration(opCtx),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

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
    stdx::lock_guard lk(_mutex);

    return _ongoingOps;
}

}  // namespace mongo
