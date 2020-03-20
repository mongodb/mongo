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

#include "mongo/util/out_of_line_executor.h"

namespace mongo {
/**
 * An "OutOfLineExecutor" that actually runs on the same thread of execution
 */
class InlineCountingExecutor : public OutOfLineExecutor {
public:
    void schedule(Task task) override {
        // Add the task to our queue
        taskQueue.emplace_back(std::move(task));

        // Make sure that we are not invocing a Task while invocing a Task. Some OutOfLineExecutors
        // do recursively dispatch Tasks, however, they also carefully monitor stack depth. For the
        // purposes of testing, let's serialize our Tasks. One Task runs at a time.
        if (std::exchange(inSchedule, true)) {
            return;
        }

        ON_BLOCK_EXIT([this] {
            // Admit we're not working on the queue anymore
            inSchedule = false;
        });

        // Clear out our queue
        while (!taskQueue.empty()) {
            auto task = std::move(taskQueue.front());

            // Relaxed to avoid adding synchronization where there otherwise wouldn't be. That would
            // cause a false negative from TSAN.
            tasksRun.fetch_add(1, std::memory_order_relaxed);
            task(Status::OK());
            taskQueue.pop_front();
        }
    }

    static auto make() {
        return std::make_shared<InlineCountingExecutor>();
    }

    bool inSchedule;

    std::deque<Task> taskQueue;
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
