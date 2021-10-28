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
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/out_of_line_executor.h"

namespace mongo {

/**
 * A utility to collect stats for tasks scheduled on an executor (e.g., the reactor thread).
 *
 * This class expects a clock source to measure the waiting and execution time of tasks. The clock
 * source must always outlive instances of this class.
 *
 * This class collects stats for any task that is wrapped using `wrapTask`. The wrapped task may
 * never outlive the instance of `ExecutorStats`.
 *
 * All public interfaces for this class are thread-safe.
 */
class ExecutorStats {
public:
    explicit ExecutorStats(ClockSource* clkSource) : _clkSource(clkSource) {}

    using Task = OutOfLineExecutor::Task;
    Task wrapTask(Task&&);

    void serialize(BSONObjBuilder* bob) const;

    static constexpr auto kNumBuckets = 22;

private:
    Counter64 _scheduled;
    Counter64 _executed;

    /**
     * The following maintain histograms for tasks scheduled on the executor:
     * `_waitingBuckets` represents the waiting time for the tasks pending execution.
     * `_runningBuckets` keeps track of execution time of individual tasks after completion.
     * Buckets 0 represents any recorded latency less than 1 milliseconds.
     * Buckets 1 to 20 represent the 1-999 ms range.
     * The last bucket represents any recorded latency that exceeds one second.
     */
    Counter64 _waitingBuckets[kNumBuckets];
    Counter64 _runningBuckets[kNumBuckets];

    ClockSource* const _clkSource;
};

}  // namespace mongo
