// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/future_util.h"

#include <boost/move/utility_core.hpp>

namespace mongo {

ExecutorFuture<void> sleepUntil(std::shared_ptr<executor::TaskExecutor> executor,
                                const Date_t& date) {
    auto [promise, future] = makePromiseFuture<void>();
    auto taskCompletionPromise = std::make_shared<Promise<void>>(std::move(promise));

    auto scheduledWorkHandle = executor->scheduleWorkAt(
        date, [taskCompletionPromise](const executor::TaskExecutor::CallbackArgs& args) mutable {
            if (args.status.isOK()) {
                taskCompletionPromise->emplaceValue();
            } else {
                taskCompletionPromise->setError(args.status);
            }
        });

    if (!scheduledWorkHandle.isOK()) {
        taskCompletionPromise->setError(scheduledWorkHandle.getStatus());
    }
    return std::move(future).thenRunOn(executor);
}

ExecutorFuture<void> sleepFor(std::shared_ptr<executor::TaskExecutor> executor,
                              Milliseconds duration) {
    return sleepUntil(executor, executor->now() + duration);
}

}  // namespace mongo
