// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/cluster_identity_loader.h"

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/status_with.h"
#include "mongo/db/global_catalog/type_config_version_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const auto getClusterIdentity = ServiceContext::declareDecoration<ClusterIdentityLoader>();

}  // namespace

ClusterIdentityLoader* ClusterIdentityLoader::get(ServiceContext* serviceContext) {
    return &getClusterIdentity(serviceContext);
}

ClusterIdentityLoader* ClusterIdentityLoader::get(OperationContext* operationContext) {
    return ClusterIdentityLoader::get(operationContext->getServiceContext());
}

OID ClusterIdentityLoader::getClusterId() {
    // TODO SERVER-78051: Re-evaluate use of ClusterIdentityLoader for shards.
    tassert(7800000,
            "Unexpectedly tried to get cluster id on a non config server node",
            serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

    std::unique_lock<std::mutex> lk(_mutex);
    invariant(_initializationState == InitializationState::kInitialized && _lastLoadResult.isOK());
    return _lastLoadResult.getValue();
}

Status ClusterIdentityLoader::loadClusterId(OperationContext* opCtx,
                                            ShardingCatalogClient* catalogClient,
                                            const repl::ReadConcernLevel& readConcernLevel) {
    std::unique_lock<std::mutex> lk(_mutex);
    if (_initializationState == InitializationState::kInitialized) {
        invariant(_lastLoadResult.getStatus());
        return Status::OK();
    }

    if (_initializationState == InitializationState::kLoading) {
        while (_initializationState == InitializationState::kLoading) {
            _inReloadCV.wait(lk);
        }
        return _lastLoadResult.getStatus();
    }

    invariant(_initializationState == InitializationState::kUninitialized);
    _initializationState = InitializationState::kLoading;

    lk.unlock();
    auto loadStatus = _fetchClusterIdFromConfig(opCtx, catalogClient, readConcernLevel);
    lk.lock();

    invariant(_initializationState == InitializationState::kLoading);
    _lastLoadResult = std::move(loadStatus);
    if (_lastLoadResult.isOK()) {
        _initializationState = InitializationState::kInitialized;
    } else {
        _initializationState = InitializationState::kUninitialized;
    }
    _inReloadCV.notify_all();
    return _lastLoadResult.getStatus();
}

StatusWith<OID> ClusterIdentityLoader::_fetchClusterIdFromConfig(
    OperationContext* opCtx,
    ShardingCatalogClient* catalogClient,
    const repl::ReadConcernLevel& readConcernLevel) {
    auto loadResult =
        catalogClient->getConfigVersion(opCtx, repl::ReadConcernArgs{readConcernLevel});
    if (!loadResult.isOK()) {
        return loadResult.getStatus().withContext("Error loading clusterID");
    }
    return loadResult.getValue().getClusterId();
}

void ClusterIdentityLoader::discardCachedClusterId() {
    std::lock_guard<std::mutex> lk(_mutex);

    if (_initializationState == InitializationState::kUninitialized) {
        return;
    }
    invariant(_initializationState == InitializationState::kInitialized);
    _lastLoadResult = {
        Status{ErrorCodes::InternalError, "cluster ID never re-loaded after rollback"}};
    _initializationState = InitializationState::kUninitialized;
}

}  // namespace mongo
