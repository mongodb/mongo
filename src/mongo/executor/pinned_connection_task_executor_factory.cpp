// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/pinned_connection_task_executor_factory.h"

#include "mongo/executor/pinned_connection_task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

namespace mongo {
namespace executor {

std::shared_ptr<TaskExecutor> makePinnedConnectionTaskExecutor(std::shared_ptr<TaskExecutor> exec,
                                                               NetworkInterface* net) {
    return PinnedConnectionTaskExecutor::create(std::move(exec), net);
}

std::shared_ptr<TaskExecutor> makePinnedConnectionTaskExecutor(std::shared_ptr<TaskExecutor> exec) {
    auto tpte = dynamic_cast<ThreadPoolTaskExecutor*>(exec.get());
    invariant(tpte,
              "Connection-pinning task executors can only be constructed from "
              "ThreadPoolTaskExecutor unless an explicit NetworkInterface is provided.");
    return makePinnedConnectionTaskExecutor(std::move(exec), tpte->getNetworkInterface().get());
}

}  // namespace executor
}  // namespace mongo
