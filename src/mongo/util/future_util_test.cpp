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

#include "mongo/util/future_util.h"

#include "mongo/base/string_data.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/cancellation.h"

#include <algorithm>
#include <functional>
#include <random>
#include <ratio>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

class FutureUtilTest : public unittest::Test {
public:
    void setUp() override {
        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _network = network.get();

        _executor = makeThreadPoolTestExecutor(std::move(network));
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

class TestBackoff {
public:
    TestBackoff(Milliseconds initialSleep) : _initialSleep(initialSleep), _lastSleep(0) {}

    Milliseconds nextSleep() {
        _lastSleep = _lastSleep == Milliseconds{0} ? _initialSleep : _lastSleep * 2;
        return _lastSleep;
    }

private:
    Milliseconds _initialSleep;
    Milliseconds _lastSleep;
};

using AsyncTryUntilTest = FutureUtilTest;

TEST_F(AsyncTryUntilTest, LoopExecutesOnceWithAlwaysTrueCondition) {
    auto i = 0;
    auto resultFut = AsyncTry([&] {
                         ++i;
                     }).until([](Status s) {
                           return true;
                       }).on(executor(), CancellationToken::uncancelable());
    resultFut.wait();

    ASSERT_EQ(i, 1);
}

TEST_F(AsyncTryUntilTest, LoopDoesNotExecuteIfExecutorAlreadyShutdown) {
    executor()->shutdown();

    auto i = 0;
    auto resultFut = AsyncTry([&] {
                         ++i;
                     }).until([](Status s) {
                           return true;
                       }).on(executor(), CancellationToken::uncancelable());

    ASSERT_THROWS_CODE(resultFut.get(), DBException, ErrorCodes::ShutdownInProgress);

    ASSERT_EQ(i, 0);
}

TEST_F(AsyncTryUntilTest, LoopDoesNotReturnBrokenPromiseIfExecutorShutdownWhileLoopBodyExecutes) {
    unittest::Barrier barrierBeforeShutdown(2);
    unittest::Barrier barrierAfterShutdown(2);
    auto resultFut = AsyncTry([&] {
                         barrierBeforeShutdown.countDownAndWait();
                         barrierAfterShutdown.countDownAndWait();
                     }).until([](Status) {
                           return false;
                       }).on(executor(), CancellationToken::uncancelable());
    barrierBeforeShutdown.countDownAndWait();
    executor()->shutdown();
    barrierAfterShutdown.countDownAndWait();

    ASSERT_THROWS_CODE(resultFut.get(), DBException, ErrorCodes::ShutdownInProgress);
}

TEST_F(AsyncTryUntilTest,
       LoopWithDelayDoesNotReturnBrokenPromiseIfExecutorShutdownWhileLoopBodyExecutes) {
    unittest::Barrier barrierBeforeShutdown(2);
    unittest::Barrier barrierAfterShutdown(2);
    auto resultFut = AsyncTry([&] {
                         barrierBeforeShutdown.countDownAndWait();
                         barrierAfterShutdown.countDownAndWait();
                     })
                         .until([](Status) { return false; })
                         .withDelayBetweenIterations(Milliseconds(10))
                         .on(executor(), CancellationToken::uncancelable());
    barrierBeforeShutdown.countDownAndWait();
    executor()->shutdown();
    barrierAfterShutdown.countDownAndWait();

    ASSERT_THROWS_CODE(resultFut.get(), DBException, ErrorCodes::ShutdownInProgress);
}

TEST_F(AsyncTryUntilTest, LoopWithDelayDoesNotExecuteIfExecutorAlreadyShutdown) {
    executor()->shutdown();

    auto i = 0;
    auto resultFut = AsyncTry([&] { ++i; })
                         .until([](Status s) { return true; })
                         .withDelayBetweenIterations(Milliseconds(0))
                         .on(executor(), CancellationToken::uncancelable());

    ASSERT_THROWS_CODE(resultFut.get(), DBException, ErrorCodes::ShutdownInProgress);

    ASSERT_EQ(i, 0);
}

TEST_F(AsyncTryUntilTest, LoopExecutesUntilConditionIsTrue) {
    const int numLoops = 3;
    auto i = 0;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return i;
                     }).until([&](StatusWith<int> swInt) {
                           return swInt.getValue() == numLoops;
                       }).on(executor(), CancellationToken::uncancelable());
    resultFut.wait();

    ASSERT_EQ(i, numLoops);
}

TEST_F(AsyncTryUntilTest, LoopExecutesUntilConditionIsTrueWithFutureReturnType) {
    const int numLoops = 3;
    auto i = 0;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return Future<int>::makeReady(i);
                     }).until([&](StatusWith<int> swInt) {
                           return swInt.getValue() == numLoops;
                       }).on(executor(), CancellationToken::uncancelable());
    resultFut.wait();

    ASSERT_EQ(i, numLoops);
}

TEST_F(AsyncTryUntilTest, LoopExecutesUntilConditionIsTrueWithSemiFutureReturnType) {
    const int numLoops = 3;
    auto i = 0;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return SemiFuture<int>::makeReady(i);
                     }).until([&](StatusWith<int> swInt) {
                           return swInt.getValue() == numLoops;
                       }).on(executor(), CancellationToken::uncancelable());
    resultFut.wait();

    ASSERT_EQ(i, numLoops);
}

TEST_F(AsyncTryUntilTest, LoopExecutesUntilConditionIsTrueWithExecutorFutureReturnType) {
    const int numLoops = 3;
    auto i = 0;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return ExecutorFuture<int>(executor(), i);
                     }).until([&](StatusWith<int> swInt) {
                           return swInt.getValue() == numLoops;
                       }).on(executor(), CancellationToken::uncancelable());
    resultFut.wait();

    ASSERT_EQ(i, numLoops);
}

