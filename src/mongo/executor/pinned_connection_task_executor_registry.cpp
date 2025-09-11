/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
