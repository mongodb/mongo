// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/executor/network_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] executor {

/**
 * Returns a new TaskExecutor that does all of its RPC execution over the same transport session.
 * The returned executor uses `exec`'s execution resources and acquires the transport session from
 * `net`.
 */
std::shared_ptr<TaskExecutor> makePinnedConnectionTaskExecutor(std::shared_ptr<TaskExecutor> exec,
                                                               NetworkInterface* net);

/**
 * Returns a new TaskExecutor that does all of its RPC execution over the same transport session.
 * The provided executor _must_ be a ThreadPoolTaskExecutor, and its underlying execution and
 * network resources will be used by the returned executor.
 */
std::shared_ptr<TaskExecutor> makePinnedConnectionTaskExecutor(std::shared_ptr<TaskExecutor> exec);

}  // namespace executor
}  // namespace mongo
