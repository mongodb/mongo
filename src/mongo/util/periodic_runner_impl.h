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

#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/future.h"
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

    JobAnchor makeJob(PeriodicJob job) override;

private:
    class PeriodicJobImpl : public ControllableJob {
        PeriodicJobImpl(const PeriodicJobImpl&) = delete;
        PeriodicJobImpl& operator=(const PeriodicJobImpl&) = delete;

    public:
        friend class PeriodicRunnerImpl;
        PeriodicJobImpl(PeriodicJob job, ClockSource* source, ServiceContext* svc);

        void start() override;
        void pause() override;
        void resume() override;
        void stop() override;
        Milliseconds getPeriod() override;
        void setPeriod(Milliseconds ms) override;

        enum class ExecutionStatus { NOT_SCHEDULED, RUNNING, PAUSED, CANCELED };

    private:
        void _run();

        PeriodicJob _job;
        ClockSource* _clockSource;
        ServiceContext* _serviceContext;

        Client* _client;
        stdx::thread _thread;
        SharedPromise<void> _stopPromise;

        Mutex _mutex = MONGO_MAKE_LATCH("PeriodicJobImpl::_mutex");
        stdx::condition_variable _condvar;
        /**
         * The current execution status of the job.
         */
        ExecutionStatus _execStatus{ExecutionStatus::NOT_SCHEDULED};
    };

    std::shared_ptr<PeriodicRunnerImpl::PeriodicJobImpl> createAndAddJob(PeriodicJob job);

    ServiceContext* _svc;
    ClockSource* _clockSource;
};

}  // namespace mongo
