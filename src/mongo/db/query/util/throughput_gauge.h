// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"
#include "mongo/util/represent_as.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <queue>

namespace mongo {

/**
 * This class will help record the ops/second of the last 1 second. It is thread safe.
 */
class ThroughputGauge {
public:
    /**
     * Records an event at a timestamp.
     * Removes all events older than 1 second.
     */
    void recordEvent(Date_t ts) {
        std::lock_guard lk(_mutex);
        _eventTimes.push(ts);
        expireOldEvents(lk, ts);
    }

    /**
     * Returns the number of stored events in the previous second.
     * Removes all events older than 1 second.
     */
    int64_t nEventsInPreviousSecond(Date_t now) {
        std::lock_guard lk(_mutex);
        expireOldEvents(lk, now);
        return representAs<int64_t>(_eventTimes.size())
            .value_or(std::numeric_limits<int64_t>::max());
    }

private:
    void expireOldEvents(const WithLock& lk, Date_t now) {
        while (!_eventTimes.empty() && (now - _eventTimes.top() > Seconds(1))) {
            _eventTimes.pop();
        }
    }

    mutable std::mutex _mutex;
    std::priority_queue<Date_t, std::vector<Date_t>, std::greater<Date_t>> _eventTimes{};
};
}  // namespace mongo
