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
#include "mongo/util/duration.h"

#include <array>
#include <span>
#include <string>

namespace mongo {
namespace {

class RetryStrategyTest : public ServiceContextTest {
public:
    RetryStrategyTest()
        : _retryBudget{std::make_shared<AdaptiveRetryStrategy::RetryBudget>(kReturnRate,
                                                                            kBudgetCapacity)},
          _opCtx{makeOperationContext()} {}

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
            DefaultRetryStrategy::defaultRetryCriteria,
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

    static inline const auto errorLabelsSystemOverloaded =
        std::array{std::string{ErrorLabel::kSystemOverloadedError}};

    // Error label array with a retriable error label that is not system overloaded.
    static inline const auto errorLabelsRetriable =
        std::array{std::string{ErrorLabel::kRetryableWrite}};

    // Error label array with no retriable error label, but an unimportant one.
    static inline const auto errorLabelsNonRetriable =
        std::array{std::string{ErrorLabel::kStreamProcessorUserError}};

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

}  // namespace
}  // namespace mongo
