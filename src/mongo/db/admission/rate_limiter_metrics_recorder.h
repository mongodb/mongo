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

#include "mongo/db/admission/rate_limiter_metrics_events.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/optional.hpp>

namespace mongo::admission {

/**
 * Abstract base class for rate limiter metrics recorders. It consumes the rate limiter's metric
 * events via record() and exposes the resulting aggregate values through the typed accessors below.
 */
class MONGO_MOD_PUBLIC RateLimiterMetricsRecorder {
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
