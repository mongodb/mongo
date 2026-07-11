// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/clock_source_mock.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/static_immortal.h"

#include <mutex>
#include <new>
#include <queue>
#include <utility>
#include <vector>

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
        std::lock_guard lk(_mutex);
        return _now;
    }

    void advance(Milliseconds ms) {
        std::unique_lock lk(_mutex);
        _now += ms;
        _processAlarms(lk);
    }

    void reset(Date_t newNow) {
        std::unique_lock lk(_mutex);
        _now = newNow;
        _processAlarms(lk);
    }

    void setAlarm(Date_t when, unique_function<void()> action) {
        if (!action)
            return;
        std::unique_lock lk(_mutex);
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

    void _processAlarms(std::unique_lock<std::mutex>& lk) {
        while (!_alarms.empty() && _alarms.top().when <= _now) {
            auto action = _alarms.consumeNextAction();
            lk.unlock();
            ScopeGuard relock([&] { lk.lock(); });
            action();
        }
    }

    mutable std::mutex _mutex;
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