TEST_F(AsyncTryUntilTest, LoopDoesNotRespectConstDelayIfConditionIsAlreadyTrue) {
    auto i = 0;
    auto resultFut = AsyncTry([&] { ++i; })
                         .until([](Status s) { return true; })
                         .withDelayBetweenIterations(Seconds(10000000))
                         .on(executor(), CancellationToken::uncancelable());
    // This would hang for a very long time if the behavior were incorrect.
    resultFut.wait();

    ASSERT_EQ(i, 1);
}

TEST_F(AsyncTryUntilTest, LoopDoesNotRespectBackoffDelayIfConditionIsAlreadyTrue) {
    auto i = 0;
    auto resultFut = AsyncTry([&] { ++i; })
                         .until([](Status s) { return true; })
                         .withBackoffBetweenIterations(TestBackoff{Seconds(10000000)})
                         .on(executor(), CancellationToken::uncancelable());
    // This would hang for a very long time if the behavior were incorrect.
    resultFut.wait();

    ASSERT_EQ(i, 1);
}

TEST_F(AsyncTryUntilTest, LoopRespectsConstDelayAfterEvaluatingCondition) {
    const int numLoops = 2;
    auto i = 0;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return i;
                     })
                         .until([&](StatusWith<int> swInt) { return swInt.getValue() == numLoops; })
                         .withDelayBetweenIterations(Seconds(1000))
                         .on(executor(), CancellationToken::uncancelable());
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
        network()->advanceTime(network()->now() + Seconds{1000});
    }

    resultFut.wait();

    ASSERT_EQ(i, numLoops);
}

TEST_F(AsyncTryUntilTest, LoopRespectsBackoffDelayAfterEvaluatingCondition) {
    const int numLoops = 3;
    auto i = 0;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return i;
                     })
                         .until([&](StatusWith<int> swInt) { return swInt.getValue() == numLoops; })
                         .withBackoffBetweenIterations(TestBackoff{Seconds(1000)})
                         .on(executor(), CancellationToken::uncancelable());
    ASSERT_FALSE(resultFut.isReady());

    // Due to the backoff, the delays are going to be 1000 seconds and 2000 seconds.
    // Advance the time some, but not enough to be past the first delay.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + Seconds{100});
    }

    ASSERT_EQ(i, 1);
    ASSERT_FALSE(resultFut.isReady());

    // Advance the time past the first delay.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + Seconds{1000});
    }

    ASSERT_EQ(i, 2);
    ASSERT_FALSE(resultFut.isReady());

    // Advance the time some more, but not enough to be past the second delay.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + Seconds{1000});
    }

    ASSERT_EQ(i, 2);
    ASSERT_FALSE(resultFut.isReady());

    // Advance the time past the second delay.
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + Seconds{1000});
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
                     }).until([&](StatusWith<int> swInt) {
                           return i == expectedResult;
                       }).on(executor(), CancellationToken::uncancelable());

    ASSERT_EQ(resultFut.get(), expectedResult);
}

TEST_F(AsyncTryUntilTest, FutureReturningLoopBodyPropagatesValueOfLastIterationToCaller) {
    auto i = 0;
    auto expectedResult = 3;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return Future<int>::makeReady(i);
                     }).until([&](StatusWith<int> swInt) {
                           return i == expectedResult;
                       }).on(executor(), CancellationToken::uncancelable());

    ASSERT_EQ(resultFut.get(), expectedResult);
}

TEST_F(AsyncTryUntilTest, SemiFutureReturningLoopBodyPropagatesValueOfLastIterationToCaller) {
    auto i = 0;
    auto expectedResult = 3;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return SemiFuture<int>::makeReady(i);
                     }).until([&](StatusWith<int> swInt) {
                           return i == expectedResult;
                       }).on(executor(), CancellationToken::uncancelable());

    ASSERT_EQ(resultFut.get(), expectedResult);
}

TEST_F(AsyncTryUntilTest, ExecutorFutureReturningLoopBodyPropagatesValueOfLastIterationToCaller) {
    auto i = 0;
    auto expectedResult = 3;
    auto resultFut = AsyncTry([&] {
                         ++i;
                         return ExecutorFuture<int>(executor(), i);
                     }).until([&](StatusWith<int> swInt) {
                           return i == expectedResult;
                       }).on(executor(), CancellationToken::uncancelable());

    ASSERT_EQ(resultFut.get(), expectedResult);
}

void doTestWithError(auto&& executor, const auto& bodyReturn) {
    unittest::threadAssertionMonitoredTest([&](auto& assertionMonitor) {
        auto resultFut = AsyncTry([&] {
                             uasserted(ErrorCodes::InternalError, "test error");
                             return bodyReturn();
                         })
                             .until([&](StatusWith<int> swInt) {
                                 assertionMonitor.exec([&] {
                                     ASSERT_NOT_OK(swInt);
                                     ASSERT_EQ(swInt.getStatus().code(), ErrorCodes::InternalError);
                                 });
                                 return true;
                             })
                             .on(executor, CancellationToken::uncancelable());

        ASSERT_EQ(resultFut.getNoThrow(), ErrorCodes::InternalError);
    });
}

TEST_F(AsyncTryUntilTest, LoopBodyPropagatesErrorToConditionAndCaller) {
    doTestWithError(executor(), [] { return 3; });
}

TEST_F(AsyncTryUntilTest, FutureReturningLoopBodyPropagatesErrorToConditionAndCaller) {
    doTestWithError(executor(), [] { return Future<int>::makeReady(3); });
}

TEST_F(AsyncTryUntilTest, SemiFutureReturningLoopBodyPropagatesErrorToConditionAndCaller) {
    doTestWithError(executor(), [] { return SemiFuture<int>::makeReady(3); });
}

TEST_F(AsyncTryUntilTest, ExecutorFutureReturningLoopBodyPropagatesErrorToConditionAndCaller) {
    doTestWithError(executor(), [&] { return ExecutorFuture<int>(executor(), 3); });
}

static const Status kCanceledStatus = {ErrorCodes::CallbackCanceled, "AsyncTry::until canceled"};

