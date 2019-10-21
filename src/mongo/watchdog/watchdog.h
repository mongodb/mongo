/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/filesystem/path.hpp>
#include <functional>
#include <string>
#include <vector>

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/duration.h"

namespace mongo {

class OperationContext;

/**
 * WatchdogDeathCallback is used by the watchdog component to terminate the process. It is expected
 * to bypass MongoDB's normal shutdown process. It should not make any syscalls other then to
 * exit/terminate the process.
 *
 * It is pluggable for testing purposes.
 */
using WatchdogDeathCallback = std::function<void(void)>;

/**
 * The OS specific implementation of WatchdogDeathCallback that kills the process.
 */
void watchdogTerminate();

/**
 * WatchdogCheck represents a health check that the watchdog will run periodically to ensure the
 * machine, and process are healthy.
 *
 * It is pluggable for testing purposes.
 */
class WatchdogCheck {
public:
    virtual ~WatchdogCheck() = default;

    /**
     * Runs a health check against the local machine.
     *
     * Note: It should throw exceptions on unexpected errors. Exceptions will result in a call to
     * WatchdogDeathCallback.
     */
    virtual void run(OperationContext* opCtx) = 0;

    /**
     * Returns a description for the watchdog check to log to the log file.
     */
    virtual std::string getDescriptionForLogging() = 0;
};

/**
 * Do a health check for a given directory. This health check is done by reading, and writing to a
 * file with direct I/O.
 */
class DirectoryCheck : public WatchdogCheck {
public:
    static constexpr StringData kProbeFileName = "watchdog_probe_"_sd;
    static constexpr StringData kProbeFileNameExt = ".txt"_sd;

public:
    DirectoryCheck(const boost::filesystem::path& directory) : _directory(directory) {}

    void run(OperationContext* opCtx) final;

    std::string getDescriptionForLogging() final;

private:
    boost::filesystem::path _directory;
};

/**
 * Runs a callback on a periodic basis. The specified time period is the time delay between
 * invocations.
 *
 * Example:
 * - callback
 * - sleep(period)
 * - callback
 */
class WatchdogPeriodicThread {
public:
    WatchdogPeriodicThread(Milliseconds period, StringData threadName);
    virtual ~WatchdogPeriodicThread() = default;

    /**
     * Starts the periodic thread.
     */
    void start();

    /**
     * Updates the period the thread runs its task.
     *
     * Period changes take affect immediately.
     */
    void setPeriod(Milliseconds period);

    /**
     * Shutdown the periodic thread. After it is shutdown, it cannot be started.
     */
    void shutdown();

protected:
    /**
     * Do one iteration of work.
     */
    virtual void run(OperationContext* opCtx) = 0;

    /**
     * Provides an opportunity for derived classes to initialize state.
     *
     * This method is called at two different times:
     * 1. First time a thread is started.
     * 2. When a thread goes from disabled to enabled. Specifically, a user calls setPeriod(-1)
     *    followed by setPeriod(> 0).
     *
     */
    virtual void resetState() = 0;

private:
    /**
     * Main thread loop
     */
    void doLoop();

private:
    /**
     * Private enum to track state.
     *
     *   +----------------------------------------------------------------+
     *   |                                                                v
     * +-------------+     +----------+     +--------------------+     +-------+
     * | kNotStarted | --> | kStarted | --> | kShutdownRequested | --> | kDone |
     * +-------------+     +----------+     +--------------------+     +-------+
     */
    enum class State {
        /**
         * Initial state. Either start() or shutdown() can be called next.
         */
        kNotStarted,

        /**
         * start() has been called. shutdown() should be called next.
         */
        kStarted,

        /**
         * shutdown() has been called, and the thread is in progress of shutting down.
         */
        kShutdownRequested,

        /**
         * PeriodicThread has been shutdown.
         */
        kDone,
    };

    // State of PeriodicThread
    State _state{State::kNotStarted};

    // Thread period
    Milliseconds _period;

    // if true, then call run() otherwise just let the thread idle,
    bool _enabled;

    // Name of thread for logging purposes
    std::string _threadName;

    // The thread
    stdx::thread _thread;

    // Lock to protect _state and control _thread
    Mutex _mutex = MONGO_MAKE_LATCH("WatchdogPeriodicThread::_mutex");
    stdx::condition_variable _condvar;
};

/**
 * Periodic background thread to run watchdog checks.
 */
class WatchdogCheckThread : public WatchdogPeriodicThread {
public:
    WatchdogCheckThread(std::vector<std::unique_ptr<WatchdogCheck>> checks, Milliseconds period);

