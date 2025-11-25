/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/stdx/mutex.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"

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
class MONGO_MOD_USE_REPLACEMENT(other OutOfLineExecutors) InlineQueuedCountingExecutor
    : public OutOfLineExecutor {
public:
    void schedule(Task task) override {
        {
            stdx::lock_guard lock(_mutex);
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
                stdx::lock_guard lock(_mutex);
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
    stdx::mutex _mutex;
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
