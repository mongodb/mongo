/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"

#include <cstdint>
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
        WindowBasedPolicy() = default;
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
        AtomicWord<RequestCount> _requestLimit = 0;

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
         */
        RequestCount _currentCount = 0;

        /*
         * Mutex used when reading/writing the window.
         */
        stdx::mutex _windowMutex;
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
        AtomicWord<SampleRate> _samplingRate = 0;
        AtomicWord<uint64_t> _randomSeed = 0;
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
    AtomicWord<PolicyType> _mode = PolicyType::kWindowBasedPolicy;
};
}  // namespace mongo
