// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/pinned_connection_task_executor_registry.h"

#include <list>

namespace mongo::executor {

struct PinnedConnectionTaskExecutorRegistry {
    std::list<ExecutorPair> entries;
};

const auto getRegistry =
    ServiceContext::declareDecoration<synchronized_value<PinnedConnectionTaskExecutorRegistry>>();

PinnedExecutorRegistryToken::PinnedExecutorRegistryToken(ServiceContext* svc,
                                                         std::shared_ptr<TaskExecutor> pinned,
                                                         std::shared_ptr<TaskExecutor> underlying)
    : _svc(svc) {
    auto reg = getRegistry(_svc).synchronize();
    // Insert the new ExecutorPair and hold the returned iterator for O(1) removal.
    _it = reg->entries.emplace(reg->entries.end(),
                               ExecutorPair{std::move(pinned), std::move(underlying)});
}

PinnedExecutorRegistryToken::~PinnedExecutorRegistryToken() {
    // Erase this token's ExecutorPair using the iterator held at construction time.
    getRegistry(_svc)->entries.erase(_it);
}

void shutdownPinnedExecutors(ServiceContext* svc, const std::shared_ptr<TaskExecutor>& underlying) {
    std::list<std::shared_ptr<TaskExecutor>> toShutdown;
    // Check the registry for all ExecutorPairs that contain 'underlying' and move all the
    // corresponding PCTEs to 'toShutdown'.
    for (auto& entry : getRegistry(svc)->entries) {
        if (entry.underlying.lock() == underlying) {
            toShutdown.emplace_back(entry.pinned.lock());
        }
    }

    for (auto& pinned : toShutdown) {
        pinned->shutdown();
        pinned->join();
    }
}
}  // namespace mongo::executor