    /**
     * Returns the current generation number of the checks.
     *
     * Incremented after each check is run.
     */
    std::int64_t getGeneration();

private:
    void run(OperationContext* opCtx) final;
    void resetState() final;

private:
    // Vector of checks to run
    std::vector<std::unique_ptr<WatchdogCheck>> _checks;

    // A counter that is incremented for each watchdog check completed, and monitored to ensure it
    // does not remain at the same value for too long.
    AtomicWord<long long> _checkGeneration{0};
};

/**
 * Periodic background thread to ensure watchdog checks run periodically.
 */
class WatchdogMonitorThread : public WatchdogPeriodicThread {
public:
    WatchdogMonitorThread(WatchdogCheckThread* checkThread,
                          WatchdogDeathCallback callback,
                          Milliseconds period);

    /**
     * Returns the current generation number of the monitor.
     *
     * Incremented after each round of monitoring is run.
     */
    std::int64_t getGeneration();

private:
    void run(OperationContext* opCtx) final;
    void resetState() final;

private:
    // Callback function to call when watchdog gets stuck
    const WatchdogDeathCallback _callback;

    // Watchdog check thread to query
    WatchdogCheckThread* _checkThread;

    // A counter that is incremented for each watchdog monitor run is completed.
    AtomicWord<long long> _monitorGeneration{0};

    // The last seen _checkGeneration value
    std::int64_t _lastSeenGeneration{-1};
};


/**
 * WatchdogMonitor
 *
 * The Watchdog is a pair of dedicated threads that try to figure out if a process is hung
 * and terminate if it is. The worst case scenario in a distributed system is a process that appears
 * to work but does not actually work.
 *
 * The watchdog is not designed to detect all the different ways the process is hung. It's goal is
 * to detect if the storage system is stuck, and to terminate the process if it is stuck.
 *
 * Threads:
 * WatchdogCheck - runs file system checks
 * WatchdogMonitor - verifies that WatchdogCheck continue to make timely progress. If WatchdogCheck
 *                   fails to make process, WatchdogMonitor calls a callback. The callback is not
 *                   expected to do any I/O and minimize the system calls it makes.
 */
class WatchdogMonitor {
public:
    /**
     * Create the watchdog with specified period.
     *
     * checkPeriod - how often to run the checks
     * monitorPeriod - how often to run the monitor, must be >= checkPeriod
     */
    WatchdogMonitor(std::vector<std::unique_ptr<WatchdogCheck>> checks,
                    Milliseconds checkPeriod,
                    Milliseconds monitorPeriod,
                    WatchdogDeathCallback callback);

    /**
     * Starts the watchdog threads.
     */
    void start();

    /**
     * Updates the watchdog monitor period. The goal is to detect a failure in the time of the
     * period.
     *
     * Does nothing if watchdog is not started. If watchdog was started, it changes the monitor
     * period, but not the check period.
     *
     * Accepts Milliseconds for testing purposes while the setParameter only works with seconds.
     */
    void setPeriod(Milliseconds duration);

    /**
     * Shutdown the watchdog.
     */
    void shutdown();

    /**
     * Returns the current generation number of the checks.
     *
     * Incremented after each round of checks is run.
     */
    std::int64_t getCheckGeneration();

    /**
     * Returns the current generation number of the checks.
     *
     * Incremented after each round of checks is run.
     */
    std::int64_t getMonitorGeneration();

private:
    /**
     * Private enum to track state.
     *
     *   +----------------------------------------------------------------+
     *   |                                                                v
     * +-------------+     +----------+     +--------------------+     +-------+
     * | kNotStarted | --> | kStarted | --> | kShutdownRequested | --> | kDone |
     * +-------------+     +----------+     +--------------------+     +-------+
     */
    enum class State {
        /**
         * Initial state. Either start() or shutdown() can be called next.
         */
        kNotStarted,

        /**
         * start() has been called. shutdown() should be called next.
         */
        kStarted,

        /**
         * shutdown() has been called, and the background threads are in progress of shutting down.
         */
        kShutdownRequested,

        /**
         * Watchdog has been shutdown.
         */
        kDone,
    };

    // Lock to protect _state and control _thread
    Mutex _mutex = MONGO_MAKE_LATCH("WatchdogMonitor::_mutex");

    // State of watchdog
    State _state{State::kNotStarted};

    // Fixed period for running the checks.
    Milliseconds _checkPeriod;

    // WatchdogCheck Thread - runs checks
    WatchdogCheckThread _watchdogCheckThread;

    // WatchdogMonitor Thread - watches _watchdogCheckThread
    WatchdogMonitorThread _watchdogMonitorThread;
};

}  // namespace mongo
