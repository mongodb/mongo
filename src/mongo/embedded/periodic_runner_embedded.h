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

#include <memory>
#include <vector>

#include "mongo/db/service_context_fwd.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

/**
 * An implementation of the PeriodicRunner which exposes a pump function to execute jobs on the
 * calling thread.
 */
class PeriodicRunnerEmbedded : public PeriodicRunner {
public:
    PeriodicRunnerEmbedded(ServiceContext* svc, ClockSource* clockSource);

    JobAnchor makeJob(PeriodicJob job) override;

    // Safe to call from multiple threads but will only execute on one thread at a time.
    // Returns true if it attempted to run any jobs.
    bool tryPump();

private:
    class PeriodicJobImpl : public ControllableJob {
        PeriodicJobImpl(const PeriodicJobImpl&) = delete;
        PeriodicJobImpl& operator=(const PeriodicJobImpl&) = delete;

    public:
        friend class PeriodicRunnerEmbedded;
        PeriodicJobImpl(PeriodicJob job, ClockSource* source, PeriodicRunnerEmbedded* runner);

        void start() override;
        void pause() override;
        void resume() override;
        void stop() override;
        Milliseconds getPeriod() override;
        void setPeriod(Milliseconds ms) override;

        bool isAlive(WithLock lk);

        Date_t nextScheduledRun() const {
            return _lastRun + _job.interval;
        }

        enum class ExecutionStatus { kNotScheduled, kRunning, kPaused, kCanceled };

    private:
        void _stopWithMasterAndJobLock(WithLock masterLock, WithLock jobLock);

        PeriodicJob _job;
        ClockSource* _clockSource;
        PeriodicRunnerEmbedded* _periodicRunner;
        Date_t _lastRun{};

        // The mutex is protecting _execStatus, the variable that can be accessed from other
        // threads.
        stdx::mutex _mutex;

        // The current execution status of the job.
        ExecutionStatus _execStatus{ExecutionStatus::kNotScheduled};
    };
    struct PeriodicJobSorter;

    ServiceContext* _svc;
    ClockSource* _clockSource;

    // min-heap for running jobs, next job to run in front()
    std::vector<std::shared_ptr<PeriodicJobImpl>> _jobs;
    std::vector<std::shared_ptr<PeriodicJobImpl>> _Pausedjobs;

    stdx::mutex _mutex;
};

}  // namespace mongo
