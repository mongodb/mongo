/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/client/retry_strategy.h"

#include "mongo/client/retry_strategy_server_parameters_gen.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"

#include <span>
#include <string>

namespace mongo {
namespace {

class RetryStrategyTest : public ServiceContextTest {
public:
    RetryStrategyTest()
        : ServiceContextTest{std::make_unique<ScopedGlobalServiceContextForTest>(
              ServiceContext::make(std::make_unique<ClockSourceMock>()))},
          _retryBudget{
              std::make_shared<AdaptiveRetryStrategy::RetryBudget>(kReturnRate, kBudgetCapacity)},
          _opCtx{makeOperationContext()} {}

    ClockSourceMock* getClockSource() {
        return static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

    DefaultRetryStrategy::RetryCriteria mockCallback() {
        return [&](Status s, std::span<const std::string> errorLabels) {
            _amountCallbackCalled.addAndFetch(1);
            return _shouldRetry;
        };
    }

    DefaultRetryStrategy makeDefaultRetryStrategyDefaultCallback() {
        return DefaultRetryStrategy{
            [&](Status s, std::span<const std::string> errorLabels) {
                _amountCallbackCalled.addAndFetch(1);
                return DefaultRetryStrategy::defaultRetryCriteria(s, errorLabels);
            },
            kBackoffParameters,
        };
    }

    DefaultRetryStrategy makeDefaultRetryStrategy() {
        return DefaultRetryStrategy{
            mockCallback(),
            kBackoffParameters,
        };
    }

    AdaptiveRetryStrategy makeAdaptiveRetryStrategy() {
        return AdaptiveRetryStrategy{
            _retryBudget,
            mockCallback(),
            kBackoffParameters,
        };
    }

    void exhaustRetryBudget() {
        auto strategy = _makeRetryStrategyNoIncrement();
        while (strategy.recordFailureAndEvaluateShouldRetry(
            statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded))
            ;
    }

    void depleteBudget(std::size_t amount) {
        auto strategy = _makeRetryStrategyNoIncrement();
        for (std::size_t i = 0; i < amount; ++i) {
            ASSERT(acquireToken(strategy));
        }
    }

    static inline const Status statusRetriableErrorCategory{
        ErrorCodes::RetriableRemoteCommandFailure, "error"};

    // Status with an error code in no categories.
    static inline const Status statusNonRetriable{ErrorCodes::CommandFailed, "error"};

    // Dummy targets with different values.
    static inline const auto target1 = HostAndPort{"host1", 12345};
    static inline const auto target2 = HostAndPort{"host2", 23456};
    static inline const auto target3 = HostAndPort{"host3", 34567};

    static inline const auto errorLabelsSystemOverloaded =
        std::vector{std::string{ErrorLabel::kSystemOverloadedError}};

    // Error label array with a retriable error label that is not system overloaded.
    static inline const auto errorLabelsRetriable =
        std::vector{std::string{ErrorLabel::kRetryableWrite}};

    // Error label array with no retriable error label, but an unimportant one.
    static inline const auto errorLabelsNonRetriable =
        std::vector{std::string{ErrorLabel::kStreamProcessorUserError}};

    static constexpr std::int32_t kMaxNumberOfRetries = 64;
    static constexpr std::int32_t kKnownSeed = 12345;

    static constexpr DefaultRetryStrategy::BackoffParameters kBackoffParameters{
        .maxRetryAttempts = kMaxNumberOfRetries,
        .baseBackoff = Milliseconds{kDefaultClientBaseBackoffMillisDefault},
        .maxBackoff = Milliseconds{kDefaultClientMaxBackoffMillisDefault},
    };

    static constexpr std::int32_t kBudgetCapacity = 16;
    static constexpr std::int32_t kAmountOfSuccessForOneReturnedToken = 32;
    static constexpr double kReturnRate = 1. / kAmountOfSuccessForOneReturnedToken;

    // Floating point error can't be more than half the return rate. Bigger than that and one value
    // could be interpreted as an adjacent one.
    static constexpr double kMaxTokenError = kReturnRate / 2.;


    static constexpr int kNumThreads = 8;
    static_assert(
        kNumThreads <= kBudgetCapacity / 2,
        "For the tests to properly function, we need the amount of threads to be half the "
        "capacity or lower");

    static bool acquireToken(AdaptiveRetryStrategy& strategy) {
        return strategy.recordFailureAndEvaluateShouldRetry(
            statusNonRetriable, target1, errorLabelsSystemOverloaded);
    }

    void retryCriteriaDontRetry() {
        _shouldRetry = false;
    }

    std::int32_t retryCriteriaCallCount() {
        return _amountCallbackCalled.load();
    }

    static constexpr auto kExampleResultValue = "result"_sd;

protected:
    std::shared_ptr<AdaptiveRetryStrategy::RetryBudget> _retryBudget;

private:
    AdaptiveRetryStrategy _makeRetryStrategyNoIncrement() {
        return AdaptiveRetryStrategy{
            _retryBudget,
            [](Status, std::span<const std::string>) { return true; },
            AdaptiveRetryStrategy::BackoffParameters{
                .maxRetryAttempts = kMaxNumberOfRetries,
                .baseBackoff = Milliseconds{kDefaultClientBaseBackoffMillisDefault},
                .maxBackoff = Milliseconds{kDefaultClientMaxBackoffMillisDefault},
            },
        };
    }

    bool _shouldRetry = true;
    Atomic<std::int32_t> _amountCallbackCalled = 0;
    ServiceContext::UniqueOperationContext _opCtx;
};

class RetryStrategyResultTest : public RetryStrategyTest {};

TEST_F(RetryStrategyTest, DefaultRetryStrategyMaxRetry) {
    auto strategy = makeDefaultRetryStrategy();

    // Exhaust the amount of retry for this strategy.
    for (std::int32_t i = 0; i < kMaxNumberOfRetries; ++i) {
        ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
            statusRetriableErrorCategory, target1, {}));
    }

