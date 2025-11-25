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

#include "mongo/util/concurrency/with_lock.h"
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
