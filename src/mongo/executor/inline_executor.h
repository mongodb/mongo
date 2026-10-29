// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/baton.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/producer_consumer_queue.h"

#include <functional>
#include <memory>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace executor {

/**
 * An RAII type to run tasks inline. Multiple threads may schedule tasks on this executor, but only
 * one thread can run the scheduled tasks. This executor does not own any execution resources (e.g.,
 * threads), so the user must call its `run` method to process scheduled tasks.
 *
 * The executor is marked non-copyable to make sure there's only one runner/consumer. At
 * destruction, it drains all scheduled tasks with a non-okay status and rejects new work.
 *
 * This is an alternative to `Baton` when an `OperationContext` is not available or a custom
 * predicate should be used to interrupt running scheduled tasks. More details and examples on using
 * `Batons` is available here: https://github.com/mongodb/mongo/blob/master/docs/baton.md.
 */
class InlineExecutor {
public:
    /**
     * Maintains the shared state between producers and the consumer.
     */
    struct [[MONGO_MOD_FILE_PRIVATE]] State {
        MultiProducerSingleConsumerQueue<OutOfLineExecutor::Task> tasks;
    };

    InlineExecutor();
    ~InlineExecutor();

    InlineExecutor(const InlineExecutor&) = delete;

    /*
     * Returns an executor that can schedule tasks on this `InlineExecutor`. This executor may
     * (safely) outlive its corresponding `InlineExecutor`, but will run tasks with
     * `ErrorCodes::ShutdownInProgress` once the destructor for `InlineExecutor` runs.
     * This method may be called by multiple threads. The returned `OutOfLineExecutor` may also be
     * safely accessed by multipe threads to schedule jobs concurrently.
     */
    std::shared_ptr<OutOfLineExecutor> getExecutor() {
        return _executor;
    }

    /*
     * Blocking call that runs scheduled tasks inline until the `predicate` returns `true`.
     * May throw if interrupted, or if a scheduled task throws.
     * May only be called from the thread that owns this `InlineExecutor`.
     */
    void run(std::function<bool()> predicate,
             Interruptible* interruptible = Interruptible::notInterruptible());

    /*
     * A wrapper to make this executor compatible with our `AsyncTry` APIs.
     */
    class SleepableExecutor : public OutOfLineExecutor {
    public:
        void schedule(OutOfLineExecutor::Task) override = 0;
        virtual ExecutorFuture<void> sleepFor(Milliseconds, const CancellationToken&) = 0;
    };

    /*
     * Makes a `SleepableExecutor` that may schedule tasks on this `InlineExecutor`, or utilize the
     * provided `baton` (if possible) or `executor` for delayed scheduling. Either `executor` or
     * `baton` must contain a non-null value, but `baton` is preferred when both provided.
     */
    std::shared_ptr<SleepableExecutor> getSleepableExecutor(
        const std::shared_ptr<TaskExecutor>& executor,
        const std::shared_ptr<Baton>& baton = nullptr);

private:
    std::shared_ptr<State> _state;
    std::shared_ptr<OutOfLineExecutor> _executor;
};

}  // namespace executor
}  // namespace mongo
