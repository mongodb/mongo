/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/base/disallow_copying.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * A clock source that uses a periodic timer to build a low-resolution, fast-to-read clock.
 * Essentially uses a background thread that repeatedly sleeps for X amount of milliseconds
 * and wakes up to store the current time. If nothing reads the time for a whole granularity, the
 * thread will sleep until it is needed again.
 */
class BackgroundThreadClockSource final : public ClockSource {
    MONGO_DISALLOW_COPYING(BackgroundThreadClockSource);

public:
    BackgroundThreadClockSource(std::unique_ptr<ClockSource> clockSource, Milliseconds granularity);
    ~BackgroundThreadClockSource() override;
    Milliseconds getPrecision() override;
    Date_t now() override;
    Status setAlarm(Date_t when, stdx::function<void()> action) override;

    /**
     * Doesn't count as a call to now() for determining whether this ClockSource is idle.
     *
     * Unlike now(), returns Date_t() if the thread is currently paused.
     */
    Date_t peekNowForTest() const {
        return Date_t::fromMillisSinceEpoch(_current.load());
    }

private:
    Date_t _slowNow();
    void _startTimerThread();
    int64_t _updateCurrent_inlock();

    const std::unique_ptr<ClockSource> _clockSource;
    AtomicInt64 _current{0};           // 0 if _timer is paused due to idleness.
    AtomicBool _timerWillPause{true};  // If true when _timer wakes up, it will pause.

    const Milliseconds _granularity;

    stdx::mutex _mutex;
    stdx::condition_variable _condition;
    bool _inShutdown = false;
    bool _started = false;
    stdx::thread _timer;
};

}  // namespace mongo
