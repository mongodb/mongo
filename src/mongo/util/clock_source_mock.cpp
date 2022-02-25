/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <queue>
#include <utility>
#include <vector>

#include "mongo/platform/basic.h"

#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/static_immortal.h"

namespace mongo {
namespace {
/**
 * This is a synchronized global mocked ClockSource.
 *
 * For ease of use, this is the underlying source behind *every* clock source.
 **/
class ClockSourceMockImpl {
public:
    Date_t now() const {
        stdx::lock_guard lk(_mutex);
        return _now;
    }

    void advance(Milliseconds ms) {
        stdx::unique_lock lk(_mutex);
        _now += ms;
        _processAlarms(lk);
    }

    void reset(Date_t newNow) {
        stdx::unique_lock lk(_mutex);
        _now = newNow;
        _processAlarms(lk);
    }

    void setAlarm(Date_t when, unique_function<void()> action) {
        if (!action)
            return;
        stdx::unique_lock lk(_mutex);
        _alarms.push({when, std::move(action)});
        _processAlarms(lk);
    }

private:
    struct Alarm {
        Date_t when;
        unique_function<void()> action;
    };

    // By `when`, descending, to create a min-heap.
    struct AlarmQueueOrder {
        bool operator()(const Alarm& a, const Alarm& b) const {
            return a.when > b.when;
        }
    };

    class AlarmQueue : public std::priority_queue<Alarm, std::vector<Alarm>, AlarmQueueOrder> {
    public:
        unique_function<void()> consumeNextAction() {
            invariant(!c.empty(), "Alarm queue cannot be empty");
            // Ok because action doesn't affect heap order.
            auto action = std::move(c.front().action);
            pop();
            return action;
        }
    };

    void _processAlarms(stdx::unique_lock<stdx::mutex>& lk) {
        while (!_alarms.empty() && _alarms.top().when <= _now) {
            auto action = _alarms.consumeNextAction();
            lk.unlock();
            ScopeGuard relock([&] { lk.lock(); });
            action();
        }
    }

    mutable stdx::mutex _mutex;  // NOLINT
    Date_t _now = ClockSourceMock::kInitialNow;
    AlarmQueue _alarms;
};

ClockSourceMockImpl* getGlobalClockSourceMock() {
    static StaticImmortal<ClockSourceMockImpl> clkSource;
    return &*clkSource;
}

}  // namespace

Milliseconds ClockSourceMock::getPrecision() {
    return Milliseconds(1);
}

Date_t ClockSourceMock::now() {
    return getGlobalClockSourceMock()->now();
}

void ClockSourceMock::advance(Milliseconds ms) {
    getGlobalClockSourceMock()->advance(ms);
}

void ClockSourceMock::reset(Date_t newNow) {
    getGlobalClockSourceMock()->reset(newNow);
}

void ClockSourceMock::setAlarm(Date_t when, unique_function<void()> action) {
    getGlobalClockSourceMock()->setAlarm(when, std::move(action));
}

}  // namespace mongo
