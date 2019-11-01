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

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/alarm.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/time_support.h"

namespace mongo {

/*
 * This is a runner for alarm schedulers that waits for and processes alarms in a single
 * background thread.
 */
class AlarmRunnerBackgroundThread {
public:
    using AlarmSchedulerHandle = std::shared_ptr<AlarmScheduler>;
    // Construct an alarm runner from a vector of shared_ptr<AlarmScheduler>'s.
    explicit AlarmRunnerBackgroundThread(std::vector<AlarmSchedulerHandle> container)
        : _schedulers(_initializeSchedulers(std::move(container))) {}

    /*
     * Starts a background thread that will process alarms from all registered schedulers.
     */
    void start();

    /*
     * Clears all outstanding timers from all registered schedulers and shuts down the background
     * thread.
     */
    void shutdown();

private:
    std::vector<AlarmSchedulerHandle> _initializeSchedulers(
        std::vector<AlarmSchedulerHandle> container);

    void _threadRoutine();

    Mutex _mutex = MONGO_MAKE_LATCH("AlarmRunnerBackgroundThread::_mutex");
    stdx::condition_variable _condVar;
    bool _running = false;
    Date_t _nextAlarm = Date_t::max();
    std::vector<AlarmSchedulerHandle> _schedulers;
    stdx::thread _thread;
};

}  // namespace mongo
