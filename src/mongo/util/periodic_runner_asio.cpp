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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include <algorithm>
#include <memory>

#include "mongo/util/periodic_runner_asio.h"

#include "mongo/util/log.h"

namespace mongo {

PeriodicRunnerASIO::PeriodicRunnerASIO(
    std::unique_ptr<executor::AsyncTimerFactoryInterface> timerFactory)
    : _io_service(),
      _strand(_io_service),
      _timerFactory(std::move(timerFactory)),
      _state(State::kReady) {}

PeriodicRunnerASIO::~PeriodicRunnerASIO() {
    // We must call shutdown here to join our background thread.
    shutdown();
}

void PeriodicRunnerASIO::scheduleJob(PeriodicJob job) {
    // The interval we use here will get written over by _scheduleJob_inlock.
    auto uniqueTimer = _timerFactory->make(&_strand, Milliseconds{0});
    std::shared_ptr<executor::AsyncTimerInterface> timer{std::move(uniqueTimer)};

    auto asioJob = std::make_shared<PeriodicJobASIO>(std::move(job), _timerFactory->now(), timer);

    {
        stdx::unique_lock<stdx::mutex> lk(_stateMutex);
        _jobs.insert(_jobs.end(), asioJob);
        if (_state == State::kRunning) {
            _scheduleJob(asioJob);
        }
    }
}

void PeriodicRunnerASIO::_scheduleJob(std::weak_ptr<PeriodicJobASIO> job) {
    auto lockedJob = job.lock();
    if (!lockedJob) {
        return;
    }

    // Adjust the timer to expire at the correct time.
    auto adjustedMS =
        std::max(Milliseconds(0), lockedJob->start + lockedJob->interval - _timerFactory->now());
    lockedJob->timer->expireAfter(adjustedMS);
    lockedJob->timer->asyncWait([this, job](std::error_code ec) mutable {
        if (ec) {
            severe() << "Encountered an error in PeriodicRunnerASIO: " << ec.message();
            return;
        }

        stdx::unique_lock<stdx::mutex> lk(_stateMutex);
        if (_state != State::kRunning) {
            return;
        }

        auto lockedJob = job.lock();
        if (!lockedJob) {
            return;
        }

        lockedJob->start = _timerFactory->now();
        lockedJob->job();

        _io_service.post([this, job]() mutable { _scheduleJob(job); });
    });
}

Status PeriodicRunnerASIO::startup() {
    stdx::unique_lock<stdx::mutex> lk(_stateMutex);
    if (_state != State::kReady) {
        return {ErrorCodes::ShutdownInProgress, "startup() already called"};
    }

    _state = State::kRunning;
    _thread = stdx::thread([this]() {
        try {
            asio::io_service::work workItem(_io_service);
            std::error_code ec;
            _io_service.run(ec);
            if (ec) {
                severe() << "Failure in PeriodicRunnerASIO: " << ec.message();
                fassertFailed(40438);
            }
        } catch (...) {
            severe() << "Uncaught exception in PeriodicRunnerASIO: " << exceptionToStatus();
            fassertFailed(40439);
        }
    });

    // schedule any jobs that we have
    for (auto& job : _jobs) {
        job->start = _timerFactory->now();
        _scheduleJob(job);
    }

    return Status::OK();
}

void PeriodicRunnerASIO::shutdown() {
    stdx::unique_lock<stdx::mutex> lk(_stateMutex);
    if (_state == State::kRunning) {
        _state = State::kComplete;

        _io_service.stop();
        _jobs.clear();

        lk.unlock();

        _thread.join();
    }
}

}  // namespace mongo