    ASSERT_EQ(retryCriteriaCallCount(), kMaxNumberOfRetries);

    // Attempting to retry past the maximum amount of retry should fail and not call the callback.
    ASSERT_FALSE(
        strategy.recordFailureAndEvaluateShouldRetry(statusRetriableErrorCategory, target1, {}));
    ASSERT_EQ(retryCriteriaCallCount(), kMaxNumberOfRetries);
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyNoDelayNotOverloaded) {
    auto strategy = makeDefaultRetryStrategy();

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(statusRetriableErrorCategory, target1, {}));

    // The retry delay should be zero since the system overloaded error label was not sent.
    ASSERT_EQ(strategy.getNextRetryDelay(), Milliseconds{0});
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyCallbackNoRetry) {
    auto strategy = makeDefaultRetryStrategy();
    retryCriteriaDontRetry();

    // The callback will return false, so the amount of calls should increment but the strategy
    // should return false.
    ASSERT_FALSE(
        strategy.recordFailureAndEvaluateShouldRetry(statusRetriableErrorCategory, target1, {}));
    ASSERT_EQ(retryCriteriaCallCount(), 1);
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyHasDelay) {
    auto strategy = makeDefaultRetryStrategy();

    BackoffWithJitter::initRandomEngineWithSeed_forTest(kKnownSeed);

    for (std::int32_t i = 0; i < kMaxNumberOfRetries; ++i) {
        ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
            statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded));
        ASSERT_GT(strategy.getNextRetryDelay(), Milliseconds{0});
    }
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyDefaultCallbackStatusRetryable) {
    auto strategy = makeDefaultRetryStrategyDefaultCallback();

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(statusRetriableErrorCategory, target1, {}));
    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(statusNonRetriable, target1, {}));
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyDefaultCallbackErrorLabelRetryable) {
    auto strategy = makeDefaultRetryStrategyDefaultCallback();

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsRetriable));
    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusNonRetriable, target1, errorLabelsRetriable));
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyDefaultCallbackErrorLabelNonRetryable) {
    auto strategy = makeDefaultRetryStrategyDefaultCallback();

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsNonRetriable));
    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(
        statusNonRetriable, target1, errorLabelsNonRetriable));
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyTargetingMetadataInitiallyEmpty) {
    auto strategy = makeDefaultRetryStrategyDefaultCallback();

    const auto& targetingMetadata = strategy.getTargetingMetadata();
    ASSERT(targetingMetadata.deprioritizedServers.empty());
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyTargetingMetadataNonRetryable) {
    auto strategy = makeDefaultRetryStrategyDefaultCallback();
    const auto& targetingMetadata = strategy.getTargetingMetadata();

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsNonRetriable));
    ASSERT_EQ(targetingMetadata.deprioritizedServers.size(), 0);
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyTargetingMetadataRetryable) {
    auto strategy = makeDefaultRetryStrategyDefaultCallback();
    const auto& targetingMetadata = strategy.getTargetingMetadata();

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded));
    ASSERT_EQ(targetingMetadata.deprioritizedServers.size(), 1);
    ASSERT(targetingMetadata.deprioritizedServers.contains(target1));
    ASSERT_FALSE(targetingMetadata.deprioritizedServers.contains(target2));
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyTargetingMetadataRetryExhausted) {
    auto strategy = makeDefaultRetryStrategyDefaultCallback();
    const auto& targetingMetadata = strategy.getTargetingMetadata();

    for (std::size_t i = 0; i < kMaxNumberOfRetries; ++i) {
        ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
            statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded));
    }

    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(
        statusNonRetriable, target2, errorLabelsSystemOverloaded));

    // The amount of deprioritized server here stays to 1 because we don't need to deprioritize
    // servers if we stop the retry loop by returning false.
    ASSERT_EQ(targetingMetadata.deprioritizedServers.size(), 1);
    ASSERT_FALSE(targetingMetadata.deprioritizedServers.contains(target2));
}

