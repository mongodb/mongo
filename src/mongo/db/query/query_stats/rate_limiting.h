// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/platform/random.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <mutex>
#include <utility>

namespace mongo {

/*
 * The rate limiting policy is either a sliding window or a sampling based policy. The sliding
 * window policy is used to limit the number of requests over a fixed time period. The sampling
 * based policy is used to limit the number of requests over a fixed percentage.
 */
class RateLimiter {
    using Limit = uint32_t;

    struct WindowBasedPolicy {
        using RequestCount = uint32_t;
        /*
         * Constructor for a rate limiter. Specify the number of requests you want to take place, as
         * well as the time period in milliseconds.
         */
        WindowBasedPolicy(const WindowBasedPolicy&) = delete;
        WindowBasedPolicy& operator=(const WindowBasedPolicy&) = delete;
        WindowBasedPolicy(WindowBasedPolicy&&) = delete;
        WindowBasedPolicy& operator=(WindowBasedPolicy&&) = delete;
        ~WindowBasedPolicy() = default;
        WindowBasedPolicy() {
            ObservableMutexRegistry::get().add("queryStatsRateLimiterWindowBasedPolicyWindowMutex",
                                               _windowMutex);
        };
        /*
         * Getter for the sampling rate.
         */
        inline RequestCount getSamplingRate() const noexcept {
            return _requestLimit.load();
        }

        /*
         * Setter for the sampling rate.
         */
        inline void setSamplingRate(RequestCount samplingRate) noexcept {
            _requestLimit.store(samplingRate);
        }

        /*
         * A method that ensures a more steady rate of requests. Rather than only looking at the
         * current time block, this method simulates a sliding window to estimate how many requests
         * occurred in the last full time period. Like the above, returns whether the request should
         * be handled, and resets the window if enough time has passed.
         */
        bool handle();

    private:
        /*
         * Resets the current window if it has ended. Returns the current time. This must be called
         * in the beginning of each handleRequest...() method.
         */
        Date_t tickWindow();

        /*
         * Clock source used to track time.
         */
        ClockSource* const _clockSource = SystemClockSource::get();

        /*
         * Sampling rate is the bound on the number of requests we want to admit per window.
         */
        Atomic<RequestCount> _requestLimit = 0;

        /*
         * Time period is the window size in ms.
         */
        const Milliseconds _timePeriod = Seconds{1};

        /*
         * Window start.
         */
        Date_t _windowStart;

        /*
         * Count of requests handled in the previous window.
         */
        RequestCount _prevCount = 0;

        /*
         * Count of requests handled in the current window.
         * Atomic to allow a lock-free pre-check in handle() before acquiring _windowMutex.
         */
        Atomic<RequestCount> _currentCount{0};

        /*
         * Mutex used when reading/writing the window.
         */
        ObservableMutex<std::mutex> _windowMutex;
    };

    struct SampleBasedPolicy {
        using SampleRate = uint32_t;
        static constexpr uint32_t kDenominator = 1000;

        /*
         * Constructor for a rate limiter. Specify the percentage of requests you want to take place
         */
        SampleBasedPolicy(const SampleBasedPolicy&) = delete;
        SampleBasedPolicy& operator=(const SampleBasedPolicy&) = delete;
        SampleBasedPolicy(SampleBasedPolicy&&) = delete;
        SampleBasedPolicy& operator=(SampleBasedPolicy&&) = delete;
        ~SampleBasedPolicy() = default;
        SampleBasedPolicy() = default;

        /*
         * Getter for the sampling rate.
         */
        inline SampleRate getSamplingRate() const noexcept {
            return _samplingRate.load();
        }

        /*
         * Setter for the sampling rate.
         */
        inline void setSamplingRate(SampleRate samplingRate) noexcept {
            _samplingRate.store(samplingRate);
        }

        /*
         * Setter for the random seed.
         */
        inline void setRandomSeed(uint64_t randomSeed) noexcept {
            _randomSeed.store(randomSeed);
        }

        /*
         * A method that ensures a steady rate of requests. This method uses a pseudo random
         * number generator to determine whether the request should be handled or not.
         */
        bool handle();

    private:
        Atomic<SampleRate> _samplingRate = 0;
        Atomic<uint64_t> _randomSeed = 0;
    };

public:
    enum PolicyType { kSampleBasedPolicy, kWindowBasedPolicy };

    /*
     * Getter for the sampling rate.
     */
    inline Limit getSamplingRate() const noexcept {
        if (_mode.load() == PolicyType::kWindowBasedPolicy) {
            return _windowPolicy.getSamplingRate();
        } else {
            return _samplePolicy.getSamplingRate();
        }
    }

    /*
     * A method that ensures a more steady rate of requests.
     * See WindowBasedPolicy::handle() & SampleBasedPolicy::handle() for more details.
     */
    bool handle();

    /*
     * Returns the policy type.
     */
    PolicyType getPolicyType() const;

    /**
     * Round a fractional sampling rate to an integer value per thousand. For example, a
     * samplingRate of 0.1 (10%) will be rounded to 100 (per thousand).
     */
    static int roundSampleRateToPerThousand(double samplingRate);

    void configureSampleBased(uint32_t rate, int seed) {
        _samplePolicy.setSamplingRate(rate);
        _samplePolicy.setRandomSeed(seed);
        _mode.store(PolicyType::kSampleBasedPolicy);
    }

    void configureWindowBased(uint32_t rate) {
        _windowPolicy.setSamplingRate(rate);
        _mode.store(PolicyType::kWindowBasedPolicy);
    }

    RateLimiter() = default;
    ~RateLimiter() = default;

private:
    SampleBasedPolicy _samplePolicy;
    WindowBasedPolicy _windowPolicy;
    Atomic<PolicyType> _mode = PolicyType::kWindowBasedPolicy;
};
}  // namespace mongo
