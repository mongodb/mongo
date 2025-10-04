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

#include "mongo/util/clock_source.h"

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/waitable.h"

#include <memory>
#include <mutex>
#include <utility>

namespace mongo {
stdx::cv_status ClockSource::waitForConditionUntil(stdx::condition_variable& cv,
                                                   BasicLockableAdapter bla,
                                                   Date_t deadline,
                                                   Waitable* waitable) {
    if (_tracksSystemClock) {
        if (deadline == Date_t::max()) {
            Waitable::wait(waitable, this, cv, bla);
            return stdx::cv_status::no_timeout;
        }

        return Waitable::wait_until(waitable, this, cv, bla, deadline.toSystemTimePoint());
    }

    // The rest of this function only runs during testing, when the clock source is virtualized and
    // does not track the system clock.

    auto testNow = now();
    if (deadline <= testNow) {
        return stdx::cv_status::timeout;
    }

    struct AlarmInfo {
        stdx::mutex mutex;

        stdx::condition_variable* cv;
        stdx::cv_status result = stdx::cv_status::no_timeout;
    };
    auto alarmInfo = std::make_shared<AlarmInfo>();
    alarmInfo->cv = &cv;

    setAlarm(deadline, [alarmInfo] {
        // Set an alarm to hit our virtualized deadline
        stdx::lock_guard infoLk(alarmInfo->mutex);
        auto cv = std::exchange(alarmInfo->cv, nullptr);
        if (!cv) {
            return;
        }

        alarmInfo->result = stdx::cv_status::timeout;
        cv->notify_all();
    });

    if (stdx::lock_guard infoLk(alarmInfo->mutex); !alarmInfo->cv) {
        // If setAlarm() ran inline, then we've timed out
        return alarmInfo->result;
    }

    // This is a wait_until because theoretically setAlarm could run out of line before this cv
    // joins the wait list. Then it could completely miss the notification and block until a lucky
    // renotify or spurious wakeup.
    auto systemClockSource = SystemClockSource::get();
    invariant(this != systemClockSource);

    Waitable::wait_until(waitable,
                         systemClockSource,
                         cv,
                         bla,
                         systemClockSource->now() + kMaxTimeoutForArtificialClocks);

    stdx::lock_guard infoLk(alarmInfo->mutex);
    alarmInfo->cv = nullptr;
    return alarmInfo->result;
}

ClockSource::StopWatch::StopWatch() : StopWatch(SystemClockSource::get()) {};

}  // namespace mongo
