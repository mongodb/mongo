// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
