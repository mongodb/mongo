// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

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
        Milliseconds getPeriod() const override;
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

        mutable std::mutex _mutex;
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
