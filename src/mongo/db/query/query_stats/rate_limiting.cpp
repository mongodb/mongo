// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/rate_limiting.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/util/clock_source.h"

#include <climits>
#include <mutex>
namespace mongo {

Date_t RateLimiter::WindowBasedPolicy::tickWindow() {
    Date_t currentTime = _clockSource->now();

    // Elapsed time since window start exceeds the time period. Start a new window.
    if (currentTime - _windowStart > _timePeriod) {
        _windowStart = currentTime;
        _prevCount = _currentCount.loadRelaxed();
        _currentCount.storeRelaxed(0);
    }
    return currentTime;
}

bool RateLimiter::WindowBasedPolicy::handle() {
    // Fast path: if the current window count already meets or exceeds the limit, reject without
    // acquiring the mutex or calling the clock. At high throughput with a low rate limit (e.g.,
    // 256 req/sec vs ~400K req/sec in ycsb), this eliminates ~99.94% of mutex acquisitions.
    // A false rejection at a window boundary (when _currentCount was just reset to 0 by another
    // thread) is harmless — query stats is observability data.
    if (_currentCount.loadRelaxed() >= _requestLimit.loadRelaxed()) {
        return false;
    }

    std::unique_lock windowLock{_windowMutex};

    Date_t currentTime = tickWindow();
    auto windowStart = _windowStart;
    auto prevCount = _prevCount;

    // Sliding window is implemented over fixed size time periods/blocks as follows. Instead of
    // making the decision to limit the rate using only the current time period, we look to the rate
    // of the previous period to predicate the rate of the current. This smooths the "sampling" of
    // the events by predicting a constant rate and limiting accordingly.

    // Percentage of time remaining in current window.
    double percentRemainingOfCurrentWindow =
        ((double)(_timePeriod.count() - (currentTime - windowStart).count())) / _timePeriod.count();
    // Estimate the number of requests remaining in the current period. We assume the requests in
    // the previous time block occurred at a constant rate. We multiply the total number of requests
    // in the previous period by the percentage of time remaining in the current period.
    double estimatedRemaining = prevCount * percentRemainingOfCurrentWindow;
    // Add this estimate to the requests we know have taken place within the current time block.
    double estimatedCount = _currentCount.loadRelaxed() + estimatedRemaining;

    if (estimatedCount < _requestLimit.loadRelaxed()) {
        // Relaxed increment is sufficient: the mutex prevents concurrent writers, and
        // the lock-free fast path above tolerates briefly stale values (false rejections
        // are harmless for observability data).
        _currentCount.fetchAndAddRelaxed(1);
        return true;
    }
    return false;
}

bool RateLimiter::SampleBasedPolicy::handle(SampleRate rate) {
    thread_local PseudoRandom prng{_randomSeed.load()};
    return prng.nextUInt32(kDenominator) < rate;
}

bool RateLimiter::handle(double samplingRate) {
    const auto rate = roundSampleRateToPerThousand(samplingRate);
    if (rate <= 0) {
        return false;
    }
    if (rate >= 1000) {
        return true;
    }
    return _samplePolicy.handle(static_cast<SampleBasedPolicy::SampleRate>(rate));
}

bool RateLimiter::handle() {
    if (_mode.load() == RateLimiter::PolicyType::kWindowBasedPolicy) {
        return _windowPolicy.handle();
    } else {
        return _samplePolicy.handle(_samplePolicy.getSamplingRate());
    }
}

CollectDecision RateLimiter::handle(OperationContext* opCtx) {
    if (const auto* samplingRateKnob = _samplingRateKnob.load()) {
        // A per-query sampling-rate override (via query settings) takes precedence over the
        // cluster-wide policy; the decision is made at the overridden rate. Query stats
        // collection remains globally enabled, so a rejected override counts as rate limited.
        const auto& knobConfig = QueryKnobConfiguration::get(opCtx);
        if (knobConfig.getKnobSource(samplingRateKnob->id) == KnobSource::kQuerySettings) {
            return handle(knobConfig.get(*samplingRateKnob)) ? CollectDecision::kCollected
                                                             : CollectDecision::kRateLimited;
        }
    }

    // Cannot collect queryStats if sampling rate is not greater than 0. Note that we do not
    // increment queryStatsRateLimitedRequestsMetric here since queryStats is entirely disabled.
    const auto samplingRate = getSamplingRate();
    if (samplingRate <= 0) {
        return CollectDecision::kDisabled;
    }

    // Check if rate limiting allows us to collect queryStats for this request.
    if (samplingRate < INT_MAX && !handle()) {
        return CollectDecision::kRateLimited;
    }
    return CollectDecision::kCollected;
}

RateLimiter::PolicyType RateLimiter::getPolicyType() const {
    return _mode.load();
}

int RateLimiter::roundSampleRateToPerThousand(double samplingRate) {
    // Round up to ensure that any nonzero sample rate is not rounded down to zero.
    return static_cast<int>(ceil(samplingRate * SampleBasedPolicy::kDenominator));
}

}  // namespace mongo