TEST_F(AsyncTryUntilTest, AsyncTryUntilCanBeCanceled) {
    CancellationSource cancelSource;
    auto resultFut =
        AsyncTry([] {}).until([](Status) { return false; }).on(executor(), cancelSource.token());
    // This should hang forever if it is not canceled.
    cancelSource.cancel();
    ASSERT_EQ(resultFut.getNoThrow(), kCanceledStatus);
}

TEST_F(AsyncTryUntilTest, AsyncTryUntilWithDelayCanBeCanceledWhileLoopBodyIsExecuting) {
    CancellationSource cancelSource;
    unittest::Barrier barrierBeforeCancel{2}, barrierAfterCancel{2};
    int timesRanCallback = 0;
    // Arbitrary delay used, enforce only one loop body execution with timesRanCallback
    auto resultFut = AsyncTry([&] {
                         timesRanCallback += 1;
                         barrierBeforeCancel.countDownAndWait();
                         barrierAfterCancel.countDownAndWait();
                     })
                         .until([](Status) { return false; })
                         .withDelayBetweenIterations(Milliseconds(10))
                         .on(executor(), cancelSource.token());
    // Enforce cancellation during loop body execution
    barrierBeforeCancel.countDownAndWait();
    cancelSource.cancel();
    barrierAfterCancel.countDownAndWait();
    ASSERT_EQ(resultFut.getNoThrow(), kCanceledStatus);
    ASSERT_EQ(timesRanCallback, 1);
}

TEST_F(AsyncTryUntilTest, AsyncTryUntilWithDelayCanBeCanceledAfterLoopBodyIsDoneExecuting) {
    CancellationSource cancelSource;
    unittest::Barrier barrierBeforeCancel{2}, barrierAfterCancel{2};
    int timesRanCallback = 0;
    auto resultFut = AsyncTry([&] { timesRanCallback += 1; })
                         .until([&](Status) {
                             barrierBeforeCancel.countDownAndWait();
                             barrierAfterCancel.countDownAndWait();
                             return false;
                         })
                         .withDelayBetweenIterations(Milliseconds(10))
                         .on(executor(), cancelSource.token());
    // Enforce cancellation after loop body executes once
    barrierBeforeCancel.countDownAndWait();
    cancelSource.cancel();
    barrierAfterCancel.countDownAndWait();
    ASSERT_EQ(resultFut.getNoThrow(), kCanceledStatus);
    ASSERT_EQ(timesRanCallback, 1);
}

TEST_F(AsyncTryUntilTest, AsyncTryUntilWithBackoffCanBeCanceledWhileLoopBodyIsExecuting) {
    CancellationSource cancelSource;
    unittest::Barrier barrierBeforeCancel{2}, barrierAfterCancel{2};
    int timesRanCallback = 0;
    auto resultFut = AsyncTry([&] {
                         timesRanCallback += 1;
                         barrierBeforeCancel.countDownAndWait();
                         barrierAfterCancel.countDownAndWait();
                     })
                         .until([](Status) { return false; })
                         .withBackoffBetweenIterations(TestBackoff{Milliseconds(10)})
                         .on(executor(), cancelSource.token());
    barrierBeforeCancel.countDownAndWait();
    cancelSource.cancel();
    barrierAfterCancel.countDownAndWait();
    ASSERT_EQ(resultFut.getNoThrow(), kCanceledStatus);
    ASSERT_EQ(timesRanCallback, 1);
}

TEST_F(AsyncTryUntilTest, AsyncTryUntilWithBackoffCanBeCanceledAfterLoopBodyIsDoneExecuting) {
    CancellationSource cancelSource;
    unittest::Barrier barrierBeforeCancel{2}, barrierAfterCancel{2};
    int timesRanCallback = 0;
    auto resultFut = AsyncTry([&] { timesRanCallback += 1; })
                         .until([&](Status) {
                             barrierBeforeCancel.countDownAndWait();
                             barrierAfterCancel.countDownAndWait();
                             return false;
                         })
                         .withBackoffBetweenIterations(TestBackoff{Milliseconds(10)})
                         .on(executor(), cancelSource.token());
    barrierBeforeCancel.countDownAndWait();
    cancelSource.cancel();
    barrierAfterCancel.countDownAndWait();
    ASSERT_EQ(resultFut.getNoThrow(), kCanceledStatus);
    ASSERT_EQ(timesRanCallback, 1);
}

TEST_F(AsyncTryUntilTest, CanceledTryUntilLoopDoesNotExecuteIfAlreadyCanceled) {
    int counter{0};
    CancellationSource cancelSource;
    auto canceledToken = cancelSource.token();
    cancelSource.cancel();
    auto resultFut = AsyncTry([&] {
                         ++counter;
                     }).until([](Status) {
                           return false;
                       }).on(executor(), canceledToken);
    ASSERT_EQ(resultFut.getNoThrow(), kCanceledStatus);
    ASSERT_EQ(counter, 0);
}

TEST_F(AsyncTryUntilTest, CanceledTryUntilLoopWithDelayDoesNotExecuteIfAlreadyCanceled) {
    CancellationSource cancelSource;
    int counter{0};
    auto canceledToken = cancelSource.token();
    cancelSource.cancel();
    auto resultFut = AsyncTry([&] { ++counter; })
                         .until([](Status) { return false; })
                         .withDelayBetweenIterations(Hours(1000))
                         .on(executor(), canceledToken);
    ASSERT_EQ(resultFut.getNoThrow(), kCanceledStatus);
    ASSERT_EQ(counter, 0);
}

TEST_F(AsyncTryUntilTest, CanceledTryUntilLoopWithBackoffDoesNotExecuteIfAlreadyCanceled) {
    CancellationSource cancelSource;
    int counter{0};
    auto canceledToken = cancelSource.token();
    cancelSource.cancel();
    auto resultFut = AsyncTry([&] { ++counter; })
                         .until([](Status) { return false; })
                         .withBackoffBetweenIterations(TestBackoff{Seconds(10000000)})
                         .on(executor(), canceledToken);
    ASSERT_EQ(resultFut.getNoThrow(), kCanceledStatus);
    ASSERT_EQ(counter, 0);
}

