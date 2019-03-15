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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/background_thread_clock_source.h"

#include <chrono>
#include <thread>

#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {

BackgroundThreadClockSource::BackgroundThreadClockSource(std::unique_ptr<ClockSource> clockSource,
                                                         Milliseconds granularity)
    : _clockSource(std::move(clockSource)), _state(kTimerPaused), _granularity(granularity) {
    _startTimerThread();
    _tracksSystemClock = true;
}

BackgroundThreadClockSource::~BackgroundThreadClockSource() {
    {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        _inShutdown = true;
        _condition.notify_one();
    }

    _timer.join();
}

Milliseconds BackgroundThreadClockSource::getPrecision() {
    return _granularity;
}

Status BackgroundThreadClockSource::setAlarm(Date_t when, unique_function<void()> action) {
    MONGO_UNREACHABLE;
}

Date_t BackgroundThreadClockSource::now() {
    // Since this is called very frequently by many threads, the common case should not write to
    // shared memory.
    //
    // If we read ReaderHasRead, we have at least the last time from a previous reader, or the
    // background thread.
    if (MONGO_unlikely(_state.load() != kReaderHasRead)) {  // acquire
        _updateClockAndWakeTimerIfNeeded();
    }

    return Date_t::lastNow();
}

void BackgroundThreadClockSource::_updateClock() {
    // We capture the lastUpdate time now to ensure that we sleep for the right target granularity,
    // even if it takes a while for the background thread to wake up.
    _lastUpdate = _clockSource->now();

    // Updates Date_t::lastNow by calling Date_t::now()
    Date_t::now();
}

// This will be called at most once per _granularity per thread. In common cases it will only be
// called by a single thread per _granularity.
void BackgroundThreadClockSource::_updateClockAndWakeTimerIfNeeded() {
    // Try to go from TimerWillPause to ReaderHasRead.
    if (_state.compareAndSwap(kTimerWillPause, kReaderHasRead) != kTimerPaused) {
        // There are three possible states _state could have been in before this cas:
        //
        // kTimerWillPause - In this case, we've transitioned to kReaderHasRead, telling the timer
        //                   it still has recent readers and should continue to loop.  In that case,
        //                   we have no more to do, return.
        //
        // kReaderHasRead - Another thread had already performed the kTimerWillPause ->
        //                  kReaderHasRead transition, we have no work to do, return.
        //
        // kTimerPaused - The timer was paused, so we have to wake it up.  Don't return and attempt
        //                to wake the timer thread.
        //
        // For the first two cases, we can be sure we've acquired an up to date notion of time (from
        // the timer thread or a reader that has woken calling updateClock()), from either
        // succeeding or failing the cas above.
        return;
    }

    // We may be in timer paused
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    // See if we still observe paused, after taking a lock.  This prevents multiple threads from
    // racing to update the clock.
    if (_state.load() != kTimerPaused) {
        // If not, someone else has taken care of it
        return;
    }

    // If we were still in pause, there are a couple of tasks we have to do:
    //
    // 1. update the clock
    // 2. store kReaderHasRead
    // 3. wake the background thread
    //
    // It's important that we do them in that order, so that the background thread sleeps
    // exactly granularity from now, and so that readers that observe kReaderHasRead pick up the
    // updated time.  Failing to keep that order may cause them to observe what may be a very
    // stale read (if the background timer was a sleep for an extended period).
    _updateClock();
    _state.store(kReaderHasRead);  // release

    _condition.notify_one();
}

size_t BackgroundThreadClockSource::timesPausedForTest() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _timesPaused;
}

void BackgroundThreadClockSource::_startTimerThread() {
    // Start the background thread that repeatedly sleeps for the specified duration of milliseconds
    // and wakes up to store the current time.
    _timer = stdx::thread([&]() {
        setThreadName("BackgroundThreadClockSource");
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        _started = true;
        _condition.notify_one();

        while (!_inShutdown) {
            // update the clock every pass
            _updateClock();

            // Always transition to will pause on every run.
            auto old = _state.swap(kTimerWillPause);  // release

            // There are 3 possible states _state could have been in:
            //
            // kTimerWillPause - We slept until the next tick without a reader.  We should pause
            //
            // kReaderHasRead - A reader has read since our last sleep.  We should sleep again
            //
            // kTimerPaused - We were asleep and spuriously woke or we just started (we start in
            //                kTimerPaused)
            //
            // If we do pause our wake up will indicate:
            //   1. That we've had a reader and it's time to tick again
            //   2. That we're in shutdown, where we'll early return from the next condvar wait
            //   3. Experiencing a spurious wake, which may make us tick an extra time, but no more
            //      than once per granularity

            if (old != kReaderHasRead) {
                // Stop running if nothing has read the time since we last updated the time.
                _state.store(kTimerPaused);

                _timesPaused++;

                // We don't care about spurious wake ups here, at worst we'll update the clock an
                // extra time.
                MONGO_IDLE_THREAD_BLOCK;
                _condition.wait(lock);
            }

            MONGO_IDLE_THREAD_BLOCK;
            _clockSource->waitForConditionUntil(
                _condition, lock, _lastUpdate + _granularity, [this] { return _inShutdown; });
        }
    });

    // Wait for the thread to start. This prevents other threads from calling now() until the timer
    // thread is at its first wait() call. While the code would work without this, it makes startup
    // more predictable and therefore easier to test.
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _condition.wait(lock, [this] { return _started; });
}

}  // namespace mongo
