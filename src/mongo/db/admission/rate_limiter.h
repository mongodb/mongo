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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/rate_limiter_counter_metrics_recorder.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/system_tick_source.h"

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] admission {

/**
 * The RateLimiter offers a thin wrapper around the folly::TokenBucket augmented with
 * interruptibility, maximum queue depth, and metrics.
 */
class [[MONGO_MOD_PUBLIC]] RateLimiter {
    class RateLimiterPrivate;

public:
    /**
     * A DeferredToken represents an atomically pre-reserved position in the rate limiter queue. It
     * encapsulates logic for waiting until the reserved position becomes valid or for abandoning
     * the reservation. See acquireToken() for details on how deferred tokens are issued and used.
     *
     * A DeferredToken must be consumed exactly once via get() before it is destroyed, unless
     * recordExemption() is called first to mark the request as not subject to admission control.
     */
    class [[MONGO_MOD_PUBLIC]] DeferredToken {
    public:
        DeferredToken(const DeferredToken&) = delete;
        DeferredToken& operator=(const DeferredToken&) = delete;
        DeferredToken& operator=(DeferredToken&&) = delete;

        // The _impl member serves as the consumed/moved-from sentinel, it is nulled when ownership
        // transfers out (move operation) or when the deferred token is redeemed (get). The default
        // move constructor would copy the pointer without nulling the source, so we define it here
        // to perform the necessary std::exchange.
        DeferredToken(DeferredToken&& other) noexcept
            : _impl(std::exchange(other._impl, nullptr)),
              _numTokens(other._numTokens),
              _timeEnqueued(other._timeEnqueued),
              _napTime(other._napTime) {}

        ~DeferredToken();

        /**
         * Returns true if the token was immediately available when acquireToken() was called.
         * For ready deferred tokens, get() returns without sleeping.
         */
        bool isReady() const {
            return _napTime == Milliseconds{0};
        }

        /**
         * Waits until the pre-reserved token slot becomes valid, or until the opCtx is
         * interrupted. For ready deferred tokens, this method returns immediately.
         *
         * Must be called exactly once, the deferred token is consumed on return.
         */
        Status get(OperationContext* opCtx) &&;

        /**
         * Records that this request is not subject to admission control.
         *
         * This method is valid only for queued (non-ready) deferred tokens. It only records the
         * exemption, token/queue cleanup is covered by the destructor.
         */
        void recordExemption() &&;

    private:
        friend class RateLimiter;

        DeferredToken(RateLimiterPrivate* impl,
                      double numTokens,
                      Milliseconds timeEnqueued,
                      Milliseconds napTime);

        RateLimiterPrivate* _impl{nullptr};
        double _numTokens{1.0};
        Milliseconds _timeEnqueued{0};
        Milliseconds _napTime{0};
    };

    /**
     * The error code used when the rate limter denies a request to acquire a token (e.g. because
     * the max queue depth is exceeded).
     */
    constexpr static ErrorCodes::Error kRejectedErrorCode = ErrorCodes::RateLimitExceeded;

    struct Options {
        TickSource* tickSource{globalSystemTickSource()};
        std::unique_ptr<RateLimiterMetricsRecorder> metricsRecorder{
            std::make_unique<RateLimiterCounterMetricsRecorder>()};
    };

    RateLimiter(double refreshRatePerSec,
                double burstCapacitySecs,
                int64_t maxQueueDepth,
                std::string name,
                TickSource* tickSource = globalSystemTickSource());

    RateLimiter(double refreshRatePerSec,
                double burstCapacitySecs,
                int64_t maxQueueDepth,
                std::string name,
                Options options);

    ~RateLimiter();

    /**
     * Atomically reserves a token position and returns a DeferredToken. The deferred token is
     * either ready (the token was immediately available) or queued (the token was not immediately
     * available and the caller must wait for the slot to become valid).
     *
     * Returns boost::none if no tokens are available and the max queue depth is exceeded.
     */
    boost::optional<DeferredToken> acquireToken(double numTokensToConsume = 1.0);

    /**
     * Convenience method that acquires a token and blocks until it is ready. This is equivalent to
     * calling acquireToken() and then get(opCtx) on the returned deferred token.
     */
    Status acquireToken(OperationContext* opCtx, double numTokensToConsume = 1.0);

    /**
     * Attempts to acquire a token without queuing. Returns false if the rate limit and the burst
     * size is exceeded.
     */
    bool tryAcquireToken(double numTokensToConsume = 1.0);

    /**
     * Returns tokens back to the bucket.
     */
    void returnTokens(double numTokensToReturn);

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
    const RateLimiterMetricsRecorder& stats() const;

    /** Returns the statistics collected by this rate limiter instance. */
    RateLimiterMetricsRecorder& stats();

    /** Adds named entries to bob based on this object's stats(). **/
    void appendStats(BSONObjBuilder* bob) const;

    /** Returns the number of tokens available in the underlying token bucket. **/
    double tokensAvailable() const;

    /**
     * Returns a snapshot of available tokens clamped to [0, INT64_MAX]. This is the value reported
     * to metrics consumers (FTDC/serverStatus and the OTel gauge), which may not handle infinity or
     * negative balances.
     */
    double sampledAvailableTokens() const;

    /**
     * Returns the balance of tokens in the bucket, which may be negative if requests have
     * "borrowed" tokens.
     * */
    double tokenBalance() const;

    /** Returns the number of sessions that are sleeping in acquireToken(...). **/
    int64_t queued() const;

    /** Returns the configured maximum number of sessions that may sleep in acquireToken(...). **/
    int64_t maxQueueDepth() const;

private:
    std::unique_ptr<RateLimiterPrivate> _impl;
};
}  // namespace admission
}  // namespace mongo
