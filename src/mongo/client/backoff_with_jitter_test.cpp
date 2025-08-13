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

#include "mongo/client/backoff_with_jitter.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <cmath>
#include <cstdint>

namespace mongo {
namespace {

class BackoffWithJitterTest : public unittest::Test {
public:
    void setUp() override {
        backoffWithJitter.initRandomEngineWithSeed_forTest(kKnownSeed);
    }

    std::int64_t exp2i(std::int64_t n) {
        return static_cast<std::int64_t>(std::exp2(n));
    }

    static constexpr Milliseconds kBaseBackoff{100};
    static constexpr Milliseconds kMaxBackoff{4000};
    static constexpr std::int32_t kKnownSeed = 12345;
    static constexpr std::int32_t kConfidence = 32;

    BackoffWithJitter backoffWithJitter{kBaseBackoff, kMaxBackoff};
};

TEST_F(BackoffWithJitterTest, BackoffWithJitterIncrementTest) {
    constexpr std::int32_t maxAttempt = 8;

    // Basic test that validate that the backoff with jitter is always within bounds.
    // It should never exceed base * 2^nthAttempt and it should also never exceed maxBackoff.
    for (int nthAttempt = 0; nthAttempt < maxAttempt; ++nthAttempt) {
        const auto maxBackoffAtNthAttempt = kBaseBackoff * exp2i(nthAttempt);

        // Validate that even when calling getBackoffDelay multiple times, we're always within
        // bounds.
        for (int confidence = 0; confidence < kConfidence; ++confidence) {
            ASSERT_LTE(backoffWithJitter.getBackoffDelay(), kMaxBackoff);
            ASSERT_LTE(backoffWithJitter.getBackoffDelay(), maxBackoffAtNthAttempt);
        }

        const auto backoff = backoffWithJitter.getBackoffDelayAndIncrementAttemptCount();

        if (nthAttempt == 0) {
            ASSERT_EQ(backoff, Milliseconds{0});
            continue;
        }

        // For this known seed, we're always jittering above 0
        ASSERT_GT(backoff, Milliseconds{0});
    }
}

// TODO: SERVER-109201 Make this test verify specific values instead of relying on random
// distributions with fixed seed.
TEST_F(BackoffWithJitterTest, BackoffWithJitterExponentialGrowth) {
    constexpr std::int32_t maxAttempt = 8;

    // Test that verifies that the exponential growth does happen by jittering enough time to find
    // a backoff higher than the mean backoff for the nth attempt.
    for (int nthAttempt = 0; nthAttempt < maxAttempt; ++nthAttempt) {
        backoffWithJitter.incrementAttemptCount();
        const auto meanBackoffAtAttempt =
            std::min(kBaseBackoff * exp2i(nthAttempt - 1), kMaxBackoff / 2);

        // This is still deterministic, but the backoff delay does return waits that are
        // implementation defined due to std::uniform_int_distribution. Since we use a known
        // seed, it will always yield the same result, but the amount of attempt is unknown at
        // compile time. 8 was found to always jitter above the mean at least once.
        std::int32_t jitteredAboveMean = 0;
        for (std::int32_t i = 0; i < kConfidence; ++i) {
            const auto backoff = backoffWithJitter.getBackoffDelay();
            if (backoff > meanBackoffAtAttempt) {
                ++jitteredAboveMean;
                break;
            }
        }
        ASSERT_GT(jitteredAboveMean, 0);
    }
}

TEST_F(BackoffWithJitterTest, BackoffWithJitterNoOverflow) {
    constexpr Milliseconds veryLargeTime{0xffffffff};
    backoffWithJitter = BackoffWithJitter{veryLargeTime, veryLargeTime};
    backoffWithJitter.setAttemptCount_forTest(veryLargeTime.count());

    ASSERT_LTE(backoffWithJitter.getBackoffDelay(), veryLargeTime);
}

}  // namespace
}  // namespace mongo
