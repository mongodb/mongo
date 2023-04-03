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

#include "rate_limiting.h"

namespace mongo {
RateLimiting::RateLimiting(RequestCount samplingRate, Milliseconds timePeriod)
    : _clockSource(SystemClockSource::get()),
      _samplingRate(samplingRate),
      _timePeriod(timePeriod),
      _windowStart(_clockSource->now()),
      _prevCount(0),
      _currentCount(0) {}

Date_t RateLimiting::tickWindow() {
    Date_t currentTime = _clockSource->now();

    // Elapsed time since window start exceeds the time period. Start a new window.
    if (currentTime - _windowStart > _timePeriod) {
        _windowStart = currentTime;
        _prevCount = _currentCount;
        _currentCount = 0;
    }
    return currentTime;
}

bool RateLimiting::handleRequestFixedWindow() {
    stdx::unique_lock windowLock{_windowMutex};
    tickWindow();

    if (_currentCount < _samplingRate.load()) {
        _currentCount += 1;
        return true;
    }
    return false;
}

bool RateLimiting::handleRequestSlidingWindow() {
    stdx::unique_lock windowLock{_windowMutex};

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
    double estimatedCount = _currentCount + estimatedRemaining;

    if (estimatedCount < _samplingRate.load()) {
        _currentCount += 1;
        return true;
    }
    return false;
}
}  // namespace mongo
