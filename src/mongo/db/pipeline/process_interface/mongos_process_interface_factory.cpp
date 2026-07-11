// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/shim.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/process_interface/mongos_process_interface.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/executor/task_executor_pool.h"

#include <memory>
#include <string>

namespace mongo {
namespace {

std::shared_ptr<MongoProcessInterface> MongoProcessInterfaceCreateImpl(OperationContext* opCtx) {
    return std::make_shared<MongosProcessInterface>(
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());
}

auto mongoProcessInterfaceCreateRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    MongoProcessInterface::create, MongoProcessInterfaceCreateImpl);

}  // namespace
}  // namespace mongo
