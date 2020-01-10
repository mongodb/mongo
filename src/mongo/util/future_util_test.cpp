/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/util/future_util.h"

namespace mongo {
namespace {

class AsyncTryUntilTest : public unittest::Test {
public:
    void setUp() override {
        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _network = network.get();

        _executor = makeSharedThreadPoolTestExecutor(std::move(network));
        _executor->startup();
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
        _executor.reset();
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> executor() {
        return _executor;
    }

    executor::NetworkInterfaceMock* network() {
        return _network;
    }

private:
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    executor::NetworkInterfaceMock* _network;
};

TEST_F(AsyncTryUntilTest, LoopExecutesOnceWithAlwaysTrueCondition) {
    auto i = 0;
    auto resultFut = AsyncTry([&] { ++i; }).until([](Status s) { return true; }).on(executor());
    resultFut.wait();

    ASSERT_EQ(i, 1);
}

TEST_F(AsyncTryUntilTest, LoopExecutesUntilConditionIsTrue) {
    const int numLoops = 3;
    auto i = 0;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return i;
                     })
                         .until([&](StatusWith<int> swInt) { return swInt.getValue() == numLoops; })
                         .on(executor());
    resultFut.wait();

    ASSERT_EQ(i, numLoops);
}

TEST_F(AsyncTryUntilTest, LoopDoesNotRespectDelayIfConditionIsAlreadyTrue) {
    auto i = 0;
    auto resultFut = AsyncTry([&] { ++i; })
                         .until([](Status s) { return true; })
                         .withDelayBetweenIterations(Seconds(10000000))
                         .on(executor());
    // This would hang for a very long time if the behavior were incorrect.
    resultFut.wait();

    ASSERT_EQ(i, 1);
}

TEST_F(AsyncTryUntilTest, LoopRespectsDelayAfterEvaluatingCondition) {
    const int numLoops = 2;
    auto i = 0;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return i;
                     })
                         .until([&](StatusWith<int> swInt) { return swInt.getValue() == numLoops; })
                         .withDelayBetweenIterations(Seconds(1000))
                         .on(executor());
    ASSERT_FALSE(resultFut.isReady());

    // Advance the time some, but not enough to be past the delay yet.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + Seconds{100});
    }

    ASSERT_FALSE(resultFut.isReady());

    // Advance the time past the delay.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + Seconds{2000});
    }

    resultFut.wait();

    ASSERT_EQ(i, numLoops);
}

TEST_F(AsyncTryUntilTest, LoopBodyPropagatesValueOfLastIterationToCaller) {
    auto i = 0;
    auto expectedResult = 3;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return i;
                     })
                         .until([&](StatusWith<int> swInt) { return i == expectedResult; })
                         .on(executor());

    ASSERT_EQ(resultFut.get(), expectedResult);
}

TEST_F(AsyncTryUntilTest, LoopBodyPropagatesErrorToConditionAndCaller) {
    auto resultFut = AsyncTry([&] {
                         uasserted(ErrorCodes::InternalError, "test error");
                         return 3;
                     })
                         .until([&](StatusWith<int> swInt) {
                             ASSERT_NOT_OK(swInt);
                             ASSERT_EQ(swInt.getStatus().code(), ErrorCodes::InternalError);
                             return true;
                         })
                         .on(executor());

    ASSERT_EQ(resultFut.getNoThrow(), ErrorCodes::InternalError);
}
}  // namespace
}  // namespace mongo
