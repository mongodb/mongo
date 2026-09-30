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

/**
 * Marks the search executors as shutting down, so that getMongotTaskExecutor() and
 * getSearchIndexManagementTaskExecutor() reject new work (ErrorCodes::ShutdownInProgress).
 *
 * This must be called before any shutdown-time draining of cursors, so that no new $search cursor
 * (and therefore no new PinnedConnectionTaskExecutor) can be established during the whole shutdown
 * window. Otherwise a cursor created after the drain can keep the underlying executor's completion
 * path alive past executor shutdown.
 */
[[MONGO_MOD_PUBLIC]] void beginSearchExecutorShutdown(ServiceContext* svc);

[[MONGO_MOD_PUBLIC]] void shutdownSearchExecutorsIfNeeded(ServiceContext* svc);

}  // namespace executor
}  // namespace mongo
