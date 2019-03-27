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

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

class Client;
class ServiceContext;

/**
 * An implementation of the PeriodicRunner which uses a thread per job and condvar waits on those
 * threads to independently sleep.
 */
class PeriodicRunnerImpl : public PeriodicRunner {
public:
    PeriodicRunnerImpl(ServiceContext* svc, ClockSource* clockSource);
    ~PeriodicRunnerImpl();

    std::unique_ptr<PeriodicRunner::PeriodicJobHandle> makeJob(PeriodicJob job) override;
    void scheduleJob(PeriodicJob job) override;

    void startup() override;

    void shutdown() override;

private:
    class PeriodicJobImpl {
        PeriodicJobImpl(const PeriodicJobImpl&) = delete;
        PeriodicJobImpl& operator=(const PeriodicJobImpl&) = delete;

    public:
        friend class PeriodicRunnerImpl;
        PeriodicJobImpl(PeriodicJob job, ClockSource* source, ServiceContext* svc);

        void start();
        void pause();
        void resume();
        void stop();

        enum class ExecutionStatus { NOT_SCHEDULED, RUNNING, PAUSED, CANCELED };

    private:
        void _run();

        PeriodicJob _job;
        ClockSource* _clockSource;
        ServiceContext* _serviceContext;
        stdx::thread _thread;

        stdx::mutex _mutex;
        stdx::condition_variable _condvar;
        /**
         * The current execution status of the job.
         */
        ExecutionStatus _execStatus{ExecutionStatus::NOT_SCHEDULED};
    };

    std::shared_ptr<PeriodicRunnerImpl::PeriodicJobImpl> createAndAddJob(PeriodicJob job);

    class PeriodicJobHandleImpl : public PeriodicJobHandle {
    public:
        explicit PeriodicJobHandleImpl(std::weak_ptr<PeriodicJobImpl> jobImpl)
            : _jobWeak(jobImpl) {}
        void start() override;
        void stop() override;
        void pause() override;
        void resume() override;

    private:
        std::weak_ptr<PeriodicJobImpl> _jobWeak;
    };

    ServiceContext* _svc;
    ClockSource* _clockSource;

    std::vector<std::shared_ptr<PeriodicJobImpl>> _jobs;

    stdx::mutex _mutex;
    bool _running = false;
};

}  // namespace mongo
