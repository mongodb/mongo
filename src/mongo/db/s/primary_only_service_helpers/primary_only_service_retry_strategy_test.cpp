/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
    ASSERT_TRUE(strategy.recordFailureAndEvaluateShouldRetry(kRetriableError, kTarget1, {}));
    ASSERT_EQ(transientErrorCount(), 1);
    ASSERT_EQ(unrecoverableErrorCount(), 0);
}

TEST_F(PrimaryOnlyServiceRetryStrategyTest, NonRetriableErrorDoesNotRetry) {
    auto strategy = makeNeverRetryStrategy();
    ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(kNonRetriableError, kTarget1, {}));
    ASSERT_EQ(transientErrorCount(), 0);
    ASSERT_EQ(unrecoverableErrorCount(), 1);
}

TEST_F(PrimaryOnlyServiceRetryStrategyTest, BackoffNonZeroAfterRetriableError) {
    auto strategy = makeStrategy();
    FailPointEnableBlock fp{"returnMaxBackoffDelay"};

    ASSERT_TRUE(strategy.recordFailureAndEvaluateShouldRetry(kRetriableError, kTarget1, {}));

    auto delay = strategy.getNextRetryDelay();
    ASSERT_EQ(delay, Milliseconds{200});
}

TEST_F(PrimaryOnlyServiceRetryStrategyTest, BackoffGrowsWithSuccessiveRetriableErrors) {
    auto strategy = makeStrategy();

    FailPointEnableBlock fp{"returnMaxBackoffDelay"};

    const auto baseBackoff = gDefaultClientBaseBackoffMillis.loadRelaxed();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(strategy.recordFailureAndEvaluateShouldRetry(kRetriableError, kTarget1, {}));
        auto delay = strategy.getNextRetryDelay();
        auto expectedDelay = Milliseconds{static_cast<int64_t>(baseBackoff * std::exp2(i + 1))};
        ASSERT_EQ(delay, expectedDelay);
    }
}

TEST_F(PrimaryOnlyServiceRetryStrategyTest, BackoffNonZeroAfterSystemOverloadedError) {
    auto strategy = makeStrategy();

    FailPointEnableBlock fp{"returnMaxBackoffDelay"};

    ASSERT_TRUE(strategy.recordFailureAndEvaluateShouldRetry(
        kSystemOverloadedError, kTarget1, kSystemOverloadedErrorLabels));

    auto delay = strategy.getNextRetryDelay();
    ASSERT_GT(delay, Milliseconds{0});
}

}  // namespace
}  // namespace primary_only_service_helpers
}  // namespace mongo

