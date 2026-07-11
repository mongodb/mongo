// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/primary_only_service_helpers/primary_only_service_retry_strategy.h"

#include "mongo/client/retry_strategy_server_parameters_gen.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/s/primary_only_service_helpers/with_automatic_retry.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

#include <cmath>

namespace mongo {
namespace primary_only_service_helpers {
namespace {

class PrimaryOnlyServiceRetryStrategyTest : public unittest::Test {
protected:
    static RetryabilityPredicate makeNeverRetryPredicate() {
        return [](const Status& status) {
            return false;
        };
    }

    PrimaryOnlyServiceRetryStrategy makeStrategy() {
        return PrimaryOnlyServiceRetryStrategy{kDefaultRetryabilityPredicate,
                                               [this](const Status& s) { ++_transientErrorCount; },
                                               [this](const Status& s) {
                                                   ++_unrecoverableErrorCount;
                                               }};
    }

    PrimaryOnlyServiceRetryStrategy makeNeverRetryStrategy() {
        return PrimaryOnlyServiceRetryStrategy{makeNeverRetryPredicate(),
                                               [this](const Status& s) { ++_transientErrorCount; },
                                               [this](const Status& s) {
                                                   ++_unrecoverableErrorCount;
                                               }};
    }

    int transientErrorCount() const {
        return _transientErrorCount;
    }

    int unrecoverableErrorCount() const {
        return _unrecoverableErrorCount;
    }

    static inline const Status kRetriableError{ErrorCodes::HostUnreachable, "retriable error"};
    static inline const Status kSystemOverloadedError{ErrorCodes::IngressRequestRateLimitExceeded,
                                                      "overloaded"};
    static inline const Status kNonRetriableError{ErrorCodes::CommandFailed, "non-retriable error"};

    static inline const auto kTarget1 = HostAndPort{"host1", 12345};

    static inline const auto kSystemOverloadedErrorLabels =
        std::vector{std::string{ErrorLabel::kSystemOverloadedError}};

private:
    int _transientErrorCount = 0;
    int _unrecoverableErrorCount = 0;
};

TEST_F(PrimaryOnlyServiceRetryStrategyTest, InitialDelayIsZero) {
    auto strategy = makeStrategy();
    ASSERT_EQ(strategy.getNextRetryDelay(), Milliseconds{0});
}

TEST_F(PrimaryOnlyServiceRetryStrategyTest, RetriableErrorCausesRetry) {
    auto strategy = makeStrategy();
    ASSERT_TRUE(
        strategy.recordFailureAndEvaluateShouldRetry(kRetriableError, kTarget1, {}, boost::none));
    ASSERT_EQ(transientErrorCount(), 1);
    ASSERT_EQ(unrecoverableErrorCount(), 0);
}

TEST_F(PrimaryOnlyServiceRetryStrategyTest, NonRetriableErrorDoesNotRetry) {
    auto strategy = makeNeverRetryStrategy();
    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(
        kNonRetriableError, kTarget1, {}, boost::none));
    ASSERT_EQ(transientErrorCount(), 0);
    ASSERT_EQ(unrecoverableErrorCount(), 1);
}

TEST_F(PrimaryOnlyServiceRetryStrategyTest, BackoffNonZeroAfterRetriableError) {
    auto strategy = makeStrategy();
    FailPointEnableBlock fp{"returnMaxBackoffDelay"};

    ASSERT_TRUE(
        strategy.recordFailureAndEvaluateShouldRetry(kRetriableError, kTarget1, {}, boost::none));

    auto delay = strategy.getNextRetryDelay();
    ASSERT_EQ(delay, Milliseconds{200});
}

TEST_F(PrimaryOnlyServiceRetryStrategyTest, BackoffGrowsWithSuccessiveRetriableErrors) {
    auto strategy = makeStrategy();

    FailPointEnableBlock fp{"returnMaxBackoffDelay"};

    const auto baseBackoff = gDefaultClientBaseBackoffMillis.loadRelaxed();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(strategy.recordFailureAndEvaluateShouldRetry(
            kRetriableError, kTarget1, {}, boost::none));
        auto delay = strategy.getNextRetryDelay();
        auto expectedDelay = Milliseconds{static_cast<int64_t>(baseBackoff * std::exp2(i + 1))};
        ASSERT_EQ(delay, expectedDelay);
    }
}

TEST_F(PrimaryOnlyServiceRetryStrategyTest, BackoffNonZeroAfterSystemOverloadedError) {
    auto strategy = makeStrategy();

    FailPointEnableBlock fp{"returnMaxBackoffDelay"};

    ASSERT_TRUE(strategy.recordFailureAndEvaluateShouldRetry(
        kSystemOverloadedError, kTarget1, kSystemOverloadedErrorLabels, boost::none));

    auto delay = strategy.getNextRetryDelay();
    ASSERT_GT(delay, Milliseconds{0});
}

}  // namespace
}  // namespace primary_only_service_helpers
}  // namespace mongo

