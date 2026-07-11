// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/admission/rate_limiter_metrics_events.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/optional.hpp>

namespace mongo::admission {

/**
 * Abstract base class for rate limiter metrics recorders. It consumes the rate limiter's metric
 * events via record() and exposes the resulting aggregate values through the typed accessors below.
 */
class [[MONGO_MOD_PUBLIC]] RateLimiterMetricsRecorder {
public:
    using Event = RateLimiterMetricsRecorderEvent;

    virtual ~RateLimiterMetricsRecorder() = default;

    /** Records an event. */
    virtual void record(const Event& event) noexcept = 0;

    /** Count of acquireToken calls that involved entering a sleep. */
    virtual int64_t addedToQueue() const = 0;
    /** Count of acquireToken calls that involved waking from a sleep. */
    virtual int64_t removedFromQueue() const = 0;
    /** Count of acquireToken calls that woke from a sleep early due to an interrupt. */
    virtual int64_t interruptedInQueue() const = 0;
    /**
     * Count of acquireToken calls that would have been queued (due to an unavailability of tokens),
     * but were instead rejected because the queue was already full.
     */
    virtual int64_t rejectedAdmissions() const = 0;
    /**
     * Count of non-error-returning calls to acquireToken, excluding interrupted and rejected calls.
     */
    virtual int64_t successfulAdmissions() const = 0;
    /** Count of calls to recordExemption (requests admitted immediately). */
    virtual int64_t exemptedAdmissions() const = 0;
    /** Count of all calls to acquireToken, regardless of the result. */
    virtual int64_t attemptedAdmissions() const = 0;
    /**
     * Exponential moving average of the microseconds callers spent sleeping in acquireToken,
     * excluding rejected and interrupted calls. Returns boost::none until the first sample.
     */
    virtual boost::optional<double> averageTimeQueuedMicros() const = 0;
    /** Cumulative sum of tokens consumed across all successful acquireToken calls. */
    virtual double tokensAcquired() const = 0;
    /**
     * Most recently recorded count of tokens available in the bucket. This is a point-in-time
     * value, sampled from the token bucket via the TokensAvailable event. Returns 0 until the
     * first sample.
     */
    virtual double tokensAvailable() const = 0;
    /** Current number of callers queued (added to queue minus removed from queue). */
    virtual int64_t currentQueueDepth() const = 0;

protected:
    RateLimiterMetricsRecorder() = default;
    RateLimiterMetricsRecorder(const RateLimiterMetricsRecorder&) = default;
    RateLimiterMetricsRecorder& operator=(const RateLimiterMetricsRecorder&) = default;
    RateLimiterMetricsRecorder(RateLimiterMetricsRecorder&&) noexcept = default;
    RateLimiterMetricsRecorder& operator=(RateLimiterMetricsRecorder&&) noexcept = default;
};

}  // namespace mongo::admission
