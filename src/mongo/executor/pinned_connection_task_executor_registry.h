// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"

#include <list>
#include <memory>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] executor {

struct ExecutorPair {
    std::weak_ptr<TaskExecutor> pinned;
    std::weak_ptr<TaskExecutor> underlying;
};

/**
 * Registers a (pinned, underlying) ExecutorPair in the Registry on construction and unregisters the
 * same pair on destruction.
 */
class PinnedExecutorRegistryToken {
public:
    PinnedExecutorRegistryToken(ServiceContext* svc,
                                std::shared_ptr<TaskExecutor> pinned,
                                std::shared_ptr<TaskExecutor> underlying);
    ~PinnedExecutorRegistryToken();

private:
    ServiceContext* _svc;
    // Iterator to this token's registry entry.
    std::list<ExecutorPair>::iterator _it;
};

// Shutdown and join all pinned executors that were built on top of 'underlying'.
void shutdownPinnedExecutors(ServiceContext* svc, const std::shared_ptr<TaskExecutor>& underlying);

}  // namespace executor
}  // namespace mongo
