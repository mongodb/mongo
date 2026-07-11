// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

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
    Atomic<uint8_t> _state;

    const Milliseconds _granularity;

    std::mutex _mutex;
    stdx::condition_variable _condition;
    bool _inShutdown = false;
    bool _started = false;
    Date_t _lastUpdate;

    // This is used exclusively for tests, to verify when we've actually gone to sleep
    size_t _timesPaused = 0;
    stdx::thread _timer;
};

}  // namespace mongo
