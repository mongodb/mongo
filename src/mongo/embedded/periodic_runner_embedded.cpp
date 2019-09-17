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

#include "mongo/embedded/periodic_runner_embedded.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

struct PeriodicRunnerEmbedded::PeriodicJobSorter {
    bool operator()(std::shared_ptr<PeriodicJobImpl> const& lhs,
                    std::shared_ptr<PeriodicJobImpl> const& rhs) const {
        // Use greater-than to make the heap a min-heap
        return lhs->nextScheduledRun() > rhs->nextScheduledRun();
    }
};

PeriodicRunnerEmbedded::PeriodicRunnerEmbedded(ServiceContext* svc, ClockSource* clockSource)
    : _svc(svc), _clockSource(clockSource) {}

auto PeriodicRunnerEmbedded::makeJob(PeriodicJob job) -> JobAnchor {
    auto impl = std::make_shared<PeriodicJobImpl>(std::move(job), this->_clockSource, this);

    stdx::lock_guard<Latch> lk(_mutex);
    _jobs.push_back(impl);
    std::push_heap(_jobs.begin(), _jobs.end(), PeriodicJobSorter());
    return JobAnchor(impl);
}

bool PeriodicRunnerEmbedded::tryPump() {
    stdx::unique_lock<Latch> lock(_mutex, stdx::try_to_lock);
    if (!lock.owns_lock())
        return false;

    const auto now = _clockSource->now();

    // First check if any paused jobs have been set to running again
    for (auto it = _Pausedjobs.begin(); it != _Pausedjobs.end();) {
        auto& job = *(*it);

        PeriodicJobImpl::ExecutionStatus jobExecStatus;
        {
            stdx::lock_guard<Latch> jobLock(job._mutex);
            jobExecStatus = job._execStatus;
        }

        if (jobExecStatus == PeriodicJobImpl::ExecutionStatus::kPaused ||
            jobExecStatus == PeriodicJobImpl::ExecutionStatus::kNotScheduled) {
            ++it;
            continue;
        }

        // If running job found, push to running heap
        if (jobExecStatus == PeriodicJobImpl::ExecutionStatus::kRunning) {
            _jobs.push_back(std::move(*it));
            std::push_heap(_jobs.begin(), _jobs.end(), PeriodicJobSorter());
        }

        // Running or cancelled jobs should be removed from the paused queue
        std::swap(*it, _Pausedjobs.back());
        _Pausedjobs.pop_back();
    }

    while (!_jobs.empty()) {
        auto& job = *_jobs.front();
        if (now < job.nextScheduledRun())
            break;

        // Begin with taking out current job from the heap
        std::pop_heap(_jobs.begin(), _jobs.end(), PeriodicJobSorter());

        // Just need to hold the job lock while interacting with the execution status, it's the
        // only variable that can be changed from other threads.
        PeriodicJobImpl::ExecutionStatus jobExecStatus;
        {
            stdx::lock_guard<Latch> jobLock(job._mutex);
            jobExecStatus = job._execStatus;
        }

        switch (jobExecStatus) {
            default:
                invariant(false);
            case PeriodicJobImpl::ExecutionStatus::kPaused:
            case PeriodicJobImpl::ExecutionStatus::kNotScheduled:
                // Paused jobs should be moved to the paused list and removed from the running heap
                _Pausedjobs.push_back(std::move(_jobs.back()));
            // fall through
            case PeriodicJobImpl::ExecutionStatus::kCanceled:
                // Cancelled jobs should be removed
                _jobs.pop_back();
                continue;
            case PeriodicJobImpl::ExecutionStatus::kRunning:
                break;
        };

        // If we get here, the job is in the running state.
        // Run the job without holding the lock so we can pause/cancel concurrently
        job._job.job(Client::getCurrent());

        // Update that the job has executed and put back in heap
        job._lastRun = now;
        std::push_heap(_jobs.begin(), _jobs.end(), PeriodicJobSorter());
    }

    return true;
}

PeriodicRunnerEmbedded::PeriodicJobImpl::PeriodicJobImpl(PeriodicJob job,
                                                         ClockSource* source,
                                                         PeriodicRunnerEmbedded* runner)
    : _job(std::move(job)), _clockSource(source), _periodicRunner(runner) {}

void PeriodicRunnerEmbedded::PeriodicJobImpl::start() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_execStatus == PeriodicJobImpl::ExecutionStatus::kNotScheduled);
    _execStatus = PeriodicJobImpl::ExecutionStatus::kRunning;
}

void PeriodicRunnerEmbedded::PeriodicJobImpl::pause() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_execStatus == PeriodicJobImpl::ExecutionStatus::kRunning);
    _execStatus = PeriodicJobImpl::ExecutionStatus::kPaused;
}

void PeriodicRunnerEmbedded::PeriodicJobImpl::resume() {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_execStatus == PeriodicJobImpl::ExecutionStatus::kPaused);
    _execStatus = PeriodicJobImpl::ExecutionStatus::kRunning;
}

void PeriodicRunnerEmbedded::PeriodicJobImpl::stop() {
    // Also take the master lock, the job lock is not held while executing the job and we must make
    // sure the user can invalidate it after this call.
    stdx::lock_guard<Latch> masterLock(_periodicRunner->_mutex);
    stdx::lock_guard<Latch> lk(_mutex);
    if (isAlive(lk)) {
        _stopWithMasterAndJobLock(masterLock, lk);
    }
}

Milliseconds PeriodicRunnerEmbedded::PeriodicJobImpl::getPeriod() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _job.interval;
}

void PeriodicRunnerEmbedded::PeriodicJobImpl::setPeriod(Milliseconds ms) {
    stdx::lock_guard<Latch> masterLk(_periodicRunner->_mutex);
    stdx::lock_guard<Latch> lk(_mutex);

    _job.interval = ms;

    if (_execStatus == PeriodicJobImpl::ExecutionStatus::kRunning) {
        std::make_heap(
            _periodicRunner->_jobs.begin(), _periodicRunner->_jobs.end(), PeriodicJobSorter());
    }
}

void PeriodicRunnerEmbedded::PeriodicJobImpl::_stopWithMasterAndJobLock(WithLock masterLock,
                                                                        WithLock jobLock) {
    invariant(isAlive(jobLock));
    _execStatus = PeriodicJobImpl::ExecutionStatus::kCanceled;
}

bool PeriodicRunnerEmbedded::PeriodicJobImpl::isAlive(WithLock lk) {
    return _execStatus == ExecutionStatus::kRunning || _execStatus == ExecutionStatus::kPaused;
}

}  // namespace mongo
