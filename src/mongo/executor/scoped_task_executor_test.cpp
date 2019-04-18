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

#include "mongo/executor/scoped_task_executor.h"

#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"

namespace mongo {
namespace executor {
namespace {

class ScopedTaskExecutorTest : public unittest::Test {
public:
    void setUp() override {
        auto net = std::make_unique<NetworkInterfaceMock>();
        _net = net.get();
        _tpte.emplace(std::make_unique<ThreadPoolMock>(_net, 1, ThreadPoolMock::Options{}),
                      std::move(net));
        _tpte->startup();
        _executor.emplace(_tpte.get_ptr());
    }

    void tearDown() override {
        _net->exitNetwork();

        if (_executor) {
            (*_executor)->shutdown();
            (*_executor)->join();
            _executor.reset();
        }
        _tpte.reset();
    }

    static inline thread_local bool isInline = false;

    void scheduleWork(Promise<void>& promise) {
        isInline = true;
        ASSERT(getExecutor()
                   ->scheduleWork([&](const TaskExecutor::CallbackArgs& ca) {
                       ASSERT_FALSE(isInline);
                       if (ca.status.isOK()) {
                           promise.emplaceValue();
                       } else {
                           promise.setError(ca.status);
                       }
                   })
                   .getStatus()
                   .isOK());
        isInline = false;
    }

    void scheduleRemoteCommand(Promise<void>& promise) {
        RemoteCommandRequest rcr(HostAndPort("localhost"), "test", BSONObj(), nullptr);

        isInline = true;
        ASSERT(getExecutor()
                   ->scheduleRemoteCommand(rcr,
                                           [&](const TaskExecutor::RemoteCommandCallbackArgs& ca) {
                                               ASSERT_FALSE(isInline);
                                               if (ca.response.status.isOK()) {
                                                   promise.emplaceValue();
                                               } else {
                                                   promise.setError(ca.response.status);
                                               }
                                           })
                   .getStatus()
                   .isOK());
        isInline = false;
    }

    void resetExecutor() {
        _executor.reset();
    }

    void shutdownUnderlying() {
        _tpte->shutdown();
    }

    ScopedTaskExecutor& getExecutor() {
        return *_executor;
    }

