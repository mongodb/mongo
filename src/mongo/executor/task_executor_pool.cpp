// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/task_executor_pool.h"

#include "mongo/executor/task_executor_pool_parameters_gen.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/processinfo.h"  // IWYU pragma: keep

#include <cstdint>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kExecutor

namespace mongo {
namespace executor {

size_t TaskExecutorPool::getSuggestedPoolSize() {
    // The default task executor pool size is 1, which should be fine in almost all cases.
    // The default value can still be overriden via startup parameter if absolutely required.
    size_t numPools = []() -> size_t {
        if (gTaskExecutorPoolSize > 0) {
            return gTaskExecutorPoolSize;
        }

        ProcessInfo p;
        auto numCores = p.getNumAvailableCores();

        // Never suggest a number outside the range [4, 64].
        return std::max<size_t>(4U, std::min<size_t>(64U, numCores));
    }();

    if (numPools > 1) {
        LOGV2_WARNING(
            10247700,
            "The sharding task executor pool size is greater than one. This can have "
            "adverse effects on performance. To avoid this, set taskExecutorPoolSize to 1.",
            "taskExecutorPoolSize"_attr = gTaskExecutorPoolSize,
            "numPools"_attr = numPools);
    }

    return numPools;
}

void TaskExecutorPool::startup() {
    invariant(!_executors.empty());
    invariant(_fixedExecutor);

    _fixedExecutor->startup();
    for (auto&& exec : _executors) {
        exec->startup();
    }
}

void TaskExecutorPool::shutdownAndJoin() {
    _fixedExecutor->shutdown();
    _fixedExecutor->join();
    for (auto&& exec : _executors) {
        exec->shutdown();
        exec->join();
    }
}

void TaskExecutorPool::shutdown_forTest() {
    _fixedExecutor->shutdown();
    for (auto&& exec : _executors) {
        exec->shutdown();
    }
}

void TaskExecutorPool::join_forTest() {
    _fixedExecutor->join();
    for (auto&& exec : _executors) {
        exec->join();
    }
}

void TaskExecutorPool::addExecutors(std::vector<std::shared_ptr<TaskExecutor>> executors,
                                    std::shared_ptr<TaskExecutor> fixedExecutor) {
    invariant(_executors.empty());
    invariant(fixedExecutor);
    invariant(!_fixedExecutor);

    _fixedExecutor = std::move(fixedExecutor);
    _executors = std::move(executors);
}

const std::shared_ptr<TaskExecutor>& TaskExecutorPool::getArbitraryExecutor() {
    invariant(!_executors.empty());
    uint64_t idx = (_counter.fetchAndAdd(1) % _executors.size());
    return _executors[idx];
}

const std::shared_ptr<TaskExecutor>& TaskExecutorPool::getFixedExecutor() {
    invariant(_fixedExecutor);
    return _fixedExecutor;
}

void TaskExecutorPool::appendConnectionStats(ConnectionPoolStats* stats) const {
    // Get stats from our fixed executor.
    _fixedExecutor->appendConnectionStats(stats);
    // Get stats from our pooled executors.
    for (auto&& executor : _executors) {
        executor->appendConnectionStats(stats);
    }
}

void TaskExecutorPool::appendNetworkInterfaceStats(BSONObjBuilder& bob,
                                                   bool forServerStatus) const {
    _fixedExecutor->appendNetworkInterfaceStats(bob, forServerStatus);
    for (auto&& executor : _executors) {
        executor->appendNetworkInterfaceStats(bob, forServerStatus);
    }
}

}  // namespace executor
}  // namespace mongo
