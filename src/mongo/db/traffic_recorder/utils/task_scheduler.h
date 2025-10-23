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

#pragma once

#include "mongo/util/functional.h"
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
