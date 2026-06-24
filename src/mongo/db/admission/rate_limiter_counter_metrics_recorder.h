/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/admission/rate_limiter_metrics_events.h"
#include "mongo/db/admission/rate_limiter_metrics_recorder.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/moving_average.h"

#include <cstdint>

#include <boost/optional.hpp>

namespace mongo::admission {

/**
 * Default rate limiter metrics recorder: maintains in-process counters and a moving average,
 * updated from the recorded events. This is installed on every RateLimiter unless a different
 * recorder is provided in the RateLimiter constructor.
 */
class RateLimiterCounterMetricsRecorder final : public RateLimiterMetricsRecorder {
public:
    void record(const RateLimiterMetricsRecorderEvent& event) noexcept override;

    int64_t addedToQueue() const override;
    int64_t removedFromQueue() const override;
    int64_t interruptedInQueue() const override;
    int64_t rejectedAdmissions() const override;
    int64_t successfulAdmissions() const override;
    int64_t exemptedAdmissions() const override;
    int64_t attemptedAdmissions() const override;
    boost::optional<double> averageTimeQueuedMicros() const override;
    double tokensAcquired() const override;
    double tokensAvailable() const override;
    int64_t currentQueueDepth() const override;

private:
    Counter64 _addedToQueue;
    Counter64 _removedFromQueue;
    Counter64 _interruptedInQueue;
    Counter64 _rejectedAdmissions;
    Counter64 _successfulAdmissions;
    Counter64 _exemptedAdmissions;
    Counter64 _attemptedAdmissions;
    MovingAverage _averageTimeQueuedMicros{0.2};
    Atomic<double> _tokensAcquired;
    Atomic<double> _tokensAvailable;
};

}  // namespace mongo::admission