TEST_F(RetryStrategyTest, AdaptiveRetryStrategyNonZeroRetryDelay) {
    auto strategy = makeAdaptiveRetryStrategy();

    BackoffWithJitter::initRandomEngineWithSeed_forTest(kKnownSeed);

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded));

    ASSERT_GT(strategy.getNextRetryDelay(), Milliseconds{0});
}

TEST_F(RetryStrategyTest, AdaptiveRetryStrategyCallbackCalled) {
    auto strategy = makeAdaptiveRetryStrategy();

    for (std::int32_t i = 0; i < kBudgetCapacity; ++i) {
        ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
            statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded));
    }

    // Validate that each of the failures recorded have called the callback and depleted the budget.
    ASSERT_EQ(retryCriteriaCallCount(), kBudgetCapacity);
    ASSERT_EQ(_retryBudget->getBalance_forTest(), 0);
}

TEST_F(RetryStrategyTest, AdaptiveRetryStrategyNoBudget) {
    auto strategy = makeAdaptiveRetryStrategy();
    exhaustRetryBudget();

    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded));

    ASSERT_EQ(retryCriteriaCallCount(), 0);
}

TEST_F(RetryStrategyTest, AdaptiveRetryStrategyFailNoReturnToken) {
    auto strategy = makeAdaptiveRetryStrategy();
    exhaustRetryBudget();

    // Failing because of other reasons than system overloaded should replenish the budget by
    // returnRate. We do not return a whole token because no system overloaded failure were
    // recorded.
    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(statusRetriableErrorCategory, target1, {}));
    ASSERT_EQ(_retryBudget->getBalance_forTest(), kReturnRate);
}

TEST_F(RetryStrategyTest, AdaptiveRetryStrategyReturnWholeToken) {
    auto strategy = makeAdaptiveRetryStrategy();

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded));

    exhaustRetryBudget();

    // Since we failed with the system overloaded error label before exhausting the budget, a whole
    // token will be returned in addition of the return rate.
    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(statusRetriableErrorCategory, target1, {}));
    ASSERT_EQ(_retryBudget->getBalance_forTest(), kReturnRate + 1);
}

TEST_F(RetryStrategyTest, AdaptiveRetryStrategyReplenishBudgetBySuccess) {
    auto strategy = makeAdaptiveRetryStrategy();

    exhaustRetryBudget();

    for (std::int32_t i = 0; i < kAmountOfSuccessForOneReturnedToken; ++i) {
        strategy.recordSuccess(target1);
    }

    ASSERT_APPROX_EQUAL(_retryBudget->getBalance_forTest(), 1, kMaxTokenError);
}

