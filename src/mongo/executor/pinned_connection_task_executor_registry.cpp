// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/pinned_connection_task_executor_registry.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"

#include <list>
#include <set>

namespace mongo::executor {

struct PinnedConnectionTaskExecutorRegistry {
    std::list<ExecutorPair> entries;

    // Underlying executors whose shutdown has begun. Registering a new PinnedConnectionTaskExecutor
    // for one of these is refused so that a PCTE cannot be created after shutdownPinnedExecutors()
    // has drained the registry (which would let it outlive the underlying executor).
    std::set<const TaskExecutor*> closingUnderlying;
};

const auto getRegistry =
    ServiceContext::declareDecoration<synchronized_value<PinnedConnectionTaskExecutorRegistry>>();

PinnedExecutorRegistryToken::PinnedExecutorRegistryToken(ServiceContext* svc,
                                                         std::shared_ptr<TaskExecutor> pinned,
                                                         std::shared_ptr<TaskExecutor> underlying)
    : _svc(svc) {
    auto reg = getRegistry(_svc).synchronize();

    // Refuse to register against an underlying executor whose shutdown has begun. Together with
    // shutdownPinnedExecutors() marking 'closingUnderlying' under this same lock before it scans,
    // this makes admission linearizable: if we win the lock our entry is visible to that scan,
    // otherwise we are rejected here. Without this, a request that obtained the executor just
    // before shutdown could register its PCTE after the drain and outlive the executor.
    uassert(
        ErrorCodes::ShutdownInProgress,
        "Cannot register a PinnedConnectionTaskExecutor after its underlying executor has begun "
        "shutting down",
        !reg->closingUnderlying.contains(underlying.get()));

    // Insert the new ExecutorPair and hold the returned iterator for O(1) removal.
    _it = reg->entries.emplace(reg->entries.end(),
                               ExecutorPair{std::move(pinned), std::move(underlying)});
}

PinnedExecutorRegistryToken::~PinnedExecutorRegistryToken() {
    // Erase this token's ExecutorPair using the iterator held at construction time. Note that we
    // must hold the registry lock: a concurrent shutdownPinnedExecutors() may be iterating it.
    auto reg = getRegistry(_svc).synchronize();
    reg->entries.erase(_it);
}

void shutdownPinnedExecutors(ServiceContext* svc, const std::shared_ptr<TaskExecutor>& underlying) {
    std::list<std::shared_ptr<TaskExecutor>> toShutdown;
    // Check the registry for all ExecutorPairs that contain 'underlying' and move all the
    // corresponding PCTEs to 'toShutdown'. Hold the registry lock for the whole scan: a concurrent
    // PinnedExecutorRegistryToken destruction may erase entries. The pinned shared_ptrs are copied
    // out so that we can shut them down without the lock, which also keeps them alive for the
    // duration of shutdown.
    {
        auto reg = getRegistry(svc).synchronize();
        // Mark this underlying executor as closing before scanning, so that any token construction
        // that loses this lock race is rejected rather than registering a PCTE we would miss.
        // 'closingUnderlying' is never pruned: shutdownPinnedExecutors() is only called while the
        // process (or the disagg replication executor) is shutting down. The marker is monotonic --
        // once a PCTE is closing it is never reopened -- so there is nothing to clean up.
        reg->closingUnderlying.insert(underlying.get());
        for (auto& entry : reg->entries) {
            if (entry.underlying.lock() != underlying) {
                continue;
            }
            // The token erases its entry asynchronously, so the pinned weak_ptr can already be
            // expired here even though the underlying executor is still alive. Skip those.
            if (auto pinned = entry.pinned.lock()) {
                toShutdown.emplace_back(std::move(pinned));
            }
        }
    }

    for (auto& pinned : toShutdown) {
        pinned->shutdown();
        pinned->join();
    }
}
}  // namespace mongo::executor