TEST_F(AsyncTryUntilTest, UntilBodyPropagatesErrorToCaller) {
    const auto error = Status(ErrorCodes::InternalError, "Some error");
    auto resultFut = AsyncTry([] {
                     }).until([&](Status status) -> bool {
                           iasserted(error);
                       }).on(executor(), CancellationToken::uncancelable());
    ASSERT_EQ(resultFut.getNoThrow(), error);
}

TEST_F(AsyncTryUntilTest, UntilWithDelayBodyPropagatesErrorToCaller) {
    const auto error = Status(ErrorCodes::InternalError, "Some error");
    auto resultFut = AsyncTry([] {})
                         .until([&](Status status) -> bool { iasserted(error); })
                         .withDelayBetweenIterations(Seconds(10))
                         .on(executor(), CancellationToken::uncancelable());
    ASSERT_EQ(resultFut.getNoThrow(), error);
}

TEST_F(AsyncTryUntilTest, MoveOnlyType) {
    using MoveOnly = std::unique_ptr<int>;

    AsyncTry([this] { return ExecutorFuture(executor(), MoveOnly{}); })
        .until([](const StatusWith<MoveOnly>& swResult) {
            // Access the move-only result via a const reference.
            return swResult.isOK();
        })
        .on(executor(), CancellationToken::uncancelable())
        .getAsync([](StatusWith<MoveOnly>) {
            // Consume the (move-only) result.
        });
}

template <typename T>
std::pair<std::vector<Promise<T>>, std::vector<Future<T>>> makePromisesAndFutures(size_t size) {
    std::vector<Future<T>> inputFutures;
    std::vector<Promise<T>> inputPromises;
    for (size_t i = 0; i < size; ++i) {
        auto [inputPromise, inputFuture] = makePromiseFuture<T>();
        inputFutures.emplace_back(std::move(inputFuture));
        inputPromises.emplace_back(std::move(inputPromise));
    }
    return std::make_pair(std::move(inputPromises), std::move(inputFutures));
}

class WhenAllSucceedTest : public FutureUtilTest {};

static const Status kErrorStatus = {ErrorCodes::InternalError, ""};

DEATH_TEST_F(WhenAllSucceedTest,
             WhenAllSucceedFailsWithInputVectorOfSizeZero,
             future_util_details::kWhenAllSucceedEmptyInputInvariantMsg) {
    std::vector<Future<int>> inputFutures;
    std::ignore = whenAllSucceed(std::move(inputFutures));
}

TEST_F(WhenAllSucceedTest, WhenAllSucceedReturnsErrorOnFirstErrorWhenOneInputFuture) {
    std::vector<Future<int>> inputFutures;
    auto [inputPromise, inputFuture] = makePromiseFuture<int>();
    inputFutures.emplace_back(std::move(inputFuture));
    auto result = whenAllSucceed(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());
    inputPromise.setError(kErrorStatus);
    ASSERT_EQ(result.getNoThrow().getStatus(), kErrorStatus);
}

TEST_F(WhenAllSucceedTest,
       WhenAllSucceedReturnsErrorOnFirstErrorWhenOthersUnresolvedAndSucceedAfter) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    inputPromises[0].setError(kErrorStatus);
    ASSERT_EQ(result.getNoThrow().getStatus(), kErrorStatus);

    // Finish resolving remaining with success.
    const auto kValue = 5;
    // Start at 1 since the 0'th input was already set with an error.
    for (auto i = 1; i < kNumInputs; ++i) {
        inputPromises[i].emplaceValue(kValue);
    }
}

TEST_F(WhenAllSucceedTest, WhenAllSucceedIgnoresErrorAfterFirstError) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    inputPromises[0].setError(kErrorStatus);
    ASSERT_EQ(result.getNoThrow().getStatus(), kErrorStatus);

    // Finish resolving remaining with an error. Start at 1 since the 0'th input was already set
    // with an error. It's not strictly necessary to do this explicitly since they'd fail with
    // BrokenPromise anyways when the promises are destroyed, but this makes the test case more
    // clear.
    for (auto i = 1; i < kNumInputs; ++i) {
        inputPromises[i].setError(kErrorStatus);
    }
}

TEST_F(WhenAllSucceedTest, WhenAllSucceedReturnsErrorOnFirstErrorWhenAllOthersAreResolved) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures));

    // Emplace all but the last promise with a value.
    const auto kValue = 5;
    for (auto i = 1; i < kNumInputs - 1; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue(kValue);
    }
    ASSERT_FALSE(result.isReady());

    // Set an error on the last input.
    inputPromises[kNumInputs - 1].setError(kErrorStatus);
    ASSERT_EQ(result.getNoThrow().getStatus(), kErrorStatus);
}

TEST_F(WhenAllSucceedTest, WhenAllSucceedReturnsAfterSuccessfulResponseWithOneInputFuture) {
    std::vector<Future<int>> inputFutures;
    auto [inputPromise, inputFuture] = makePromiseFuture<int>();
    inputFutures.emplace_back(std::move(inputFuture));
    auto result = whenAllSucceed(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());
    const auto kValue = 5;
    inputPromise.emplaceValue(kValue);
    auto outputValues = result.get();
    ASSERT_EQ(outputValues.size(), 1);
    ASSERT_EQ(outputValues[0], kValue);
}

TEST_F(WhenAllSucceedTest, WhenAllSucceedReturnsAfterLastSuccessfulResponseWithManyInputFutures) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures));

    const auto kValue = 5;
    // Emplace success on all input futures. The result should not be ready
    // until the last one is emplaced.
    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue(kValue);
    }

    auto outputValues = result.get();
    ASSERT_EQ(outputValues.size(), kNumInputs);
    for (auto v : outputValues) {
        ASSERT_EQ(v, kValue);
    }
}

