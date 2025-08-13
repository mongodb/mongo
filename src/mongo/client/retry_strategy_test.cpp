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
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <array>
#include <span>
#include <string>

namespace mongo {
namespace {

const Status statusRetriableErrorCategory{ErrorCodes::RetriableRemoteCommandFailure, "error"};

// Status with an error code in no categories.
const Status statusNonRetriable{ErrorCodes::CommandFailed, "error"};

// Dummy targets with different values.
const auto target1 = HostAndPort{"host1", 12345};
const auto target2 = HostAndPort{"host2", 23456};
const auto target3 = HostAndPort{"host3", 34567};


const auto errorLabelsSystemOverloaded =
    std::array{std::string{ErrorLabel::kSystemOverloadedError}};

// Error label array with a retriable error label that is not system overloaded.
const auto errorLabelsRetriable = std::array{std::string{ErrorLabel::kRetryableWrite}};

// Error label array with no retriable error label, but an unimportant one.
const auto errorLabelsNonRetriable = std::array{std::string{ErrorLabel::kStreamProcessorUserError}};

constexpr std::int32_t kMaxNumberOfRetries = 64;
constexpr std::int32_t kKnownSeed = 12345;

class RetryStrategyTest : public unittest::Test {
public:
    DefaultRetryStrategy defaultRetryStrategyDefault{
        DefaultRetryStrategy::defaultRetryCriteria,
        DefaultRetryStrategy::BackoffParameters{
            .maxRetryAttempts = kMaxNumberOfRetries,
            .baseBackoff = Milliseconds{kDefaultClientBaseBackoffMillisDefault},
            .maxBackoff = Milliseconds{kDefaultClientMaxBackoffMillisDefault},
        },
    };
};

// TODO: SERVER-108321 Split this test into many smaller ones and use fixture to setup values like
// initialStrategy and shouldRetry.
TEST_F(RetryStrategyTest, DefaultRetryStrategy) {
    std::int32_t amountCallbackCalled = 0;
    bool shouldRetry = true;
    const auto initialStrategy = DefaultRetryStrategy{
        [&](Status s, std::span<const std::string> errorLabels) {
            ++amountCallbackCalled;
            return shouldRetry;
        },
        DefaultRetryStrategy::BackoffParameters{
            .maxRetryAttempts = kMaxNumberOfRetries,
            .baseBackoff = Milliseconds{kDefaultClientBaseBackoffMillisDefault},
            .maxBackoff = Milliseconds{kDefaultClientMaxBackoffMillisDefault},
        },
    };

    auto strategy = initialStrategy;

    const auto target = HostAndPort{};

    for (std::int32_t i = 0; i < kMaxNumberOfRetries; ++i) {
        ASSERT(
            strategy.recordFailureAndEvaluateShouldRetry(statusRetriableErrorCategory, target, {}));

        // The retry delay should be zero since the system overloaded error label was not sent.
        ASSERT_EQ(strategy.getNextRetryDelay(), Milliseconds{0});
    }

    ASSERT_EQ(amountCallbackCalled, kMaxNumberOfRetries);

    ASSERT_FALSE(
        strategy.recordFailureAndEvaluateShouldRetry(statusRetriableErrorCategory, target, {}));
    ASSERT_EQ(amountCallbackCalled, kMaxNumberOfRetries);

    // Reset the state of the strategy so we can perform retries again.
    strategy = initialStrategy;
    amountCallbackCalled = 0;
    shouldRetry = false;

    // The callback will return false, so the amount of calls should increment but the strategy
    // should return false.
    ASSERT_FALSE(
        strategy.recordFailureAndEvaluateShouldRetry(statusRetriableErrorCategory, target, {}));
    ASSERT_EQ(amountCallbackCalled, 1);

    // Reset the state of the strategy so we can perform retries again.
    strategy = initialStrategy;
    amountCallbackCalled = 0;
    shouldRetry = true;

    BackoffWithJitter::initRandomEngineWithSeed_forTest(kKnownSeed);

    std::int32_t delayAboveZeroCount = 0;
    for (std::int32_t i = 0; i < kMaxNumberOfRetries; ++i) {
        ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
            statusRetriableErrorCategory, target, errorLabelsSystemOverloaded));
        if (strategy.getNextRetryDelay() > Milliseconds{0}) {
            ++delayAboveZeroCount;
        }
    }

    ASSERT_GT(delayAboveZeroCount, 0);
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyDefaultCallback) {
    auto strategy = defaultRetryStrategyDefault;

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(statusRetriableErrorCategory, target1, {}));
    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(statusNonRetriable, target1, {}));

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsRetriable));
    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusNonRetriable, target1, errorLabelsRetriable));

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsNonRetriable));
    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(
        statusNonRetriable, target1, errorLabelsNonRetriable));
}

TEST_F(RetryStrategyTest, DefaultRetryStrategyTargetingMetadata) {
    auto strategy = defaultRetryStrategyDefault;

    const auto& targetingMetadata = strategy.getTargetingMetadata();
    ASSERT(targetingMetadata.deprioritizedServers.empty());

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsNonRetriable));
    ASSERT_EQ(targetingMetadata.deprioritizedServers.size(), 0);

    ASSERT(strategy.recordFailureAndEvaluateShouldRetry(
        statusRetriableErrorCategory, target1, errorLabelsSystemOverloaded));
    ASSERT_EQ(targetingMetadata.deprioritizedServers.size(), 1);
    ASSERT(targetingMetadata.deprioritizedServers.contains(target1));
    ASSERT_FALSE(targetingMetadata.deprioritizedServers.contains(target2));

    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(
        statusNonRetriable, target2, errorLabelsNonRetriable));
    ASSERT_EQ(targetingMetadata.deprioritizedServers.size(), 1);
    ASSERT(targetingMetadata.deprioritizedServers.contains(target1));
    ASSERT_FALSE(targetingMetadata.deprioritizedServers.contains(target2));

    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(
        statusNonRetriable, target2, errorLabelsSystemOverloaded));
    // The amount of deprioritized server here stays to 1 because we don't need to deprioritize
    // servers if we stop the retry loop by returning false.
    ASSERT_EQ(targetingMetadata.deprioritizedServers.size(), 1);
    ASSERT(targetingMetadata.deprioritizedServers.contains(target1));
}

}  // namespace
}  // namespace mongo
