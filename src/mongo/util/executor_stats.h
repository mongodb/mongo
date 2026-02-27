/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/counter.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/util/duration.h"
#include "mongo/util/histogram.h"
#include "mongo/util/modules.h"
#include "mongo/util/moving_average.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/tick_source.h"

namespace MONGO_MOD_PUB mongo {

/**
 * A utility to collect stats for tasks scheduled on an executor (e.g., the reactor thread).
 *
 * This class expects a tick source to measure the waiting and execution time of tasks. The tick
 * source must always outlive instances of this class.
 *
 * This class collects stats for any task that is wrapped using `wrapTask`. The wrapped task may
 * never outlive the instance of `ExecutorStats`.
 *
 * All public interfaces for this class are thread-safe.
 */
class ExecutorStats {
public:
    explicit ExecutorStats(TickSource*);

    using Task = OutOfLineExecutor::Task;
    Task wrapTask(Task&&);

    void serialize(BSONObjBuilder* bob, bool forServerStatus = false) const;

    const Histogram<Microseconds>& waiting_forTest() const;
    const Histogram<Microseconds>& running_forTest() const;

private:
    Counter64 _scheduled;
    Counter64 _executed;

    MovingAverage _averageWaitTimeMicros{0.2};
    MovingAverage _averageRunTimeMicros{0.2};

    /**
     * The following maintain histograms for tasks scheduled on the executor.
     * `_waiting` keeps track of the waiting time for the tasks pending execution.
     * `_running` keeps track of the execution time of individual tasks after completion.
     * Bucket 0 counts durations `t` with `t >= 0 μs` and `t < 1 μs`.
     * Bucket `i+1` counts durations `t` with `pow(5, i) <= t < pow(5, i+1)`.
     * The last bucket represents any recorded latency that is greater than or
     * equal to `pow(5, 9)` μs, which is 1.953125 seconds.
     */
    Histogram<Microseconds> _waiting;
    Histogram<Microseconds> _running;

    TickSource* const _tickSource;
    logv2::SeveritySuppressor severitySuppressor{
        Seconds{10}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2)};
};

}  // namespace MONGO_MOD_PUB mongo