TEST_F(WhenAllSucceedTest, WhenAllSucceedMaintainsOrderingOfInputFutures) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures));

    // Create a random order of indexes in which to resolve the input futures.
    std::vector<int> ordering;
    for (auto i = 0; i < kNumInputs; ++i) {
        ordering.push_back(i);
    }
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(ordering.begin(), ordering.end(), g);

    for (auto idx : ordering) {
        ASSERT_FALSE(result.isReady());
        inputPromises[idx].emplaceValue(idx);
    }

    auto outputValues = result.get();
    ASSERT_EQ(outputValues.size(), kNumInputs);

    // The output should be in the same order as the input, regardless of the
    // order in which the futures resolved.
    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_EQ(outputValues[i], i);
    }
}

TEST_F(WhenAllSucceedTest, WhenAllSucceedWorksWithExecutorFutures) {
    const auto kNumInputs = 5;
    auto [inputPromises, rawInputFutures] = makePromisesAndFutures<int>(kNumInputs);

    // Turn raw input Futures into ExecutorFutures.
    std::vector<ExecutorFuture<int>> inputFutures;
    for (auto i = 0; i < kNumInputs; ++i) {
        inputFutures.emplace_back(std::move(rawInputFutures[i]).thenRunOn(executor()));
    }

    auto result = whenAllSucceed(std::move(inputFutures));

    const auto kValue = 5;
    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue(kValue);
    }

    auto outputValues = result.get();

    ASSERT_EQ(outputValues.size(), kNumInputs);
    for (auto v : outputValues) {
        ASSERT_EQ(v, kValue);
    }
}

TEST_F(WhenAllSucceedTest, VariadicWhenAllSucceedMaintainsOrderingOfInputFutures) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures[0]),
                                 std::move(inputFutures[1]),
                                 std::move(inputFutures[2]),
                                 std::move(inputFutures[3]),
                                 std::move(inputFutures[4]));

    // Create a random order of indexes in which to resolve the input futures.
    std::vector<int> ordering;
    for (auto i = 0; i < kNumInputs; ++i) {
        ordering.push_back(i);
    }
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(ordering.begin(), ordering.end(), g);

    for (auto idx : ordering) {
        ASSERT_FALSE(result.isReady());
        inputPromises[idx].emplaceValue(idx);
    }

    auto outputValues = result.get();
    ASSERT_EQ(outputValues.size(), kNumInputs);

    // The output should be in the same order as the input, regardless of the
    // order in which the futures resolved.
    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_EQ(outputValues[i], i);
    }
}

// Test whenAllSucceed with void input futures.
using WhenAllSucceedVoidTest = WhenAllSucceedTest;

DEATH_TEST_F(WhenAllSucceedVoidTest,
             WhenAllSucceedFailsWithInputVectorOfSizeZero,
             future_util_details::kWhenAllSucceedEmptyInputInvariantMsg) {
    std::vector<Future<void>> inputFutures;
    std::ignore = whenAllSucceed(std::move(inputFutures));
}

TEST_F(WhenAllSucceedVoidTest, WhenAllSucceedReturnsErrorOnFirstErrorWhenOneInputFuture) {
    std::vector<Future<void>> inputFutures;
    auto [inputPromise, inputFuture] = makePromiseFuture<void>();
    inputFutures.emplace_back(std::move(inputFuture));
    auto result = whenAllSucceed(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());
    inputPromise.setError(kErrorStatus);
    ASSERT_EQ(result.getNoThrow(), kErrorStatus);
}


TEST_F(WhenAllSucceedVoidTest,
       WhenAllSucceedReturnsErrorOnFirstErrorWhenOthersUnresolvedAndSucceedAfter) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<void>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    inputPromises[0].setError(kErrorStatus);
    ASSERT_EQ(result.getNoThrow(), kErrorStatus);

    // Start at 1 since the 0'th input was already set with an error.
    for (auto i = 1; i < kNumInputs; ++i) {
        inputPromises[i].emplaceValue();
    }
}

TEST_F(WhenAllSucceedVoidTest, WhenAllSucceedIgnoresErrorAfterFirstError) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<void>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    inputPromises[0].setError(kErrorStatus);
    ASSERT_EQ(result.getNoThrow(), kErrorStatus);

    // Finish resolving remaining with an error. Start at 1 since the 0'th input was already set
    // with an error. It's not strictly necessary to do this explicitly since they'd fail with
    // BrokenPromise anyways when the promises are destroyed, but this makes the test case more
    // clear.
    for (auto i = 1; i < kNumInputs; ++i) {
        inputPromises[i].setError(kErrorStatus);
    }
}

TEST_F(WhenAllSucceedVoidTest, WhenAllSucceedReturnsErrorOnFirstErrorWhenAllOthersAreResolved) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<void>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures));

    // Emplace all but the last promise with a value.
    for (auto i = 1; i < kNumInputs - 1; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue();
    }
    ASSERT_FALSE(result.isReady());
    // Set an error on the last input.
    inputPromises[kNumInputs - 1].setError(kErrorStatus);
    ASSERT_EQ(result.getNoThrow(), kErrorStatus);
}

TEST_F(WhenAllSucceedVoidTest, WhenAllSucceedReturnsAfterSuccessfulResponseWithOneInputFuture) {
    std::vector<Future<void>> inputFutures;
    auto [inputPromise, inputFuture] = makePromiseFuture<void>();
    inputFutures.emplace_back(std::move(inputFuture));
    auto result = whenAllSucceed(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());
    inputPromise.emplaceValue();
    ASSERT_EQ(result.getNoThrow(), Status::OK());
}

TEST_F(WhenAllSucceedVoidTest,
       WhenAllSucceedVoidReturnsAfterLastSuccessfulResponseWithManyInputFutures) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<void>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures));

    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue();
    }

    ASSERT_EQ(result.getNoThrow(), Status::OK());
}

