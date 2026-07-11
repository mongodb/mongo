// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"

#include <mutex>

namespace mongo {
/**
 * An "OutOfLineExecutor" that actually runs on the same thread of execution. This executor is
 * thread-safe, but not scalable: at most one thread will execute tasks at a time, and it may be the
 * thread that added the task.
 *
 * This is meant to simplify testing when it is important to have an executor that can handle tasks
 * being added as a result of the execution of other tasks, but multiple threads are not actually
 * needed.
 */
class [[MONGO_MOD_USE_REPLACEMENT("other OutOfLineExecutors")]] InlineQueuedCountingExecutor
    : public OutOfLineExecutor {
public:
    void schedule(Task task) override {
        {
            std::lock_guard lock(_mutex);
            // Add the task to our queue
            _taskQueue.emplace_back(std::move(task));
            // Make sure that we are not invoking a Task while invoking a Task. Some
            // OutOfLineExecutors do recursively dispatch Tasks, however, they also carefully
            // monitor stack depth. For the purposes of testing, let's serialize our Tasks. One Task
            // runs at a time.
            if (std::exchange(_inSchedule, true)) {
                return;
            }
        }

        // Clear out our queue
        while (true) {
            Task task_to_run;
            {
                std::lock_guard lock(_mutex);
                if (_taskQueue.empty()) {
                    _inSchedule = false;
                    break;
                }
                task_to_run = std::move(_taskQueue.front());
                _taskQueue.pop_front();
            }
            // Relaxed to avoid adding synchronization where there otherwise wouldn't be. That
            // would cause a false negative from TSAN.
            tasksRun.fetch_add(1, std::memory_order_relaxed);
            // Allow more to be added while we run the current task. Note: we want to destroy the
            // current task before checking for more in case the current task's destructor adds more
            // tasks.
            task_to_run(Status::OK());
        }
    }

    static auto make() {
        return std::make_shared<InlineQueuedCountingExecutor>();
    }

    std::atomic<uint32_t> tasksRun{0};  // NOLINT
private:
    std::mutex _mutex;
    // Whether or not a task is currently being run.
    bool _inSchedule = false;
    std::deque<Task> _taskQueue;
};

class InlineRecursiveCountingExecutor final : public OutOfLineExecutor {
public:
    void schedule(Task task) noexcept override {
        // Relaxed to avoid adding synchronization where there otherwise wouldn't be. That would
        // cause a false negative from TSAN.
        tasksRun.fetch_add(1, std::memory_order_relaxed);
        task(Status::OK());
    }

    static auto make() {
        return std::make_shared<InlineRecursiveCountingExecutor>();
    }

    std::atomic<uint32_t> tasksRun{0};  // NOLINT
};

class RejectingExecutor final : public OutOfLineExecutor {
public:
    void schedule(Task task) noexcept override {
        // Relaxed to avoid adding synchronization where there otherwise wouldn't be. That would
        // cause a false negative from TSAN.
        tasksRejected.fetch_add(1, std::memory_order_relaxed);
        task(Status(ErrorCodes::ShutdownInProgress, ""));
    }

    static auto make() {
        return std::make_shared<RejectingExecutor>();
    }

    std::atomic<uint32_t> tasksRejected{0};  // NOLINT
};

}  // namespace mongo
