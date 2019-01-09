
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault;

#include "mongo/platform/basic.h"

#include "boost/optional.hpp"

#include "mongo/db/service_context.h"
#include "mongo/transport/service_executor_adaptive.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/service_executor_task_names.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

#include <asio.hpp>

namespace mongo {
namespace {
using namespace transport;

namespace {
constexpr Milliseconds kWorkerThreadRunTime{1000};
// Run time + generous scheduling time slice
const Milliseconds kShutdownTime = kWorkerThreadRunTime + Milliseconds{50};
}

struct TestOptions : public ServiceExecutorAdaptive::Options {
    int reservedThreads() const final {
        return 1;
    }

    Milliseconds workerThreadRunTime() const final {
        return kWorkerThreadRunTime;
    }

    int runTimeJitter() const final {
        return 0;
    }

    Milliseconds stuckThreadTimeout() const final {
        return Milliseconds{100};
    }

    Microseconds maxQueueLatency() const final {
        return duration_cast<Microseconds>(Milliseconds{5});
    }

    int idlePctThreshold() const final {
        return 0;
    }

    int recursionLimit() const final {
        return 0;
    }
};

/* This implements the portions of the transport::Reactor based on ASIO, but leaves out
 * the methods not needed by ServiceExecutors.
 *
 * TODO Maybe use TransportLayerASIO's Reactor?
 */
class ASIOReactor : public transport::Reactor {
public:
    ASIOReactor() : _ioContext() {}

    void run() noexcept final {
        MONGO_UNREACHABLE;
    }

    void runFor(Milliseconds time) noexcept final {
        asio::io_context::work work(_ioContext);

        try {
            _ioContext.run_for(time.toSystemDuration());
        } catch (...) {
            severe() << "Uncaught exception in reactor: " << exceptionToStatus();
            fassertFailed(50476);
        }
    }

    void stop() final {
        _ioContext.stop();
    }

    void drain() override final {
        _ioContext.restart();
        while (_ioContext.poll()) {
            LOG(1) << "Draining remaining work in reactor.";
        }
        _ioContext.stop();
    }

    std::unique_ptr<ReactorTimer> makeTimer() final {
        MONGO_UNREACHABLE;
    }

    Date_t now() final {
        MONGO_UNREACHABLE;
    }

    void schedule(ScheduleMode mode, Task task) final {
        if (mode == kDispatch) {
            asio::dispatch(_ioContext, std::move(task));
        } else {
            asio::post(_ioContext, std::move(task));
        }
    }

    bool onReactorThread() const final {
        return false;
    }

    operator asio::io_context&() {
        return _ioContext;
    }

private:
    asio::io_context _ioContext;
};

class ServiceExecutorAdaptiveFixture : public unittest::Test {
protected:
    void setUp() override {
        auto scOwned = ServiceContext::make();
        setGlobalServiceContext(std::move(scOwned));

        auto configOwned = stdx::make_unique<TestOptions>();
        executorConfig = configOwned.get();
        executor = stdx::make_unique<ServiceExecutorAdaptive>(
            getGlobalServiceContext(), std::make_shared<ASIOReactor>(), std::move(configOwned));
    }

    ServiceExecutorAdaptive::Options* executorConfig;
    std::unique_ptr<ServiceExecutorAdaptive> executor;
    std::shared_ptr<asio::io_context> asioIOCtx;
};

class ServiceExecutorSynchronousFixture : public unittest::Test {
protected:
    void setUp() override {
        auto scOwned = ServiceContext::make();
        setGlobalServiceContext(std::move(scOwned));

        executor = stdx::make_unique<ServiceExecutorSynchronous>(getGlobalServiceContext());
    }

    std::unique_ptr<ServiceExecutorSynchronous> executor;
};

void scheduleBasicTask(ServiceExecutor* exec, bool expectSuccess) {
    stdx::condition_variable cond;
    stdx::mutex mutex;
    auto task = [&cond, &mutex] {
        stdx::unique_lock<stdx::mutex> lk(mutex);
        cond.notify_all();
    };

    stdx::unique_lock<stdx::mutex> lk(mutex);
    auto status = exec->schedule(
        std::move(task), ServiceExecutor::kEmptyFlags, ServiceExecutorTaskName::kSSMStartSession);
    if (expectSuccess) {
        ASSERT_OK(status);
        cond.wait(lk);
    } else {
        ASSERT_NOT_OK(status);
    }
}

TEST_F(ServiceExecutorAdaptiveFixture, BasicTaskRuns) {
    ASSERT_OK(executor->start());
    auto guard = makeGuard([this] { ASSERT_OK(executor->shutdown(kShutdownTime)); });

    scheduleBasicTask(executor.get(), true);
}

TEST_F(ServiceExecutorAdaptiveFixture, ScheduleFailsBeforeStartup) {
    scheduleBasicTask(executor.get(), false);
}

TEST_F(ServiceExecutorSynchronousFixture, BasicTaskRuns) {
    ASSERT_OK(executor->start());
    auto guard = makeGuard([this] { ASSERT_OK(executor->shutdown(kShutdownTime)); });

    scheduleBasicTask(executor.get(), true);
}

TEST_F(ServiceExecutorSynchronousFixture, ScheduleFailsBeforeStartup) {
    scheduleBasicTask(executor.get(), false);
}


}  // namespace
}  // namespace mongo
