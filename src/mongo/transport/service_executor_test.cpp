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
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/db/service_context_noop.h"
#include "mongo/transport/service_executor_adaptive.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

#include <asio.hpp>

namespace mongo {
namespace {
using namespace transport;

struct TestOptions : public ServiceExecutorAdaptive::Options {
    int reservedThreads() const final {
        return 1;
    }

    Milliseconds workerThreadRunTime() const final {
        return Milliseconds{1000};
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

class ServiceExecutorAdaptiveFixture : public unittest::Test {
protected:
    void setUp() override {
        auto scOwned = stdx::make_unique<ServiceContextNoop>();
        setGlobalServiceContext(std::move(scOwned));
        asioIOCtx = std::make_shared<asio::io_context>();

        auto configOwned = stdx::make_unique<TestOptions>();
        executorConfig = configOwned.get();
        executor = stdx::make_unique<ServiceExecutorAdaptive>(
            getGlobalServiceContext(), asioIOCtx, std::move(configOwned));
    }

    ServiceExecutorAdaptive::Options* executorConfig;
    std::unique_ptr<ServiceExecutorAdaptive> executor;
    std::shared_ptr<asio::io_context> asioIOCtx;
};

class ServiceExecutorSynchronousFixture : public unittest::Test {
protected:
    void setUp() override {
        auto scOwned = stdx::make_unique<ServiceContextNoop>();
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
    auto status = exec->schedule(std::move(task), ServiceExecutor::kEmptyFlags);
    if (expectSuccess) {
        ASSERT_OK(status);
        cond.wait(lk);
    } else {
        ASSERT_NOT_OK(status);
    }
}

TEST_F(ServiceExecutorAdaptiveFixture, BasicTaskRuns) {
    ASSERT_OK(executor->start());
    auto guard = MakeGuard([this] { ASSERT_OK(executor->shutdown(Milliseconds{500})); });

    scheduleBasicTask(executor.get(), true);
}

TEST_F(ServiceExecutorAdaptiveFixture, ScheduleFailsBeforeStartup) {
    scheduleBasicTask(executor.get(), false);
}

TEST_F(ServiceExecutorSynchronousFixture, BasicTaskRuns) {
    ASSERT_OK(executor->start());
    auto guard = MakeGuard([this] { ASSERT_OK(executor->shutdown(Milliseconds{500})); });

    scheduleBasicTask(executor.get(), true);
}

TEST_F(ServiceExecutorSynchronousFixture, ScheduleFailsBeforeStartup) {
    scheduleBasicTask(executor.get(), false);
}


}  // namespace
}  // namespace mongo
