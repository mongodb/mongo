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

#include "mongo/util/periodic_runner_impl.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

PeriodicRunnerImpl::PeriodicRunnerImpl(ServiceContext* svc, ClockSource* clockSource)
    : _svc(svc), _clockSource(clockSource) {}

PeriodicRunnerImpl::~PeriodicRunnerImpl() {
    PeriodicRunnerImpl::shutdown();
}

std::shared_ptr<PeriodicRunnerImpl::PeriodicJobImpl> PeriodicRunnerImpl::createAndAddJob(
    PeriodicJob job) {
    auto impl = std::make_shared<PeriodicJobImpl>(std::move(job), this->_clockSource, this->_svc);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _jobs.push_back(impl);
    return impl;
}

std::unique_ptr<PeriodicRunner::PeriodicJobHandle> PeriodicRunnerImpl::makeJob(PeriodicJob job) {
    auto handle = std::make_unique<PeriodicJobHandleImpl>(createAndAddJob(job));
    return std::move(handle);
}

void PeriodicRunnerImpl::scheduleJob(PeriodicJob job) {
    auto impl = createAndAddJob(job);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_running) {
        impl->start();
    }
}

void PeriodicRunnerImpl::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_running) {
        return;
    }

    _running = true;

    // schedule any jobs that we have
    for (auto& job : _jobs) {
        job->start();
    }
}

void PeriodicRunnerImpl::shutdown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_running) {
        _running = false;

        for (auto& job : _jobs) {
            job->stop();
        }
        _jobs.clear();
    }
}

PeriodicRunnerImpl::PeriodicJobImpl::PeriodicJobImpl(PeriodicJob job,
                                                     ClockSource* source,
                                                     ServiceContext* svc)
    : _job(std::move(job)), _clockSource(source), _serviceContext(svc) {}

void PeriodicRunnerImpl::PeriodicJobImpl::_run() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_execStatus == PeriodicJobImpl::ExecutionStatus::NOT_SCHEDULED);
    _thread = stdx::thread([this] {
        Client::initThread(_job.name, _serviceContext, nullptr);
        while (true) {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            // Wait until it's unpaused or canceled
            _condvar.wait(lk, [&] { return _execStatus != ExecutionStatus::PAUSED; });
            if (_execStatus == ExecutionStatus::CANCELED) {
                return;
            }

            auto start = _clockSource->now();

            // Unlock while job is running so we can pause/cancel concurrently
            lk.unlock();
            _job.job(Client::getCurrent());
            lk.lock();

            auto getDeadlineFromInterval = [&] { return start + _job.interval; };

            do {
                auto deadline = getDeadlineFromInterval();

                if (_clockSource->waitForConditionUntil(_condvar, lk, deadline, [&] {
                        return _execStatus == ExecutionStatus::CANCELED ||
                            getDeadlineFromInterval() != deadline;
                    })) {
                    if (_execStatus == ExecutionStatus::CANCELED) {
                        return;
                    }
                }
            } while (_clockSource->now() < getDeadlineFromInterval());
        }
    });
    _execStatus = PeriodicJobImpl::ExecutionStatus::RUNNING;
}

void PeriodicRunnerImpl::PeriodicJobImpl::start() {
    _run();
}

void PeriodicRunnerImpl::PeriodicJobImpl::pause() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_execStatus == PeriodicJobImpl::ExecutionStatus::RUNNING);
    _execStatus = PeriodicJobImpl::ExecutionStatus::PAUSED;
}

void PeriodicRunnerImpl::PeriodicJobImpl::resume() {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        invariant(_execStatus == PeriodicJobImpl::ExecutionStatus::PAUSED);
        _execStatus = PeriodicJobImpl::ExecutionStatus::RUNNING;
    }
    _condvar.notify_one();
}

void PeriodicRunnerImpl::PeriodicJobImpl::stop() {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (_execStatus != ExecutionStatus::RUNNING && _execStatus != ExecutionStatus::PAUSED)
            return;

        invariant(_thread.joinable());

        _execStatus = PeriodicJobImpl::ExecutionStatus::CANCELED;
    }
    _condvar.notify_one();
    _thread.join();
}

Milliseconds PeriodicRunnerImpl::PeriodicJobImpl::getPeriod() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _job.interval;
}

void PeriodicRunnerImpl::PeriodicJobImpl::setPeriod(Milliseconds ms) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _job.interval = ms;

    if (_execStatus == PeriodicJobImpl::ExecutionStatus::RUNNING) {
        _condvar.notify_one();
    }
}

namespace {

template <typename T>
std::shared_ptr<T> lockAndAssertExists(std::weak_ptr<T> ptr, StringData errMsg) {
    if (auto p = ptr.lock()) {
        return p;
    } else {
        uasserted(ErrorCodes::InternalError, errMsg);
    }
}

constexpr auto kPeriodicJobHandleLifetimeErrMsg =
    "The PeriodicRunner job for this handle no longer exists"_sd;

}  // namespace

void PeriodicRunnerImpl::PeriodicJobHandleImpl::start() {
    auto job = lockAndAssertExists(_jobWeak, kPeriodicJobHandleLifetimeErrMsg);
    job->start();
}

void PeriodicRunnerImpl::PeriodicJobHandleImpl::stop() {
    auto job = lockAndAssertExists(_jobWeak, kPeriodicJobHandleLifetimeErrMsg);
    job->stop();
}

void PeriodicRunnerImpl::PeriodicJobHandleImpl::pause() {
    auto job = lockAndAssertExists(_jobWeak, kPeriodicJobHandleLifetimeErrMsg);
    job->pause();
}

void PeriodicRunnerImpl::PeriodicJobHandleImpl::resume() {
    auto job = lockAndAssertExists(_jobWeak, kPeriodicJobHandleLifetimeErrMsg);
    job->resume();
}

Milliseconds PeriodicRunnerImpl::PeriodicJobHandleImpl::getPeriod() {
    auto job = lockAndAssertExists(_jobWeak, kPeriodicJobHandleLifetimeErrMsg);
    return job->getPeriod();
}

void PeriodicRunnerImpl::PeriodicJobHandleImpl::setPeriod(Milliseconds ms) {
    auto job = lockAndAssertExists(_jobWeak, kPeriodicJobHandleLifetimeErrMsg);
    job->setPeriod(ms);
}

}  // namespace mongo
