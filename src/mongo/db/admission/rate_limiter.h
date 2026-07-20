// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

    /**
     * Variant used to specify ownership. Using a unique_ptr indicates that the RateLimiter will
     * own the RateLimiterMetricsRecorder, and using a raw pointer indicates that it's owned
     * elsewhere. If using a raw pointer, you must ensure that the RateLimiterMetricsRecorder
     * outlives the RateLimiter. In most cases, the RateLimiter should own the
     * RateLimiterMetricsRecorder.
     */
    using MetricsRecorderType =
        std::variant<std::unique_ptr<RateLimiterMetricsRecorder>, RateLimiterMetricsRecorder*>;

    struct Options {
        TickSource* tickSource{globalSystemTickSource()};
        MetricsRecorderType metricsRecorder{std::make_unique<RateLimiterCounterMetricsRecorder>()};
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
     * Reconciles a post-hoc cost by draining numTokens from the bucket without queuing or blocking.
     * The balance may go negative (borrow), delaying subsequent acquisitions. Unlike
     * acquireToken/tryAcquireToken this does not record an admission, since the operation being
     * charged was already admitted; it only adjusts the bucket balance.
     */
    void reconcileTokens(double numTokens);

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

    /**
     * Returns the number of tokens issued per second by the underlying token bucket (its refresh
     * rate). This is the effective rate currently configured on the bucket, so callers can report
     * the source-of-truth rate rather than tracking it separately.
     */
    double refreshRate() const;

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
