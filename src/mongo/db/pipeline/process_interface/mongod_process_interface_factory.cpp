// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/shim.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"
#include "mongo/db/pipeline/process_interface/replica_set_node_process_interface.h"
#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/transport/session.h"

#include <memory>
#include <string>
#include <utility>

namespace mongo {
namespace {

std::shared_ptr<MongoProcessInterface> MongoProcessInterfaceCreateImpl(OperationContext* opCtx) {
    // In the case where the client has connected directly to a shard rather than via mongoS, we
    // should behave exactly as we do when running on a standalone or single-replset deployment,
    // so we will use the standalone or replset process interfaces.
    const auto isInternalThreadOrClient =
        !opCtx->getClient()->session() || opCtx->getClient()->isInternalClient();
    const auto isRouterOperation =
        opCtx->getService()->role().hasExclusively(ClusterRole::RouterServer);
    // Router operations always use the router's process interface.
    if (isRouterOperation) {
        return std::make_shared<MongosProcessInterface>(
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());
    }
    if (ShardingState::get(opCtx)->enabled() && isInternalThreadOrClient) {
        return std::make_shared<ShardServerProcessInterface>(
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());
    } else if (auto executor = ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(opCtx)) {
        return std::make_shared<ReplicaSetNodeProcessInterface>(std::move(executor));
    }
    return std::make_shared<StandaloneProcessInterface>(nullptr);
}

auto mongoProcessInterfaceCreateRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    MongoProcessInterface::create, MongoProcessInterfaceCreateImpl);

}  // namespace
}  // namespace mongo
