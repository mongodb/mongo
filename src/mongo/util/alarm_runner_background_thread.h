// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/alarm.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/*
 * This is a runner for alarm schedulers that waits for and processes alarms in a single
 * background thread.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] AlarmRunnerBackgroundThread {
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

    std::mutex _mutex;
    stdx::condition_variable _condVar;
    bool _running = false;
    Date_t _nextAlarm = Date_t::max();
    std::vector<AlarmSchedulerHandle> _schedulers;
    stdx::thread _thread;
};

}  // namespace mongo
