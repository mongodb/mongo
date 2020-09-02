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

#include <algorithm>

#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/future_util.h"

namespace mongo {
namespace {

class FutureUtilTest : public unittest::Test {
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

using AsyncTryUntilTest = FutureUtilTest;

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

class WhenAllSucceedTest : public FutureUtilTest {
public:
    template <typename T>
    static std::pair<std::vector<Promise<T>>, std::vector<Future<T>>> makePromisesAndFutures(
        size_t size) {
        std::vector<Future<T>> inputFutures;
        std::vector<Promise<T>> inputPromises;
        for (size_t i = 0; i < size; ++i) {
            auto [inputPromise, inputFuture] = makePromiseFuture<T>();
            inputFutures.emplace_back(std::move(inputFuture));
            inputPromises.emplace_back(std::move(inputPromise));
        }
        return std::make_pair(std::move(inputPromises), std::move(inputFutures));
    }
};

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
    std::random_shuffle(ordering.begin(), ordering.end());

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

}  // namespace
}  // namespace mongo