    NetworkInterfaceMock* getNet() {
        return _net;
    }

private:
    NetworkInterfaceMock* _net;
    boost::optional<ThreadPoolTaskExecutor> _tpte;
    boost::optional<ScopedTaskExecutor> _executor;
};

TEST_F(ScopedTaskExecutorTest, onEvent) {
    auto pf = makePromiseFuture<void>();

    auto event = uassertStatusOK(getExecutor()->makeEvent());

    ASSERT(getExecutor()
               ->onEvent(event,
                         [&](const TaskExecutor::CallbackArgs& ca) {
                             if (ca.status.isOK()) {
                                 pf.promise.emplaceValue();
                             } else {
                                 pf.promise.setError(ca.status);
                             }
                         })
               .getStatus()
               .isOK());

    ASSERT_FALSE(pf.future.isReady());

    getExecutor()->signalEvent(event);

    ASSERT_OK(pf.future.getNoThrow());
}

TEST_F(ScopedTaskExecutorTest, scheduleWork) {
    auto pf = makePromiseFuture<void>();

    ASSERT(getExecutor()
               ->scheduleWork([&](const TaskExecutor::CallbackArgs& ca) {
                   if (ca.status.isOK()) {
                       pf.promise.emplaceValue();
                   } else {
                       pf.promise.setError(ca.status);
                   }
               })
               .getStatus()
               .isOK());

    ASSERT_OK(pf.future.getNoThrow());
}

TEST_F(ScopedTaskExecutorTest, scheduleWorkAt) {
    auto pf = makePromiseFuture<void>();

    ASSERT(getExecutor()
               ->scheduleWorkAt(getExecutor()->now(),
                                [&](const TaskExecutor::CallbackArgs& ca) {
                                    if (ca.status.isOK()) {
                                        pf.promise.emplaceValue();
                                    } else {
                                        pf.promise.setError(ca.status);
                                    }
                                })
               .getStatus()
               .isOK());

    ASSERT_OK(pf.future.getNoThrow());
}

TEST_F(ScopedTaskExecutorTest, scheduleRemoteCommand) {
    auto pf = makePromiseFuture<void>();

    scheduleRemoteCommand(pf.promise);
    {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        ASSERT(getNet()->hasReadyRequests());
        getNet()->scheduleSuccessfulResponse(BSONObj());
        getNet()->runReadyNetworkOperations();
    }

    ASSERT_OK(pf.future.getNoThrow());
}

// Fully run the callback before capturing the callback handle
TEST_F(ScopedTaskExecutorTest, scheduleLoseRaceWithSuccess) {
    auto resultPf = makePromiseFuture<void>();
    auto schedulePf = makePromiseFuture<void>();

    auto& fp = ScopedTaskExecutorHangAfterSchedule;
    fp.setMode(FailPoint::alwaysOn);

    stdx::thread scheduler([&] {
        scheduleWork(resultPf.promise);
        schedulePf.promise.emplaceValue();
    });

    ASSERT_OK(resultPf.future.getNoThrow());
    ASSERT_FALSE(schedulePf.future.isReady());

    fp.setMode(FailPoint::off);
    ASSERT_OK(schedulePf.future.getNoThrow());

    scheduler.join();
}

// Stash the handle before running the callback
TEST_F(ScopedTaskExecutorTest, scheduleWinRaceWithSuccess) {
    auto resultPf = makePromiseFuture<void>();
    auto schedulePf = makePromiseFuture<void>();

    getNet()->enterNetwork();

    stdx::thread scheduler([&] {
        scheduleWork(resultPf.promise);
        schedulePf.promise.emplaceValue();
    });

    ASSERT_OK(schedulePf.future.getNoThrow());

    ASSERT_FALSE(resultPf.future.isReady());

    getNet()->exitNetwork();

    ASSERT_OK(resultPf.future.getNoThrow());

    scheduler.join();
}

// Schedule on the underlying, but are shut down when we execute our wrapping callback
TEST_F(ScopedTaskExecutorTest, scheduleLoseRaceWithShutdown) {
    auto resultPf = makePromiseFuture<void>();
    auto schedulePf = makePromiseFuture<void>();

    getNet()->enterNetwork();

    scheduleWork(resultPf.promise);

    ASSERT_FALSE(resultPf.future.isReady());

    getExecutor()->shutdown();
    getNet()->exitNetwork();

    ASSERT_EQUALS(resultPf.future.getNoThrow(), ErrorCodes::ShutdownInProgress);
}

// ScheduleRemoteCommand on the underlying, but are shut down when we execute our wrapping callback
TEST_F(ScopedTaskExecutorTest, scheduleRemoteCommandLoseRaceWithShutdown) {
    auto pf = makePromiseFuture<void>();

    scheduleRemoteCommand(pf.promise);
    {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        ASSERT(getNet()->hasReadyRequests());
        getNet()->scheduleSuccessfulResponse(BSONObj());
        getExecutor()->shutdown();
        getNet()->runReadyNetworkOperations();
    }

    ASSERT_EQUALS(pf.future.getNoThrow(), ErrorCodes::ShutdownInProgress);
}

// Fail to schedule on the underlying
TEST_F(ScopedTaskExecutorTest, scheduleLoseRaceWithShutdownOfUnderlying) {
    auto& bfp = ScopedTaskExecutorHangBeforeSchedule;
    auto& efp = ScopedTaskExecutorHangExitBeforeSchedule;
    bfp.setMode(FailPoint::alwaysOn);
    efp.setMode(FailPoint::alwaysOn);

    stdx::thread scheduler([&] {
        ASSERT_FALSE(
            getExecutor()
                ->scheduleWork([&](const TaskExecutor::CallbackArgs& ca) { MONGO_UNREACHABLE; })
                .getStatus()
                .isOK());
    });

    MONGO_FAIL_POINT_PAUSE_WHILE_SET((bfp));

    shutdownUnderlying();

    efp.setMode(FailPoint::off);

    scheduler.join();
}

TEST_F(ScopedTaskExecutorTest, DestructionShutsDown) {
    auto pf = makePromiseFuture<void>();

    {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

        scheduleWork(pf.promise);

        ASSERT_FALSE(pf.future.isReady());

        resetExecutor();

        ASSERT_FALSE(pf.future.isReady());
    }

    ASSERT_EQUALS(pf.future.getNoThrow(), ErrorCodes::ShutdownInProgress);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
