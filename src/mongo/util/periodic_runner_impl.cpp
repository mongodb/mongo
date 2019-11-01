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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/util/periodic_runner_impl.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

PeriodicRunnerImpl::PeriodicRunnerImpl(ServiceContext* svc, ClockSource* clockSource)
    : _svc(svc), _clockSource(clockSource) {}

auto PeriodicRunnerImpl::makeJob(PeriodicJob job) -> JobAnchor {
    auto impl = std::make_shared<PeriodicJobImpl>(std::move(job), this->_clockSource, this->_svc);

    JobAnchor anchor(std::move(impl));
    return anchor;
}

PeriodicRunnerImpl::PeriodicJobImpl::PeriodicJobImpl(PeriodicJob job,
                                                     ClockSource* source,
                                                     ServiceContext* svc)
    : _job(std::move(job)), _clockSource(source), _serviceContext(svc) {}

void PeriodicRunnerImpl::PeriodicJobImpl::_run() {
    auto [startPromise, startFuture] = makePromiseFuture<void>();

    {
        stdx::lock_guard lk(_mutex);
        invariant(_execStatus == ExecutionStatus::NOT_SCHEDULED);
    }


    _thread = stdx::thread([this, startPromise = std::move(startPromise)]() mutable {
        auto guard = makeGuard([this] { _stopPromise.emplaceValue(); });

        Client::initThread(_job.name, _serviceContext, nullptr);

        // Let start() know we're running
        {
            stdx::lock_guard lk(_mutex);
            _execStatus = PeriodicJobImpl::ExecutionStatus::RUNNING;
        }
        startPromise.emplaceValue();

        stdx::unique_lock<Latch> lk(_mutex);
        while (_execStatus != ExecutionStatus::CANCELED) {
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

    // Wait for the thread to actually start
    startFuture.get();
}

void PeriodicRunnerImpl::PeriodicJobImpl::start() {
    LOG(2) << "Starting periodic job " << _job.name;

    _run();
}

void PeriodicRunnerImpl::PeriodicJobImpl::pause() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_execStatus == PeriodicJobImpl::ExecutionStatus::RUNNING);
    _execStatus = PeriodicJobImpl::ExecutionStatus::PAUSED;
}

void PeriodicRunnerImpl::PeriodicJobImpl::resume() {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        invariant(_execStatus == PeriodicJobImpl::ExecutionStatus::PAUSED);
        _execStatus = PeriodicJobImpl::ExecutionStatus::RUNNING;
    }
    _condvar.notify_one();
}

void PeriodicRunnerImpl::PeriodicJobImpl::stop() {
    auto lastExecStatus = [&] {
        stdx::lock_guard<Latch> lk(_mutex);

        return std::exchange(_execStatus, ExecutionStatus::CANCELED);
    }();

    // If we never started, then nobody should wait
    if (lastExecStatus == ExecutionStatus::NOT_SCHEDULED) {
        return;
    }

    // Only join once
    if (lastExecStatus != ExecutionStatus::CANCELED) {
        LOG(2) << "Stopping periodic job " << _job.name;

        _condvar.notify_one();
        _thread.join();
    }

    _stopPromise.getFuture().get();
}

Milliseconds PeriodicRunnerImpl::PeriodicJobImpl::getPeriod() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _job.interval;
}

void PeriodicRunnerImpl::PeriodicJobImpl::setPeriod(Milliseconds ms) {
    stdx::lock_guard<Latch> lk(_mutex);
    _job.interval = ms;

    if (_execStatus == PeriodicJobImpl::ExecutionStatus::RUNNING) {
        _condvar.notify_one();
    }
}

}  // namespace mongo
