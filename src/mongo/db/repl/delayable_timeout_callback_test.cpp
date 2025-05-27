/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/repl/delayable_timeout_callback.h"

#include "mongo/base/string_data.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
template <typename T>
class DelayableTimeoutCallbackBaseTest : public unittest::Test {
protected:
    void setUp() override {
        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _net = network.get();
        _executor = makeThreadPoolTestExecutor(std::move(network));
        _executor->startup();
        createDelayableTimeoutCallback();
    }

    void tearDown() override {
        _delayableTimeoutCallback = boost::none;
        _executor->shutdown();
        _executor->join();
        _executor.reset();
    }

    void callback(const mongo::executor::TaskExecutor::CallbackArgs& cbData) {
        callbackRan++;
        if (!cbData.status.isOK()) {
            callbackRanWithError++;
        }
    }

    void createDelayableTimeoutCallback() {
        MONGO_UNREACHABLE;
    }


protected:
    mutable stdx::mutex _mutex;
    boost::optional<T> _delayableTimeoutCallback;
    executor::NetworkInterfaceMock* _net;
    std::shared_ptr<executor::TaskExecutor> _executor;
    int callbackRan = 0;
    int callbackRanWithError = 0;
};

template <>
void DelayableTimeoutCallbackBaseTest<DelayableTimeoutCallback>::createDelayableTimeoutCallback() {
    _delayableTimeoutCallback.emplace(
        _executor.get(), [this](const mongo::executor::TaskExecutor::CallbackArgs& cbData) {
            this->callback(cbData);
        });
}

template <>
void DelayableTimeoutCallbackBaseTest<
    DelayableTimeoutCallbackWithJitter>::createDelayableTimeoutCallback() {
    _delayableTimeoutCallback.emplace(
        _executor.get(),
        [this](const mongo::executor::TaskExecutor::CallbackArgs& cbData) {
            this->callback(cbData);
        },
        [](WithLock, int64_t limit) {
            static int64_t notVeryRandom = 0;
            notVeryRandom += 10;
            return notVeryRandom % limit;
        });
}

typedef DelayableTimeoutCallbackBaseTest<DelayableTimeoutCallback> DelayableTimeoutCallbackTest;
typedef DelayableTimeoutCallbackBaseTest<DelayableTimeoutCallbackWithJitter>
    DelayableTimeoutCallbackWithJitterTest;

