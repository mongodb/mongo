// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class ServiceContext;

namespace executor {

/**
 * Provides access to a service context scoped task executor for mongot.
 * Returns ErrorCodes::ShutdownInProgress if shutdown has begun.
 */
[[MONGO_MOD_PUBLIC]] StatusWith<std::shared_ptr<TaskExecutor>> getMongotTaskExecutor(
    ServiceContext* svc);

/**
 * Provides access to a service context scoped task executor for the search-index-management server.
 * Returns ErrorCodes::ShutdownInProgress if shutdown has begun.
 */
[[MONGO_MOD_PUBLIC]] StatusWith<std::shared_ptr<TaskExecutor>> getSearchIndexManagementTaskExecutor(
    ServiceContext* svc);

/**
 * Starts up the search executors if configured.
 */
[[MONGO_MOD_PUBLIC]] void startupSearchExecutorsIfNeeded(ServiceContext* svc);

[[MONGO_MOD_PUBLIC]] void shutdownSearchExecutorsIfNeeded(ServiceContext* svc);

}  // namespace executor
}  // namespace mongo
