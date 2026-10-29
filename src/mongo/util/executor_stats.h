// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
    logv2::SeveritySuppressor _severitySuppressorSlowWait{
        Seconds{10}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2)};
    logv2::SeveritySuppressor _severitySuppressorSlowRun{
        Seconds{10}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2)};
};

}  // namespace mongo