TEST_F(WhenAllSucceedVoidTest,
       VariadicWhenAllSucceedVoidReturnsReturnsAfterLastSuccessfulResponseWithManyInputFutures) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<void>(kNumInputs);

    auto result = whenAllSucceed(std::move(inputFutures[0]),
                                 std::move(inputFutures[1]),
                                 std::move(inputFutures[2]),
                                 std::move(inputFutures[3]),
                                 std::move(inputFutures[4]));

    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue();
    }

    ASSERT_EQ(result.getNoThrow(), Status::OK());
}

TEST_F(WhenAllSucceedVoidTest, WhenAllSucceedWorksWithExecutorFutures) {
    const auto kNumInputs = 5;
    auto [inputPromises, rawInputFutures] = makePromisesAndFutures<void>(kNumInputs);

    // Turn raw input Futures into ExecutorFutures.
    std::vector<ExecutorFuture<void>> inputFutures;
    for (auto i = 0; i < kNumInputs; ++i) {
        inputFutures.emplace_back(std::move(rawInputFutures[i]).thenRunOn(executor()));
    }

    auto result = whenAllSucceed(std::move(inputFutures));

    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue();
    }

    ASSERT_EQ(result.getNoThrow(), Status::OK());
}

class WhenAllTest : public FutureUtilTest {};

TEST_F(WhenAllTest, ReturnsOnceAllInputsResolveWithSuccess) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAll(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kValue = 10;
    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue(kValue);
    }

    auto output = result.get();
    ASSERT_EQ(output.size(), kNumInputs);
    for (auto& swValue : output) {
        ASSERT_TRUE(swValue.isOK());
        ASSERT_EQ(swValue.getValue(), kValue);
    }
}

TEST_F(WhenAllTest, ReturnsOnceAllInputsResolveWithError) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAll(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].setError(kErrorStatus);
    }

    auto output = result.get();
    ASSERT_EQ(output.size(), kNumInputs);
    for (auto& swValue : output) {
        ASSERT_EQ(swValue.getStatus(), kErrorStatus);
    }
}

TEST_F(WhenAllTest, ReturnsOnceAllInputsResolveWithSuccessWithVoidInputs) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<void>(kNumInputs);

    auto result = whenAll(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue();
    }

    auto output = result.get();
    ASSERT_EQ(output.size(), kNumInputs);
    for (auto& status : output) {
        ASSERT_OK(status);
    }
}

TEST_F(WhenAllTest, ReturnsOnceAllInputsResolveWithErrorWithVoidInputs) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<void>(kNumInputs);

    auto result = whenAll(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].setError(kErrorStatus);
    }

    auto output = result.get();
    ASSERT_EQ(output.size(), kNumInputs);
    for (auto& status : output) {
        ASSERT_EQ(status, kErrorStatus);
    }
}


TEST_F(WhenAllTest, ReturnsOnceAllInputsResolveWithMixOfSuccessAndError) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAll(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kValue = 10;
    const auto kNumSuccesses = 3;
    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        if (i < kNumSuccesses) {
            inputPromises[i].emplaceValue(kValue);
        } else {
            inputPromises[i].setError(kErrorStatus);
        }
    }

    auto output = result.get();
    ASSERT_EQ(output.size(), kNumInputs);
    for (auto i = 0; i < kNumInputs; ++i) {
        auto swValue = output[i];
        if (i < kNumSuccesses) {
            ASSERT_TRUE(swValue.isOK());
            ASSERT_EQ(swValue.getValue(), kValue);
        } else {
            ASSERT_EQ(swValue.getStatus(), kErrorStatus);
        }
    }
}

TEST_F(WhenAllTest, ReturnsOnceAllInputsResolveWithMixOfSuccessAndErrorInReverseOrder) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAll(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kValue = 10;
    const auto kNumSuccesses = 3;
    // Iterate over inputs backwards, resolving them one at a time.
    for (auto i = kNumInputs - 1; i >= 0; --i) {
        ASSERT_FALSE(result.isReady());
        if (i < kNumSuccesses) {
            inputPromises[i].emplaceValue(kValue);
        } else {
            inputPromises[i].setError(kErrorStatus);
        }
    }

    auto output = result.get();
    ASSERT_EQ(output.size(), kNumInputs);
    for (auto i = kNumInputs - 1; i >= 0; --i) {
        auto swValue = output[i];
        if (i < kNumSuccesses) {
            ASSERT_TRUE(swValue.isOK());
            ASSERT_EQ(swValue.getValue(), kValue);
        } else {
            ASSERT_EQ(swValue.getStatus(), kErrorStatus);
        }
    }
}

TEST_F(WhenAllTest, WorksWithExecutorFutures) {
    const auto kNumInputs = 5;
    auto [inputPromises, rawInputFutures] = makePromisesAndFutures<int>(kNumInputs);

    // Turn raw input Futures into ExecutorFutures.
    std::vector<ExecutorFuture<int>> inputFutures;
    for (auto i = 0; i < kNumInputs; ++i) {
        inputFutures.emplace_back(std::move(rawInputFutures[i]).thenRunOn(executor()));
    }

    auto result = whenAll(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kValue = 10;
    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue(kValue);
    }

    auto output = result.get();
    ASSERT_EQ(output.size(), kNumInputs);
    for (auto& swValue : output) {
        ASSERT_TRUE(swValue.isOK());
        ASSERT_EQ(swValue.getValue(), kValue);
    }
}

TEST_F(WhenAllTest, WorksWithVariadicTemplate) {
    const auto kNumInputs = 3;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result =
        whenAll(std::move(inputFutures[0]), std::move(inputFutures[1]), std::move(inputFutures[2]));
    ASSERT_FALSE(result.isReady());

    const auto kValue = 10;
    for (auto i = 0; i < kNumInputs; ++i) {
        ASSERT_FALSE(result.isReady());
        inputPromises[i].emplaceValue(kValue);
    }

    auto output = result.get();
    ASSERT_EQ(output.size(), kNumInputs);
    for (auto& swValue : output) {
        ASSERT_TRUE(swValue.isOK());
        ASSERT_EQ(swValue.getValue(), kValue);
    }
}