TEST_F(DelayableTimeoutCallbackTest, ScheduleAtSchedulesFirstCallback) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());

    ASSERT_OK(_delayableTimeoutCallback->scheduleAt(_net->now() + Seconds(2)));

    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_EQ(1, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackTest, DelayUntilSchedulesFirstCallback) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());

    ASSERT_OK(_delayableTimeoutCallback->delayUntil(_net->now() + Seconds(2)));

    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_EQ(1, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackTest, ScheduleAtMovesCallbackLater) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());

    ASSERT_OK(_delayableTimeoutCallback->scheduleAt(_net->now() + Seconds(2)));

    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);

    ASSERT_OK(_delayableTimeoutCallback->scheduleAt(_net->now() + Seconds(2)));

    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_EQ(1, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackTest, DelayUntilMovesCallbackLater) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());

    ASSERT_OK(_delayableTimeoutCallback->delayUntil(_net->now() + Seconds(2)));

    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);

    ASSERT_OK(_delayableTimeoutCallback->delayUntil(_net->now() + Seconds(2)));

    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_EQ(1, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackTest, ScheduleAtMovesCallbackEarlier) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());

    ASSERT_OK(_delayableTimeoutCallback->scheduleAt(_net->now() + Seconds(3)));

    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);

    ASSERT_OK(_delayableTimeoutCallback->scheduleAt(_net->now() + Seconds(1)));

    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_EQ(1, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackTest, DelayUntilDoesNotMoveCallbackEarlier) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());

    ASSERT_OK(_delayableTimeoutCallback->delayUntil(_net->now() + Seconds(3)));

    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);

    ASSERT_OK(_delayableTimeoutCallback->delayUntil(_net->now() + Seconds(1)));

    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_EQ(1, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackTest, ScheduleAtInPastRunsImmediately) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    // Make sure there's a past to schedule in.
    _net->runUntil(_net->now() + Days(1));

    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
    ASSERT_OK(_delayableTimeoutCallback->scheduleAt(_net->now() - Seconds(1)));

    // Needed to trigger anything scheduled.
    _net->runReadyNetworkOperations();

    ASSERT_EQ(1, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackTest, DelayUntilInPastRunsImmediately) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    // Make sure there's a past to schedule in.
    _net->runUntil(_net->now() + Days(1));

    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
    ASSERT_OK(_delayableTimeoutCallback->delayUntil(_net->now() - Seconds(1)));

    // Needed to trigger anything scheduled.
    _net->runReadyNetworkOperations();

    ASSERT_EQ(1, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackTest, Cancellation) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());

    ASSERT_OK(_delayableTimeoutCallback->scheduleAt(_net->now() + Seconds(2)));

    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    _delayableTimeoutCallback->cancel();
    ASSERT_EQ(0, callbackRan);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_EQ(0, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackTest, Shutdown) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());

    ASSERT_OK(_delayableTimeoutCallback->scheduleAt(_net->now() + Seconds(2)));

    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(0, callbackRan);
    _net->runUntil(_net->now() + Seconds(1));
    ASSERT_TRUE(_delayableTimeoutCallback->isActive());

    _executor->shutdown();

    // This makes sure the executor processes the shutdown.
    _net->runReadyNetworkOperations();

    ASSERT_EQ(0, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackWithJitterTest, DelayUntilWithJitter) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());

    stdx::lock_guard lk(_mutex);
    ASSERT_OK(_delayableTimeoutCallback->delayUntilWithJitter(
        lk, _net->now() + Seconds(10), Milliseconds(100)));

    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    // Our "random" generator is just 10,20,30,...
    ASSERT_EQ(_net->now() + Milliseconds(10010), _delayableTimeoutCallback->getNextCall());
    ASSERT_EQ(0, callbackRan);

    // Setting it again in the same time shouldn't change jitter.
    ASSERT_OK(_delayableTimeoutCallback->delayUntilWithJitter(
        lk, _net->now() + Seconds(10), Milliseconds(100)));
    ASSERT_EQ(_net->now() + Milliseconds(10010), _delayableTimeoutCallback->getNextCall());
    ASSERT_EQ(0, callbackRan);

    // Move forward less than the max jitter shouldn't change jitter.
    for (int i = 0; i < 3; i++) {
        _net->runUntil(_net->now() + Milliseconds(25));
        ASSERT_OK(_delayableTimeoutCallback->delayUntilWithJitter(
            lk, _net->now() + Seconds(10), Milliseconds(100)));
        ASSERT_EQ(_net->now() + Milliseconds(10010), _delayableTimeoutCallback->getNextCall());
        ASSERT_EQ(0, callbackRan);
    }

    // Move forward to the max jitter should recalculate jitter.
    _net->runUntil(_net->now() + Milliseconds(25));
    ASSERT_OK(_delayableTimeoutCallback->delayUntilWithJitter(
        lk, _net->now() + Seconds(10), Milliseconds(100)));
    ASSERT_EQ(_net->now() + Milliseconds(10020), _delayableTimeoutCallback->getNextCall());
    ASSERT_EQ(0, callbackRan);

    // Setting max jitter to less than actual jitter should recalculate jitter.
    ASSERT_OK(_delayableTimeoutCallback->delayUntilWithJitter(
        lk, _net->now() + Seconds(10), Milliseconds(19)));
    // Jitter value will be 30 % 19 = 11
    ASSERT_EQ(_net->now() + Milliseconds(10011), _delayableTimeoutCallback->getNextCall());
    ASSERT_EQ(0, callbackRan);

    _net->runUntil(_net->now() + Milliseconds(10011));
    ASSERT_EQ(1, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

TEST_F(DelayableTimeoutCallbackWithJitterTest, DelayUntilWithZeroJitter) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());

    stdx::lock_guard lk(_mutex);

    ASSERT_OK(
        _delayableTimeoutCallback->delayUntilWithJitter(lk, _net->now() + Seconds(10), Seconds(0)));

    ASSERT_TRUE(_delayableTimeoutCallback->isActive());
    ASSERT_EQ(_net->now() + Milliseconds(10000), _delayableTimeoutCallback->getNextCall());
    ASSERT_EQ(0, callbackRan);

    _net->runUntil(_net->now() + Milliseconds(10000));
    ASSERT_EQ(1, callbackRan);
    ASSERT_EQ(0, callbackRanWithError);
    ASSERT_FALSE(_delayableTimeoutCallback->isActive());
}

}  // namespace repl
}  // namespace mongo
