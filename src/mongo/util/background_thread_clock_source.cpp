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

#include "mongo/platform/basic.h"

#include "mongo/util/background_thread_clock_source.h"

#include <chrono>
#include <thread>

#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/time_support.h"

namespace mongo {

BackgroundThreadClockSource::BackgroundThreadClockSource(std::unique_ptr<ClockSource> clockSource,
                                                         Milliseconds granularity)
    : _clockSource(std::move(clockSource)), _granularity(granularity), _shutdownTimer(false) {
    _updateCurrent();
    _startTimerThread();
}

BackgroundThreadClockSource::~BackgroundThreadClockSource() {
    {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        _shutdownTimer = true;
        _condition.notify_one();
    }

    _timer.join();
}

Milliseconds BackgroundThreadClockSource::getPrecision() {
    return _granularity;
}

Date_t BackgroundThreadClockSource::now() {
    return Date_t::fromMillisSinceEpoch(_current.load());
}

void BackgroundThreadClockSource::_startTimerThread() {
    // Start the background thread that repeatedly sleeps for the specified duration of milliseconds
    // and wakes up to store the current time.
    _timer = stdx::thread([&]() {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        while (!_shutdownTimer) {
            if (_condition.wait_for(lock, _granularity.toSystemDuration()) ==
                stdx::cv_status::timeout) {
                _updateCurrent();
            }
        }
    });
}

void BackgroundThreadClockSource::_updateCurrent() {
    _current.store(_clockSource->now().toMillisSinceEpoch());
}

}  // namespace mongo
