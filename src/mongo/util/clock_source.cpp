// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
        std::mutex mutex;

        stdx::condition_variable* cv;
        stdx::cv_status result = stdx::cv_status::no_timeout;
    };
    auto alarmInfo = std::make_shared<AlarmInfo>();
    alarmInfo->cv = &cv;

    setAlarm(deadline, [alarmInfo] {
        // Set an alarm to hit our virtualized deadline
        std::lock_guard infoLk(alarmInfo->mutex);
        auto cv = std::exchange(alarmInfo->cv, nullptr);
        if (!cv) {
            return;
        }

        alarmInfo->result = stdx::cv_status::timeout;
        cv->notify_all();
    });

    if (std::lock_guard infoLk(alarmInfo->mutex); !alarmInfo->cv) {
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

    std::lock_guard infoLk(alarmInfo->mutex);
    alarmInfo->cv = nullptr;
    return alarmInfo->result;
}

ClockSource::StopWatch::StopWatch() : StopWatch(SystemClockSource::get()) {};

}  // namespace mongo
