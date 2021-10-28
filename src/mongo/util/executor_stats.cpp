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

#include <fmt/format.h>
#include <string>

#include "mongo/util/executor_stats.h"

#include "mongo/base/status.h"

namespace mongo {

namespace {

constexpr auto kMillisPerBucket = 50;

void recordDuration(Counter64* buckets, Milliseconds duration) {
    size_t index;
    if (duration < Milliseconds(1)) {
        index = 0;
    } else if (duration >= Seconds(1)) {
        index = ExecutorStats::kNumBuckets - 1;
    } else {
        // Each bucket covers a 50 milliseconds window (e.g., [1, 50), [50, 100) and so on).
        // That's why the duration (in milliseconds) is divided by 50 to compute the index.
        index = 1 + durationCount<Milliseconds>(duration) / kMillisPerBucket;
    }
    buckets[index].increment();
}

void serializeBuckets(const Counter64* buckets, BSONObjBuilder bob) {
    auto makeTag = [](size_t i) -> std::string {
        if (i == 0)
            return "0-999us";
        if (i == ExecutorStats::kNumBuckets - 1)
            return "1000ms+";

        const auto lb = i > 1 ? (i - 1) * kMillisPerBucket : 1;
        const auto ub = i * kMillisPerBucket - 1;
        return fmt::format("{}-{}ms", lb, ub);
    };

    for (size_t i = 0; i < ExecutorStats::kNumBuckets; i++) {
        bob.append(makeTag(i), buckets[i].get());
    }
}

}  // namespace

ExecutorStats::Task ExecutorStats::wrapTask(ExecutorStats::Task&& task) {
    _scheduled.increment(1);
    return [this, task = std::move(task), scheduledAt = _clkSource->now()](Status status) {
        const auto startedAt = _clkSource->now();
        recordDuration(_waitingBuckets, startedAt - scheduledAt);

        task(std::move(status));

        recordDuration(_runningBuckets, _clkSource->now() - startedAt);
        _executed.increment(1);
    };
}

void ExecutorStats::serialize(BSONObjBuilder* bob) const {
    bob->append("scheduled"_sd, _scheduled.get());
    bob->append("executed"_sd, _executed.get());
    serializeBuckets(_waitingBuckets, bob->subobjStart("waitTime"_sd));
    serializeBuckets(_runningBuckets, bob->subobjStart("runTime"_sd));
}

}  // namespace mongo
