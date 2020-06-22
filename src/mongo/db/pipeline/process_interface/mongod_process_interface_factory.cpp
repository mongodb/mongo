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

#include "mongo/platform/basic.h"

#include "mongo/base/shim.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/pipeline/process_interface/replica_set_node_process_interface.h"
#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

std::shared_ptr<MongoProcessInterface> MongoProcessInterfaceCreateImpl(OperationContext* opCtx) {
    if (ShardingState::get(opCtx)->enabled()) {
        return std::make_shared<ShardServerProcessInterface>(
            opCtx, Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());
    } else if (auto executor = ReplicaSetNodeProcessInterface::getReplicaSetNodeExecutor(opCtx)) {
        return std::make_shared<ReplicaSetNodeProcessInterface>(std::move(executor));
    }
    return std::make_shared<StandaloneProcessInterface>(nullptr);
}

auto mongoProcessInterfaceCreateRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    MongoProcessInterface::create, MongoProcessInterfaceCreateImpl);

}  // namespace
}  // namespace mongo
