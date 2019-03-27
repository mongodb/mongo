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

#pragma once

#include <chrono>
#include <thread>

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * A clock source that reads the Date_t lastNow time and uses a background thread to ensure
 * Date_t::now is called at least every X amount of milliseconds.  If nothing reads the time for a
 * whole granularity, the thread will sleep until it is needed again.
 *
 * Its now() returns Date_t::lastNow(), the passed in clock source is only used to control the
 * sleeps for the background thread.
 */
class BackgroundThreadClockSource final : public ClockSource {
    BackgroundThreadClockSource(const BackgroundThreadClockSource&) = delete;
    BackgroundThreadClockSource& operator=(const BackgroundThreadClockSource&) = delete;

public:
    BackgroundThreadClockSource(std::unique_ptr<ClockSource> clockSource, Milliseconds granularity);
    ~BackgroundThreadClockSource() override;
    Milliseconds getPrecision() override;
    Date_t now() override;
    Status setAlarm(Date_t when, unique_function<void()> action) override;

    size_t timesPausedForTest();

private:
    void _updateClockAndWakeTimerIfNeeded();
    void _updateClock();
    void _startTimerThread();

    const std::unique_ptr<ClockSource> _clockSource;

    // Expected transitions:
    //
    // Starting the clock source
    //   _ -> kTimerPaused
    //
    // Timer thread has woken from its timed sleep
    //   kTimerPaused
    //   kReaderRead  -> kTimerWillPause
    //
    // Reader reads a time and the timer isn't paused
    //   kTimerWillPause -> kReaderHasRead
    //
    // Reader wakes up the timer thread
    //   kTimerPaused -> kReaderHasRead
    enum States : uint8_t {
        kReaderHasRead,
        kTimerWillPause,
        kTimerPaused,
    };
    AtomicWord<uint8_t> _state;

    const Milliseconds _granularity;

    stdx::mutex _mutex;
    stdx::condition_variable _condition;
    bool _inShutdown = false;
    bool _started = false;
    Date_t _lastUpdate;

    // This is used exclusively for tests, to verify when we've actually gone to sleep
    size_t _timesPaused = 0;
    stdx::thread _timer;
};

}  // namespace mongo
