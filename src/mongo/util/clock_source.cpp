/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/stdx/thread.h"
#include "mongo/util/clock_source.h"

namespace mongo {
stdx::cv_status ClockSource::waitForConditionUntil(stdx::condition_variable& cv,
                                                   stdx::unique_lock<stdx::mutex>& m,
                                                   Date_t deadline) {
    if (_tracksSystemClock) {
        return cv.wait_until(m, deadline.toSystemTimePoint());
    }

    // The rest of this function only runs during testing, when the clock source is virtualized and
    // does not track the system clock.

    if (deadline <= now()) {
        return stdx::cv_status::timeout;
    }

    struct AlarmInfo {
        stdx::mutex controlMutex;
        stdx::mutex* waitMutex;
        stdx::condition_variable* waitCV;
        stdx::cv_status cvWaitResult = stdx::cv_status::no_timeout;
    };
    auto alarmInfo = std::make_shared<AlarmInfo>();
    alarmInfo->waitCV = &cv;
    alarmInfo->waitMutex = m.mutex();
    const auto waiterThreadId = stdx::this_thread::get_id();
    bool invokedAlarmInline = false;
    invariantOK(setAlarm(deadline, [alarmInfo, waiterThreadId, &invokedAlarmInline] {
        stdx::lock_guard<stdx::mutex> controlLk(alarmInfo->controlMutex);
        alarmInfo->cvWaitResult = stdx::cv_status::timeout;
        if (!alarmInfo->waitMutex) {
            return;
        }
        if (stdx::this_thread::get_id() == waiterThreadId) {
            // In NetworkInterfaceMock, setAlarm may invoke its callback immediately if the deadline
            // has expired, so we detect that case and avoid self-deadlock by returning early, here.
            // It is safe to set invokedAlarmInline without synchronization in this case, because it
            // is exactly the case where the same thread is writing and consulting the value.
            invokedAlarmInline = true;
            return;
        }
        stdx::lock_guard<stdx::mutex> waitLk(*alarmInfo->waitMutex);
        alarmInfo->waitCV->notify_all();
    }));
    if (!invokedAlarmInline) {
        cv.wait(m);
    }
    m.unlock();
    stdx::lock_guard<stdx::mutex> controlLk(alarmInfo->controlMutex);
    m.lock();
    alarmInfo->waitMutex = nullptr;
    alarmInfo->waitCV = nullptr;
    return alarmInfo->cvWaitResult;
}
}  // namespace mongo