class WhenAnyTest : public FutureUtilTest {};

TEST_F(WhenAnyTest, ReturnsTheFirstFutureToResolveWhenThatFutureContainsSuccessAndOnlyOneInput) {
    std::vector<Future<int>> inputFutures;
    auto [promise, future] = makePromiseFuture<int>();
    inputFutures.emplace_back(std::move(future));
    auto result = whenAny(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kValue = 10;
    promise.emplaceValue(kValue);
    auto [swVal, idx] = result.get();
    ASSERT_TRUE(swVal.isOK());
    ASSERT_EQ(swVal.getValue(), kValue);
    ASSERT_EQ(idx, 0);
}

TEST_F(WhenAnyTest, ReturnsTheFirstFutureToResolveWhenThatFutureContainsSuccessAndMultipleInputs) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAny(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kWhichIdxWillBeFirst = 3;
    const auto kValue = 10;
    inputPromises[kWhichIdxWillBeFirst].emplaceValue(kValue);
    auto [swVal, idx] = result.get();
    ASSERT_TRUE(swVal.isOK());
    ASSERT_EQ(swVal.getValue(), kValue);
    ASSERT_EQ(idx, kWhichIdxWillBeFirst);
}

TEST_F(WhenAnyTest,
       ReturnsTheFirstFutureToResolveWhenThatFutureContainsSuccessAndMultipleInputsWithVoidInputs) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<void>(kNumInputs);

    auto result = whenAny(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kWhichIdxWillBeFirst = 3;
    inputPromises[kWhichIdxWillBeFirst].emplaceValue();
    auto [status, idx] = result.get();
    ASSERT_OK(status);
    ASSERT_EQ(idx, kWhichIdxWillBeFirst);
}

TEST_F(
    WhenAnyTest,
    ReturnsTheFirstFutureToResolveWhenThatFutureContainsSuccessAndMultipleInputsAndOthersSucceedAfter) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAny(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kWhichIdxWillBeFirst = 3;
    const auto kValue = 10;
    inputPromises[kWhichIdxWillBeFirst].emplaceValue(kValue);
    auto [swVal, idx] = result.get();
    ASSERT_TRUE(swVal.isOK());
    ASSERT_EQ(swVal.getValue(), kValue);
    ASSERT_EQ(idx, kWhichIdxWillBeFirst);

    // Make sure there's no problem when these resolve after whenAny has resolved due to an error.
    for (auto i = 0; i < kNumInputs; ++i) {
        if (i != kWhichIdxWillBeFirst) {
            inputPromises[i].emplaceValue(5);
        }
    }
}

TEST_F(WhenAnyTest, ReturnsTheFirstFutureToResolveWhenThatFutureContainsAnErrorAndOnlyOneInput) {
    std::vector<Future<int>> inputFutures;
    auto [promise, future] = makePromiseFuture<int>();
    inputFutures.emplace_back(std::move(future));
    auto result = whenAny(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    promise.setError(kErrorStatus);
    auto [swVal, idx] = result.get();
    ASSERT_EQ(swVal.getStatus(), kErrorStatus);
    ASSERT_EQ(idx, 0);
}

TEST_F(WhenAnyTest, ReturnsTheFirstFutureToResolveWhenThatFutureContainsAnErrorAndMultipleInputs) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAny(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kWhichIdxWillBeFirst = 3;
    inputPromises[kWhichIdxWillBeFirst].setError(kErrorStatus);
    auto [swVal, idx] = result.get();
    ASSERT_EQ(swVal.getStatus(), kErrorStatus);
    ASSERT_EQ(idx, kWhichIdxWillBeFirst);
}

TEST_F(WhenAnyTest,
       ReturnsTheFirstFutureToResolveWhenThatFutureContainsAnErrorAndMultipleInputsWithVoidInputs) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<void>(kNumInputs);

    auto result = whenAny(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kWhichIdxWillBeFirst = 3;
    inputPromises[kWhichIdxWillBeFirst].setError(kErrorStatus);
    auto [status, idx] = result.get();
    ASSERT_EQ(status, kErrorStatus);
    ASSERT_EQ(idx, kWhichIdxWillBeFirst);
}

TEST_F(
    WhenAnyTest,
    ReturnsTheFirstFutureToResolveWhenThatFutureContainsAnErrorAndMultipleInputsAndOthersSucceedAfter) {
    const auto kNumInputs = 5;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result = whenAny(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kWhichIdxWillBeFirst = 3;
    inputPromises[kWhichIdxWillBeFirst].setError(kErrorStatus);
    auto [swVal, idx] = result.get();
    ASSERT_EQ(swVal.getStatus(), kErrorStatus);
    ASSERT_EQ(idx, kWhichIdxWillBeFirst);

    // Make sure there's no problem when these resolve after whenAny has resolved due to an error.
    for (auto i = 0; i < kNumInputs; ++i) {
        if (i != kWhichIdxWillBeFirst) {
            inputPromises[i].emplaceValue(5);
        }
    }
}

TEST_F(WhenAnyTest, WorksWithExecutorFutures) {
    const auto kNumInputs = 5;
    auto [inputPromises, rawInputFutures] = makePromisesAndFutures<int>(kNumInputs);

    // Turn raw input Futures into ExecutorFutures.
    std::vector<ExecutorFuture<int>> inputFutures;
    for (auto i = 0; i < kNumInputs; ++i) {
        inputFutures.emplace_back(std::move(rawInputFutures[i]).thenRunOn(executor()));
    }

    auto result = whenAny(std::move(inputFutures));
    ASSERT_FALSE(result.isReady());

    const auto kWhichIdxWillBeFirst = 3;
    const auto kValue = 10;
    inputPromises[kWhichIdxWillBeFirst].emplaceValue(kValue);
    auto [swVal, idx] = result.get();
    ASSERT_TRUE(swVal.isOK());
    ASSERT_EQ(swVal.getValue(), kValue);
    ASSERT_EQ(idx, kWhichIdxWillBeFirst);
}

TEST_F(WhenAnyTest, WorksWithVariadicTemplate) {
    const auto kNumInputs = 3;
    auto [inputPromises, inputFutures] = makePromisesAndFutures<int>(kNumInputs);

    auto result =
        whenAny(std::move(inputFutures[0]), std::move(inputFutures[1]), std::move(inputFutures[2]));
    ASSERT_FALSE(result.isReady());

    const auto kWhichIdxWillBeFirst = 1;
    const auto kValue = 10;
    inputPromises[kWhichIdxWillBeFirst].emplaceValue(kValue);
    auto [swVal, idx] = result.get();
    ASSERT_TRUE(swVal.isOK());
    ASSERT_EQ(swVal.getValue(), kValue);
    ASSERT_EQ(idx, kWhichIdxWillBeFirst);
}

TEST_F(WhenAnyTest, WorksWithVariadicTemplateAndExecutorFutures) {
    const auto kNumInputs = 3;
    auto [inputPromises, rawInputFutures] = makePromisesAndFutures<int>(kNumInputs);

    // Turn raw input Futures into ExecutorFutures.
    std::vector<ExecutorFuture<int>> inputFutures;
    for (auto i = 0; i < kNumInputs; ++i) {
        inputFutures.emplace_back(std::move(rawInputFutures[i]).thenRunOn(executor()));
    }

    auto result =
        whenAny(std::move(inputFutures[0]), std::move(inputFutures[1]), std::move(inputFutures[2]));
    ASSERT_FALSE(result.isReady());

    const auto kWhichIdxWillBeFirst = 1;
    const auto kValue = 10;
    inputPromises[kWhichIdxWillBeFirst].emplaceValue(kValue);
    auto [swVal, idx] = result.get();
    ASSERT_TRUE(swVal.isOK());
    ASSERT_EQ(swVal.getValue(), kValue);
    ASSERT_EQ(idx, kWhichIdxWillBeFirst);
}

class WithCancellationTest : public FutureUtilTest {};

TEST_F(FutureUtilTest,
       WithCancellationReturnsSuccessIfInputFutureResolvedWithSuccessBeforeCancellation) {
    const int kResult{5};
    auto [promise, future] = makePromiseFuture<int>();

    CancellationSource cancelSource;

    auto cancelableFuture = future_util::withCancellation(std::move(future), cancelSource.token());

    promise.emplaceValue(kResult);
    ASSERT_EQ(cancelableFuture.get(), kResult);

    // Canceling after the fact shouldn't hang or cause crashes.
    cancelSource.cancel();
}

TEST_F(FutureUtilTest,
       WithCancellationReturnsErrorIfInputFutureResolvedWithErrorBeforeCancellation) {
    auto [promise, future] = makePromiseFuture<int>();

    CancellationSource cancelSource;

    auto cancelableFuture = future_util::withCancellation(std::move(future), cancelSource.token());

    const Status kErrorStatus{ErrorCodes::InternalError, ""};
    promise.setError(kErrorStatus);
    ASSERT_EQ(cancelableFuture.getNoThrow(), kErrorStatus);

    // Canceling after the fact shouldn't hang or cause crashes.
    cancelSource.cancel();
}

TEST_F(FutureUtilTest, WithCancellationReturnsErrorIfTokenCanceledFirst) {
    auto [promise, future] = makePromiseFuture<int>();

    CancellationSource cancelSource;
    auto cancelableFuture = future_util::withCancellation(std::move(future), cancelSource.token());
    cancelSource.cancel();
    ASSERT_THROWS_CODE(cancelableFuture.get(), DBException, ErrorCodes::CallbackCanceled);

    // Keep from getting a BrokenPromise assertion when the input promise is destroyed.
    promise.setError(kCanceledStatus);
}

TEST_F(FutureUtilTest, WithCancellationWorksWithVoidInput) {
    auto [promise, future] = makePromiseFuture<void>();

    auto cancelableFuture =
        future_util::withCancellation(std::move(future), CancellationToken::uncancelable());

    promise.emplaceValue();
    ASSERT(cancelableFuture.isReady());
}

// TODO(SERVER-102282): One particular v5 buildvariant fails to build this test.
#if !(!defined(__clang__) && defined(__x86_64__) && __GNUC__ >= 14)
TEST_F(FutureUtilTest, WithCancellationWorksWithSemiFutureInput) {
    const int kResult{5};
    auto [promise, future] = makePromiseFuture<int>();

    auto cancelableFuture =
        future_util::withCancellation(std::move(future).semi(), CancellationToken::uncancelable());

    promise.emplaceValue(kResult);
    ASSERT_EQ(cancelableFuture.get(), kResult);
}
#endif

TEST_F(FutureUtilTest, WithCancellationWorksWithSharedSemiFutureInput) {
    const int kResult{5};
    auto [promise, future] = makePromiseFuture<int>();

    auto cancelableFuture = future_util::withCancellation(std::move(future).semi().share(),
                                                          CancellationToken::uncancelable());

    promise.emplaceValue(kResult);
    ASSERT_EQ(cancelableFuture.get(), kResult);
}

TEST_F(FutureUtilTest, WithCancellationWorksWithExecutorFutureInput) {
    const int kResult{5};
    auto [promise, future] = makePromiseFuture<int>();

    auto cancelableFuture = future_util::withCancellation(std::move(future).thenRunOn(executor()),
                                                          CancellationToken::uncancelable());

    promise.emplaceValue(kResult);
    ASSERT_EQ(cancelableFuture.get(), kResult);
}

}  // namespace
}  // namespace mongo
