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
    Status scheduleJob(PeriodicJob job) override;

    /**
     * Starts up this periodic runner.
     */
    void startup() override;

    /**
     * Shut down this periodic runner. Stops all jobs from running.
     */
    void shutdown() override;

private:
    struct PeriodicJobASIO {
        explicit PeriodicJobASIO(PeriodicJob callable)
            : job(std::move(callable.job)), interval(callable.interval), start(Date_t::now()) {}
        Job job;
        Milliseconds interval;
        Date_t start;
    };

    void _scheduleJob_inlock(PeriodicJobASIO job,
                             std::shared_ptr<executor::AsyncTimerInterface> timer,
                             const stdx::unique_lock<stdx::mutex>& lk);

    asio::io_service _io_service;
    asio::io_service::strand _strand;

    stdx::thread _thread;

    std::unique_ptr<executor::AsyncTimerFactoryInterface> _timerFactory;

    stdx::mutex _runningMutex;
    bool _running;
};

}  // namespace mongo
