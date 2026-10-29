// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>

namespace mongo {
class TaskScheduler {
public:
    /**
     * Schedule a piece of work to be run at provided timepoint on a background thread.
     *
     * If the timepoint has elapsed, the work will be performed as soon as possible.
     */
    virtual void runAt(Date_t timepoint, unique_function<void()> work) = 0;

    /**
     * Discard all tasks which have not yet started execution.
     *
     * Further work _can_ be scheduled after this.
     */
    virtual void cancelAll() = 0;

    /**
     * Discard all tasks which have not yet started execution, notify any background thread
     * to stop, and join() the thread.
     *
     * After stop(), no further work should be queued (it will not be executed).
     */
    virtual void stop() = 0;

    virtual ~TaskScheduler() = default;
};

std::unique_ptr<TaskScheduler> makeTaskScheduler(std::string name);
}  // namespace mongo
