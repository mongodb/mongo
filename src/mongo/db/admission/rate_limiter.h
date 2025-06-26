/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/operation_context.h"
#include "mongo/util/moving_average.h"

namespace mongo::admission {

/**
 * The RateLimiter offers a thin wrapper around the folly::TokenBucket augmented with
 * interruptibility, maximum queue depth, and metrics.
 */
class RateLimiter {
public:
    struct Stats {
        /**
         * addedToQueue is the count of acquireToken calls that involved entering a sleep.
         */
        Counter64 addedToQueue;
        /**
         * removedFromQueue is the count of acquireToken calls that involved waking from a
         * sleep.
         */
        Counter64 removedFromQueue;
        /**
         * interruptedInQueue is the count of acquireToken calls that involved waking from a
         * sleep early due to some interrupt condition.
         */
        Counter64 interruptedInQueue;
        /**
         * rejectedAdmissions is the count of acquireToken calls that would have been
         * queued (due to an unavailability of tokens), but were instead rejected due to there
         * already being too many callers in the queue (threads sleeping in acquireToken).
         */
        Counter64 rejectedAdmissions;
        /**
         * successfulAdmissions is the count of non-error-returning calls to acquireToken. It
         * excludes interrupted and rejected calls.
         */
        Counter64 successfulAdmissions;
        /**
         * exemptedAdmissions is the count of calls to recordExemption. It indicates how often
         * the rate limiter was told to admit immediately.
         */
        Counter64 exemptedAdmissions;
        /**
         * attemptedAdmissions is the count of all calls to acquireToken, regardless of the
         * result.
         */
        Counter64 attemptedAdmissions;
        /**
         * averageTimeQueuedMicros is an exponential moving average of the amount of
         * microseconds that callers spent sleeping in acquireToken, excluding rejected calls
         * and excluding interrupted calls.
         */
        MovingAverage averageTimeQueuedMicros{0.2};
    };

    /**
     * The error code used when the rate limter denies a request to acquire a token (e.g. because
     * the max queue depth is exceeded).
     */
    constexpr static ErrorCodes::Error kRejectedErrorCode = ErrorCodes::RateLimitExceeded;

    RateLimiter(double refreshRatePerSec,
                double burstCapacitySecs,
                int64_t maxQueueDepth,
                std::string name);

    ~RateLimiter();

    /**
     * Acquire a token or block until one becomes available. Returns an error status if
     * the operationContext is interrupted or the maxQueueDepth is exceeded.
     */
    Status acquireToken(OperationContext*);

    /**
     * Attempts to acquire a token without queuing. Returns an error status if the rate limit
     * and the burst size is exceeded.
     */
    Status tryAcquireToken();

    /**
     * Updates metrics for admission granted without having called acquireToken.
     * Use this function in place of acquireToken when admission must be granted immediately.
     * Admission granted in this way does not consume any tokens.
     */
    void recordExemption();

    /**
     * refreshRate is the number of tokens issued per second when rate-limiting has kicked in.
     * Tokens will be issued smoothly, rather than all at once every 1 second. The
     * burstCapacitySecs is the number of seconds worth of unutilized rate limit that can be
     * stored away before rate-limiting kicks in.
     */
    void updateRateParameters(double refreshRatePerSec, double burstCapacitySecs);

    /**
     * The maximum number of requests enqueued waiting for a token. Token requests that come in and
     * will queue past the maxQueueDepth will be rejected with a RateLimiter::kRejectedErrorCode
     * error.
     */
    void setMaxQueueDepth(int64_t maxQueueDepth);

    /** Returns a read-only view of the statistics collected by this rate limiter instance. */
    const Stats& stats() const;

    /** Adds named entries to bob based on this object's stats(). **/
    void appendStats(BSONObjBuilder* bob) const;

    /** Returns the number of tokens available in the underlying token bucket. **/
    double tokensAvailable() const;

    /**
     * Returns the balance of tokens in the bucket, which may be negative if requests have
     * "borrowed" tokens.
     * */
    double tokenBalance() const;

    /** Returns the number of sessions that are sleeping in acquireToken(...). **/
    int64_t queued() const;

private:
    struct RateLimiterPrivate;
    std::unique_ptr<RateLimiterPrivate> _impl;
};
}  // namespace mongo::admission
