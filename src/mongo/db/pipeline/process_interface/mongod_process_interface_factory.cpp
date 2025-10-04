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
