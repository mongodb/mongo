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

#include "mongo/util/clock_source.h"
#include "mongo/util/system_clock_source.h"

namespace mongo {

/**
 * Rate limiting is used to put a bound on the number of requests to a certain resource over a fixed
 * time window. This implementation is approximate in the sense that it may permit the bound to
 * exceeded. The bound is approximate as a trade off to reduce contention on internal resources.
 */
class RateLimiting {
    using RequestCount = uint32_t;

public:
    /*
     * Constructor for a rate limiter. Specify the number of requests you want to take place, as
     * well as the time period in milliseconds.
     */
    RateLimiting(RequestCount samplingRate, Milliseconds timePeriod = Seconds{1});

    /*
     * Getter for the sampling rate.
     */
    RequestCount getSamplingRate() {
        return _samplingRate.load();
    }

    /*
     * Setter for the sampling rate.
     */
    void setSamplingRate(RequestCount samplingRate) {
        _samplingRate.store(samplingRate);
    }

    /*
     * A simple method for rate limiting. Returns false if we have reached the request limit for the
     * current time window; otherwise, returns true and adds the request to the count for the
     * current window. If we have passed the end of the previous window, the slate is wiped clean.
     */
    bool handleRequestFixedWindow();

    /*
     * A method that ensures a more steady rate of requests. Rather than only looking at the current
     * time block, this method simulates a sliding window to estimate how many requests occurred in
     * the last full time period. Like the above, returns whether the request should be handled, and
     * resets the window if enough time has passed.
     */
    bool handleRequestSlidingWindow();

private:
    /*
     * Resets the current window if it has ended. Returns the current time. This must be called in
     * the beginning of each handleRequest...() method.
     */
    Date_t tickWindow();

    /*
     * Clock source used to track time.
     */
    ClockSource* const _clockSource;

    /*
     * Sampling rate is the bound on the number of requests we want to admit per window.
     */
    AtomicWord<RequestCount> _samplingRate;

    /*
     * Time period is the window size in ms.
     */
    const Milliseconds _timePeriod;

    /*
     * Window start.
     */
    Date_t _windowStart;

    /*
     * Count of requests handled in the previous window.
     */
    RequestCount _prevCount;

    /*
     * Count of requests handled in the current window.
     */
    RequestCount _currentCount;

    /*
     * Mutex used when reading/writing the window.
     */
    stdx::recursive_mutex _windowMutex;
};
}  // namespace mongo