TEST_F(RetryStrategyTest, AdaptiveRetryStrategyReplenishBudgetByError) {
    auto strategy = makeAdaptiveRetryStrategy();

    exhaustRetryBudget();

    // We should have accumulated one token after non overloaded errors.
    for (std::int32_t i = 0; i < kAmountOfSuccessForOneReturnedToken; ++i) {
        ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
            statusRetriableErrorCategory, target1, {}));
    }

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded));
    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded));
}

TEST_F(RetryStrategyTest, RetryBudgetHighlyConcurrentAcquire) {
    std::vector<stdx::thread> threads;

    // We start many threads all trying to acquire tokens.
    for (std::size_t i = 0; i < kNumThreads; ++i) {
        threads.emplace_back(
            [strategy = makeAdaptiveRetryStrategy()]() mutable { ASSERT(acquireToken(strategy)); });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Ensure the count is right since the retry budget should be thread safe.
    ASSERT_EQ(_retryBudget->getBalance_forTest(), kBudgetCapacity - kNumThreads);
}

TEST_F(RetryStrategyTest, RetryBudgetHighlyConcurrentAcquireAndReleaseInBound) {
    auto strategy = makeAdaptiveRetryStrategy();

    // Deplete enough of the budget so that all threads can return or acquire a token.
    depleteBudget(kBudgetCapacity - kNumThreads / 2);

    {
        std::vector<unittest::JoinThread> threads;

        // We start the first half of the threads, and they will each try to acquire a token.
        // The balance is large enough for all of them to succeed.
        for (std::size_t i = 0; i < kNumThreads / 2; ++i) {
            threads.emplace_back([strategy = makeAdaptiveRetryStrategy()]() mutable {
                ASSERT(acquireToken(strategy));
            });
        }

        // We then start the other half of the threads, and they will all attempt return one whole
        // token. There is enough capacity for all of them to succeed.
        for (std::size_t i = 0; i < kNumThreads / 2; ++i) {
            threads.emplace_back([strategy = makeAdaptiveRetryStrategy()]() mutable {
                for (std::size_t i = 0; i < kAmountOfSuccessForOneReturnedToken; ++i) {
                    strategy.recordSuccess(target1);
                }
            });
        }
    }

    // Here we should see no change in balance since half the threads are returning tokens.
    ASSERT_EQ(_retryBudget->getBalance_forTest(), kNumThreads / 2);
}

TEST_F(RetryStrategyTest, RetryBudgetHighlyConcurrentAcquireWithFailures) {
    auto strategy = makeAdaptiveRetryStrategy();

    // Deplete most of the capacity so that some threads won't be able to acquire tokens.
    depleteBudget(kBudgetCapacity - kNumThreads / 2);
    ASSERT_LT(_retryBudget->getBalance_forTest(), kNumThreads);
    const auto expectedAmountOfFailures = kNumThreads - _retryBudget->getBalance_forTest();
    Atomic<std::int32_t> failures{0};

    {
        std::vector<unittest::JoinThread> threads;

        // Start many threads all trying to acquire a token. Since there's not enough tokens
        // for all threads to succeed, we check the amount of threads that couldn't.
        for (std::size_t i = 0; i < kNumThreads; ++i) {
            threads.emplace_back([strategy = makeAdaptiveRetryStrategy(), &failures]() mutable {
                if (!acquireToken(strategy)) {
                    failures.addAndFetch(1);
                }
            });
        }
    }

    ASSERT_EQ(failures.load(), expectedAmountOfFailures);
}

TEST_F(RetryStrategyTest, RetryBudgetHighlyConcurrentReleaseMaxCapacity) {
    auto strategy = makeAdaptiveRetryStrategy();

    // Deplete the budget just enough that some threads will not return tokens.
    depleteBudget(kNumThreads / 2);

    {
        std::vector<unittest::JoinThread> threads;

        // Start many threads all trying to record a success. We verify that we do not exceed
        // the budget capacity. The atomic operation should respect the maximum amount.
        for (std::size_t i = 0; i < kNumThreads; ++i) {
            threads.emplace_back([strategy = makeAdaptiveRetryStrategy()]() mutable {
                for (std::size_t i = 0; i < kAmountOfSuccessForOneReturnedToken; ++i) {
                    strategy.recordSuccess(target1);
                }
            });
        }
    }

    ASSERT_EQ(_retryBudget->getBalance_forTest(), kBudgetCapacity);
}

TEST_F(RetryStrategyTest, RetryBudgetSetCapacitySmaller) {
    ASSERT_EQ(_retryBudget->getBalance_forTest(), kBudgetCapacity);
    _retryBudget->updateRateParameters(kReturnRate, kBudgetCapacity / 2.);
    ASSERT_EQ(_retryBudget->getBalance_forTest(), kBudgetCapacity / 2.);
}

TEST_F(RetryStrategyTest, RetryBudgetSetCapacityBigger) {
    depleteBudget(kBudgetCapacity - 2);
    ASSERT_EQ(_retryBudget->getBalance_forTest(), 2);
    _retryBudget->updateRateParameters(kReturnRate, kBudgetCapacity / 2.);
    ASSERT_EQ(_retryBudget->getBalance_forTest(), 2);
}

TEST_F(RetryStrategyTest, RunWithRetryStrategySuccessOnly) {
    auto strategy = makeDefaultRetryStrategy();

    auto result =
        runWithRetryStrategy(opCtx(), strategy, [&](const TargetingMetadata& targetingMetadata) {
            return RetryStrategy::Result{kExampleResultValue, target1};
        });

    ASSERT(result.isOK());
    ASSERT_EQ(result, kExampleResultValue);
    ASSERT_EQ(retryCriteriaCallCount(), 0);
}

TEST_F(RetryStrategyTest, RunWithRetryStrategyWithRetry) {
    auto strategy = makeDefaultRetryStrategy();

    auto result =
        runWithRetryStrategy(opCtx(), strategy, [&](const TargetingMetadata& targetingMetadata) {
            // Always fail in this case to cause retries until we reach the maximum.
            return RetryStrategy::Result<StringData>{statusRetriableErrorCategory, {}};
        });

    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(result, statusRetriableErrorCategory.code());
    ASSERT_EQ(retryCriteriaCallCount(), kMaxNumberOfRetries);
}

TEST_F(RetryStrategyTest, RunWithRetryStrategyTargetingMetadata) {
    auto strategy = makeDefaultRetryStrategy();

    auto result =
        runWithRetryStrategy(opCtx(), strategy, [&](const TargetingMetadata& targetingMetadata) {
            // At the first try, there is no target1 in the list of deprioritized servers.
            if (!targetingMetadata.deprioritizedServers.contains(target1)) {
                return RetryStrategy::Result<StringData>{
                    statusNonRetriable, errorLabelsSystemOverloaded, target1};
            }

            // At the second try, there is target1, but no target2 in the list of deprioritized
            // servers.
            if (!targetingMetadata.deprioritizedServers.contains(target2)) {
                return RetryStrategy::Result<StringData>{
                    statusNonRetriable, errorLabelsSystemOverloaded, target2};
            }

            // At the third try, we return a success on target3.
            return RetryStrategy::Result{kExampleResultValue, target3};
        });

    ASSERT(result.isOK());
    ASSERT_EQ(result, kExampleResultValue);
    ASSERT_EQ(retryCriteriaCallCount(), 2);
}

TEST_F(RetryStrategyTest, RunWithRetryStrategyWithNonRetryableFailure) {
    auto strategy = makeDefaultRetryStrategy();
    retryCriteriaDontRetry();

    // We expect run operation to not retry when the retry strategy returns false.
    auto result =
        runWithRetryStrategy(opCtx(), strategy, [](const TargetingMetadata& targetingMetadata) {
            return RetryStrategy::Result<StringData>{statusNonRetriable, {}};
        });

    // Since there was no retry, we expect to get the same error code and we expect
    // the strategy to only run once.
    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(result, statusNonRetriable.code());
    ASSERT_EQ(retryCriteriaCallCount(), 1);
}

TEST_F(RetryStrategyTest, RunWithRetryStrategyWithNonRetryableFailureException) {
    auto strategy = makeDefaultRetryStrategy();
    retryCriteriaDontRetry();

    // In this case runOperation always throws. We expect runWithRetryStrategy to catch and return.
    auto result = runWithRetryStrategy(
        opCtx(),
        strategy,
        [](const TargetingMetadata& targetingMetadata) -> RetryStrategy::Result<StringData> {
            uasserted(statusNonRetriable.code(), statusNonRetriable.reason());
        });

    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(result, statusNonRetriable.code());
    ASSERT_EQ(retryCriteriaCallCount(), 1);
}

TEST_F(RetryStrategyTest, RunWithRetryStrategyWithArtificialDeadlineNow) {
    auto strategy = makeDefaultRetryStrategyDefaultCallback();

    auto result =
        opCtx()->runWithDeadline(getClockSource()->now(), ErrorCodes::MaxTimeMSExpired, [&] {
            return runWithRetryStrategy(
                opCtx(), strategy, [](const TargetingMetadata& targetingMetadata) {
                    return RetryStrategy::Result<StringData>{statusNonRetriable,
                                                             errorLabelsRetriable};
                });
        });

    ASSERT_FALSE(result.isOK());
    ASSERT_EQ(result, ErrorCodes::MaxTimeMSExpired);
    ASSERT_EQ(retryCriteriaCallCount(), 1);
}

TEST_F(RetryStrategyTest, RunWithRetryStrategyDeadlineDontPreventOperation) {
    const auto beginOfTest = getClockSource()->now();
    const auto deadline = beginOfTest + Milliseconds{100};
    constexpr auto operationRunningTime = Milliseconds{500};

    auto strategy = makeDefaultRetryStrategyDefaultCallback();

    auto result = opCtx()->runWithDeadline(deadline, ErrorCodes::MaxTimeMSExpired, [&] {
        return runWithRetryStrategy(
            opCtx(), strategy, [&](const TargetingMetadata& targetingMetadata) {
                // We wait for longer than the deadline.
                getClockSource()->advance(operationRunningTime);
                return RetryStrategy::Result{kExampleResultValue};
            });
    });

    ASSERT(result.isOK());
    ASSERT_EQ(retryCriteriaCallCount(), 0);
    ASSERT_GTE(getClockSource()->now() - beginOfTest, operationRunningTime);
}

// Since RetryStrategy::Result is meant to be similar to StatusWith, we perform similar tests as
// seen in status_with_test.cpp.
TEST_F(RetryStrategyResultTest, MakeCtad) {
    auto validate = [](auto&& arg) {
        auto r = RetryStrategy::Result(arg);
        ASSERT_TRUE(r.isOK());
        using Arg = std::decay_t<decltype(arg)>;
        return std::is_same_v<decltype(r), RetryStrategy::Result<Arg>>;
    };
    ASSERT_TRUE(validate(3));
    ASSERT_TRUE(validate(false));
    ASSERT_TRUE(validate(123.45));
    ASSERT_TRUE(validate(std::string("foo")));
    ASSERT_TRUE(validate(std::vector<int>()));
    ASSERT_TRUE(validate(std::vector<int>({1, 2, 3})));
}

TEST_F(RetryStrategyResultTest, nonDefaultConstructible) {
    class NoDefault {
    public:
        NoDefault() = delete;
        NoDefault(int x) : x{x} {}
        int x;
    };

    auto rND = RetryStrategy::Result(NoDefault(1));
    ASSERT_EQ(rND.getValue().x, 1);

    auto rNDerror = RetryStrategy::Result<NoDefault>{statusNonRetriable, {}};
    ASSERT_FALSE(rNDerror.isOK());
}

TEST_F(RetryStrategyResultTest, AssertionFormat) {
    ASSERT_EQ(
        unittest::stringify::invoke(RetryStrategy::Result<StringData>(statusNonRetriable, {})),
        unittest::stringify::invoke(statusNonRetriable));
    ASSERT_EQ(unittest::stringify::invoke(StatusWith<StringData>("foo")), "foo");
}

TEST_F(RetryStrategyResultTest, ErrorLabels) {
    auto rLabels = RetryStrategy::Result<StringData>{statusNonRetriable, errorLabelsRetriable};
    ASSERT(std::ranges::equal(rLabels.getErrorLabels(), errorLabelsRetriable));
    auto rNoLabels = RetryStrategy::Result<StringData>{statusNonRetriable, {}};
    ASSERT(std::ranges::equal(rNoLabels.getErrorLabels(), std::vector<std::string>{}));
}

TEST_F(RetryStrategyResultTest, OriginError) {
    auto rWithOrigin = RetryStrategy::Result<StringData>{statusNonRetriable, {}, target1};
    ASSERT_EQ(rWithOrigin.getOrigin(), target1);
    auto rNoOrigin = RetryStrategy::Result<StringData>{statusNonRetriable, {}};
    ASSERT_EQ(rNoOrigin.getOrigin(), boost::none);
}

TEST_F(RetryStrategyResultTest, OriginSuccess) {
    auto rWithOrigin = RetryStrategy::Result{kExampleResultValue, target2};
    ASSERT_EQ(rWithOrigin.getOrigin(), target2);
    auto rNoOrigin = RetryStrategy::Result{kExampleResultValue};
    ASSERT_EQ(rNoOrigin.getOrigin(), boost::none);
}

TEST_F(RetryStrategyResultTest, ConvertingCopyOK) {
    auto r1 = RetryStrategy::Result{kExampleResultValue, target1};
    auto r2 = RetryStrategy::Result<std::string>{r1};

    ASSERT(r2.isOK());
    ASSERT_EQ(r2.getValue(), r1.getValue());
    ASSERT_EQ(r2.getOrigin(), r1.getOrigin());
}

TEST_F(RetryStrategyResultTest, ConvertingMoveOK) {
    auto r1 = RetryStrategy::Result{kExampleResultValue, target1};
    auto r2 = RetryStrategy::Result<std::string>{std::move(r1)};

    ASSERT(r2.isOK());
    ASSERT_EQ(r2.getValue(), kExampleResultValue);
    ASSERT_EQ(r2.getOrigin(), target1);
}

TEST_F(RetryStrategyResultTest, ConvertingCopyError) {
    auto r1 =
        RetryStrategy::Result<StringData>{statusNonRetriable, errorLabelsNonRetriable, target1};
    auto r2 = RetryStrategy::Result<std::string>{r1};

    ASSERT_FALSE(r2.isOK());
    ASSERT_EQ(r2.getStatus(), statusNonRetriable);
    ASSERT_EQ(r2.getOrigin(), r1.getOrigin());
    ASSERT(std::ranges::equal(r2.getErrorLabels(), r1.getErrorLabels()));
}

TEST_F(RetryStrategyResultTest, ConvertingMoveError) {
    auto r1 =
        RetryStrategy::Result<StringData>{statusNonRetriable, errorLabelsNonRetriable, target1};
    auto r2 = RetryStrategy::Result<std::string>{std::move(r1)};

    ASSERT_FALSE(r2.isOK());
    ASSERT_EQ(r2.getStatus(), statusNonRetriable);
    ASSERT_EQ(r2.getOrigin(), target1);
    ASSERT(std::ranges::equal(r2.getErrorLabels(), errorLabelsNonRetriable));
}

TEST_F(RetryStrategyResultTest, ConvertingStatusWithCopy) {
    auto r1 = RetryStrategy::Result<StringData>{statusNonRetriable, {}};
    auto sw = StatusWith<std::string>{r1};

    ASSERT_FALSE(sw.isOK());
    ASSERT_EQ(sw.getStatus(), statusNonRetriable);
}

TEST_F(RetryStrategyResultTest, ConvertingStatusWithMove) {
    auto r1 = RetryStrategy::Result<StringData>{statusNonRetriable, {}};
    auto sw = StatusWith<std::string>{std::move(r1)};

    ASSERT_FALSE(sw.isOK());
    ASSERT_EQ(sw.getStatus(), statusNonRetriable);
}

}  // namespace
}  // namespace mongo
