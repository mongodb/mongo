// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/executor_stats.h"

#include "mongo/base/status.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"

#include <cmath>
#include <string_view>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor
namespace mongo {
using namespace std::literals::string_view_literals;

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
        const Milliseconds waitTimeThreshold{serverGlobalParams.slowWaitMs.loadRelaxed()};
        if (waitTime >= waitTimeThreshold) {
            LOGV2_DEBUG(9757000,
                        _severitySuppressorSlowWait().toInt(),
                        "Task exceeded the slow wait time threshold",
                        "slowWaitTimeThreshold"_attr = waitTimeThreshold,
                        "duration"_attr = waitTime);
        }
        task(std::move(status));

        const auto runTime =
            _tickSource->ticksTo<Microseconds>(_tickSource->getTicks() - startedAt);
        _averageRunTimeMicros.addSample(runTime.count());
        _running.increment(runTime);
        const Milliseconds runTimeThreshold{serverGlobalParams.slowRunMs.loadRelaxed()};
        if (runTime >= runTimeThreshold) {
            LOGV2_DEBUG(10602200,
                        _severitySuppressorSlowRun().toInt(),
                        "Task exceeded the slow execution time threshold",
                        "slowRunTimeThreshold"_attr = runTimeThreshold,
                        "duration"_attr = runTime);
        }
        _executed.increment(1);
    };
}

void ExecutorStats::serialize(BSONObjBuilder* bob, bool forServerStatus) const {
    invariant(bob);
    bob->append("scheduled"sv, _scheduled.get());
    bob->append("executed"sv, _executed.get());
    bob->append("averageWaitTimeMicros"sv, _averageWaitTimeMicros.get().value_or(0));
    bob->append("averageRunTimeMicros"sv, _averageRunTimeMicros.get().value_or(0));
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
