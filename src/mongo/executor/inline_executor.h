/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

namespace MONGO_MOD_PUB mongo {
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
    struct MONGO_MOD_FILE_PRIVATE State {
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
}  // namespace MONGO_MOD_PUB mongo
