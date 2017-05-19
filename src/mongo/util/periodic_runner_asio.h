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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <asio.hpp>

#include "mongo/executor/async_timer_interface.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

/**
 * A PeriodicRunner implementation that uses the ASIO library's eventing system
 * to schedule and run jobs at regular intervals.
 *
 * This class takes a timer factory so that it may be mocked out for testing.
 */
class PeriodicRunnerASIO : public PeriodicRunner {
public:
    using PeriodicRunner::PeriodicJob;

    /**
     * Construct a new instance of this class using the provided timer factory.
     */
    explicit PeriodicRunnerASIO(std::unique_ptr<executor::AsyncTimerFactoryInterface> timerFactory);

    ~PeriodicRunnerASIO();

    /**
     * Schedule a job to be run at periodic intervals.
     */
    void scheduleJob(PeriodicJob job) override;

    /**
     * Starts up this periodic runner.
     *
     * This periodic runner will only run once; if it is subsequently started up
     * again, it will return an error.
     */
    Status startup() override;

    /**
     * Shut down this periodic runner. Stops all jobs from running. This method
     * may safely be called multiple times, but only the first call will have any effect.
     */
    void shutdown() override;

private:
    struct PeriodicJobASIO {
        explicit PeriodicJobASIO(PeriodicJob callable,
                                 Date_t startTime,
                                 std::shared_ptr<executor::AsyncTimerInterface> sharedTimer)
            : job(std::move(callable.job)),
              interval(callable.interval),
              start(startTime),
              timer(sharedTimer) {}
        Job job;
        Milliseconds interval;
        Date_t start;
        std::shared_ptr<executor::AsyncTimerInterface> timer;
    };

    // Internally, we will transition through these states
    enum class State { kReady, kRunning, kComplete };

    void _scheduleJob(std::weak_ptr<PeriodicJobASIO> job);

    asio::io_service _io_service;
    asio::io_service::strand _strand;

    stdx::thread _thread;

    std::unique_ptr<executor::AsyncTimerFactoryInterface> _timerFactory;

    stdx::mutex _stateMutex;
    State _state;

    std::vector<std::shared_ptr<PeriodicJobASIO>> _jobs;
};

}  // namespace mongo
