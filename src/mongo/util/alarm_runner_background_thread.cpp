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

#include "mongo/platform/basic.h"

#include "mongo/util/alarm_runner_background_thread.h"

namespace mongo {

void AlarmRunnerBackgroundThread::start() {
    stdx::lock_guard<Latch> lk(_mutex);
    _running = true;
    _thread = stdx::thread(&AlarmRunnerBackgroundThread::_threadRoutine, this);
}

void AlarmRunnerBackgroundThread::shutdown() {
    stdx::unique_lock<Latch> lk(_mutex);
    _running = false;
    lk.unlock();
    _condVar.notify_one();
    _thread.join();

    for (const auto& scheduler : _schedulers) {
        scheduler->clearAllAlarmsAndShutdown();
    }
}

std::vector<AlarmRunnerBackgroundThread::AlarmSchedulerHandle>
AlarmRunnerBackgroundThread::_initializeSchedulers(std::vector<AlarmSchedulerHandle> schedulers) {
    invariant(!schedulers.empty());

    const auto registerHook = [this](Date_t next, const std::shared_ptr<AlarmScheduler>& which) {
        stdx::unique_lock<Latch> lk(_mutex);
        if (next >= _nextAlarm) {
            return;
        }

        _nextAlarm = next;
        _condVar.notify_one();
    };

    const auto clockSource = schedulers.front()->clockSource();
    for (auto& scheduler : schedulers) {
        scheduler->setAlarmRegisterHook(registerHook);
        auto nextAlarm = scheduler->nextAlarm();
        if (nextAlarm < _nextAlarm) {
            _nextAlarm = nextAlarm;
        }
        // The thread routine uses the clock source of the first registered scheduler to wait
        // on its condvar, so all registered schedulers must use the same clock source.
        fassert(51046, scheduler->clockSource() == clockSource);
    }

    return schedulers;
}

void AlarmRunnerBackgroundThread::_threadRoutine() {
    stdx::unique_lock<Latch> lk(_mutex);
    while (_running) {
        const auto clockSource = _schedulers.front()->clockSource();
        const auto now = clockSource->now();

        // Schedulers may try to alter our _nextAlarm/_newAlarm state as they process expired
        // alarms that then reschedule themselves, so to eliminate any lock contention, just
        // unlock the mutex while we process expired alarms.
        lk.unlock();
        for (const auto& scheduler : _schedulers) {
            if (scheduler->nextAlarm() > now) {
                continue;
            }
            scheduler->processExpiredAlarms();
        }

        // Lock the mutex while the sleep duration is computed. This will block schedulers from
        // scheduling new alarms.
        lk.lock();
        auto sleepUntil = Date_t::max();
        for (const auto& scheduler : _schedulers) {
            sleepUntil = std::min(sleepUntil, scheduler->nextAlarm());
        }

        // Update _nextAlarm so that any schedulers that have pending new alarms see the actual
        // time we're about to sleep for, and set _newAlarm to false before going to sleep.
        _nextAlarm = sleepUntil;

        clockSource->waitForConditionUntil(_condVar, lk, _nextAlarm, [&] {
            return (_nextAlarm != sleepUntil || _running == false);
        });
    }
}

}  // namespace mongo
