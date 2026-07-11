// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/analyze_shard_key_role.h"

#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/topology/cluster_role.h"

namespace mongo {
namespace analyze_shard_key {

namespace {

bool isReplEnabled(ServiceContext* serviceContext) {
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        return false;
    }
    auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
    return replCoord && replCoord->getSettings().isReplSet();
}

}  // namespace

bool supportsCoordinatingQueryAnalysis(bool isReplEnabled) {
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        return false;
    }
    return isReplEnabled && !gMultitenancySupport &&
        (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::None));
}

bool supportsCoordinatingQueryAnalysis(OperationContext* opCtx) {
    return supportsCoordinatingQueryAnalysis(isReplEnabled(opCtx->getServiceContext()));
}

bool supportsPersistingSampledQueries(bool isReplEnabled) {
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        return false;
    }
    return isReplEnabled && !gMultitenancySupport &&
        (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::None));
}

bool supportsPersistingSampledQueries(OperationContext* opCtx) {
    return supportsPersistingSampledQueries(isReplEnabled(opCtx->getServiceContext()));
}

bool supportsSamplingQueries(bool isReplEnabled) {
    if (serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        return true;
    }
    return isReplEnabled && !gMultitenancySupport &&
        (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::None));
}

bool supportsSamplingQueries(ServiceContext* serviceContext) {
    return supportsSamplingQueries(isReplEnabled(serviceContext));
}

bool supportsSamplingQueries(OperationContext* opCtx) {
    return supportsSamplingQueries(opCtx->getServiceContext());
}

}  // namespace analyze_shard_key
}  // namespace mongo
