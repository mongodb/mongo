// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo {

/**
 * NOTE:
 * Local executor is a ThreadPoolTaskExecutor executor that prohibits remote execution.
 * The prohibition is achieved by a hook on the network interface that asserts, so if the client
 * code tries to schedule a remote task on this executor, it will fail.
 * This executor is used in cases where we normally have to choose between ReplSetNodeExecutor and
 * the Grid's one. In those cases usually we select based on the serverGlobalParam (the startup
 * flag). But the fac that we have a cluster role doesn't mean the grid is already initialized (we
 * might still wait for the shardIdentity to initialize). Since this executor is always initialized
 * we can use this instead of the selection mechanism.
 */

/**
 * Returns the local executor.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::shared_ptr<executor::TaskExecutor> getLocalExecutor(
    ServiceContext* srvCtx);

/**
 * Returns the local executor.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::shared_ptr<executor::TaskExecutor> getLocalExecutor(
    OperationContext* opCtx);

/**
 * Sets the local executor.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void setLocalExecutor(
    ServiceContext* srvCtx, std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Creates a local executor.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] std::shared_ptr<executor::TaskExecutor> createLocalExecutor(
    ServiceContext* srvCtx, const std::string& name);

}  // namespace mongo
