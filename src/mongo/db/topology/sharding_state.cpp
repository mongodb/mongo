// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/sharding_state.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const auto getShardingState = ServiceContext::declareDecoration<boost::optional<ShardingState>>();

}  // namespace

ShardingState::ShardingState(bool inMaintenanceMode) : _inMaintenanceMode(inMaintenanceMode) {
    auto [promise, future] = makePromiseFuture<RecoveredClusterRole>();
    _promise = std::move(promise);
    _future = std::move(future).tapAll(
        [this](auto sw) { _awaitClusterRoleRecoveryPromise.setFrom(std::move(sw)); });
}

ShardingState::~ShardingState() = default;

void ShardingState::create(ServiceContext* serviceContext) {
    auto& shardingState = getShardingState(serviceContext);
    invariant(!shardingState);
    shardingState.emplace(serverGlobalParams.maintenanceMode !=
                          ServerGlobalParams::MaintenanceMode::None);
}

ShardingState* ShardingState::get(ServiceContext* serviceContext) {
    auto& shardingState = getShardingState(serviceContext);
    return shardingState.get_ptr();
}

ShardingState* ShardingState::get(OperationContext* operationContext) {
    return ShardingState::get(operationContext->getServiceContext());
}

bool ShardingState::inMaintenanceMode() const {
    return _inMaintenanceMode;
}

void ShardingState::setRecoveryCompleted(RecoveredClusterRole role) {
    LOGV2(
        22081, "Sharding status of the node recovered successfully", "role"_attr = role.toString());
    std::unique_lock<std::mutex> ul(_mutex);
    invariant(!_future.isReady());

    _promise.emplaceValue(std::move(role));
}

void ShardingState::setRecoveryFailed(Status failedStatus) {
    invariant(!failedStatus.isOK());
    LOGV2(22082, "Sharding status of the node failed to recover", "error"_attr = failedStatus);

    std::unique_lock<std::mutex> ul(_mutex);
    invariant(!_future.isReady());

    _promise.setError(
        {ErrorCodes::ManualInterventionRequired,
         str::stream()
             << "This instance's sharding role failed to initialize and will remain in this state "
                "until the process is restarted. In addition some manual intervention might be "
                "required, which would require the node to be started in maintenance mode."
             << causedBy(failedStatus)});
}

SharedSemiFuture<ShardingState::RecoveredClusterRole> ShardingState::awaitClusterRoleRecovery() {
    return _awaitClusterRoleRecoveryPromise.getFuture();
}

boost::optional<ClusterRole> ShardingState::pollClusterRole() const {
    if (!_future.isReady())
        return boost::none;

    return _future.get().role;
}

bool ShardingState::enabled() const {
    if (inMaintenanceMode())
        return false;

    const auto role = pollClusterRole();
    return role && (role->has(ClusterRole::ConfigServer) || role->has(ClusterRole::ShardServer));
}

void ShardingState::assertCanAcceptShardedCommands() const {
    if (pollClusterRole())
        return;

    uasserted(ErrorCodes::ShardingStateNotInitialized,
              "Cannot accept sharding commands if sharding state has not been initialized with a "
              "shardIdentity document");
}

ShardId ShardingState::shardId() const {
    uassert(ErrorCodes::ShardingStateNotInitialized,
            "ShardingState cannot be accessed before it is initialized",
            _future.isReady());

    return _future.get().shardId;
}

OID ShardingState::clusterId() const {
    invariant(_future.isReady());
    const auto& role = _future.get();
    return role.clusterId;
}

std::string ShardingState::RecoveredClusterRole::toString() const {
    return str::stream() << clusterId << " : " << role << " : " << configShardConnectionString
                         << " : " << shardId;
}

}  // namespace mongo
