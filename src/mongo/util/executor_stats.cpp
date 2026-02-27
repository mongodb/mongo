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

#include "mongo/util/executor_stats.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"

#include <cmath>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor
namespace mongo {

namespace {

std::vector<Microseconds> partitions() {
    // {1 μs, 5 μs, 25 μs, 125 μs, 625 μs, ...}
    std::vector<Microseconds> bounds(10);
    Microseconds upperBound{1};
    for (Microseconds& bound : bounds) {
        bound = upperBound;
        upperBound *= 5;
    }
    return bounds;
}

}  // namespace

ExecutorStats::ExecutorStats(TickSource* tickSource)
    : _waiting(partitions()), _running(partitions()), _tickSource(tickSource) {}

ExecutorStats::Task ExecutorStats::wrapTask(ExecutorStats::Task&& task) {
    _scheduled.increment(1);
    return [this, task = std::move(task), scheduledAt = _tickSource->getTicks()](Status status) {
        const auto startedAt = _tickSource->getTicks();
        const auto waitTime = _tickSource->ticksTo<Microseconds>(startedAt - scheduledAt);
        _averageWaitTimeMicros.addSample(waitTime.count());
        _waiting.increment(waitTime);
        const Milliseconds waitTimeThreshold{
            serverGlobalParams.slowTaskExecutorWaitTimeProfilingMs.loadRelaxed()};
        if (waitTime > waitTimeThreshold) {
            LOGV2_DEBUG(9757000,
                        severitySuppressor().toInt(),
                        "Task exceeded the slow wait time threshold",
                        "slowWaitTimeThreshold"_attr = waitTimeThreshold,
                        "duration"_attr = waitTime);
        }
        task(std::move(status));

        const auto runTime =
            _tickSource->ticksTo<Microseconds>(_tickSource->getTicks() - startedAt);
        _averageRunTimeMicros.addSample(runTime.count());
        _running.increment(runTime);
        _executed.increment(1);
    };
}

void ExecutorStats::serialize(BSONObjBuilder* bob, bool forServerStatus) const {
    invariant(bob);
    bob->append("scheduled"_sd, _scheduled.get());
    bob->append("executed"_sd, _executed.get());
    bob->append("averageWaitTimeMicros"_sd, _averageWaitTimeMicros.get().value_or(0));
    bob->append("averageRunTimeMicros"_sd, _averageRunTimeMicros.get().value_or(0));
    if (!forServerStatus) {
        appendHistogram(*bob, _waiting, "waitTime");
        appendHistogram(*bob, _running, "runTime");
    }
}

const Histogram<Microseconds>& ExecutorStats::waiting_forTest() const {
    return _waiting;
}

const Histogram<Microseconds>& ExecutorStats::running_forTest() const {
    return _running;
}

}  // namespace mongo
