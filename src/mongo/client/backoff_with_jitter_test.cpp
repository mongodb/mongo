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
#include "mongo/util/fail_point.h"

#include <cmath>
#include <cstdint>

namespace mongo {
namespace {

class BackoffWithJitterTest : public unittest::Test {
public:
    static constexpr Milliseconds kBaseBackoff{100};
    static constexpr Milliseconds kMaxBackoff{4000};

    BackoffWithJitter backoffWithJitter{kBaseBackoff, kMaxBackoff};
};

TEST_F(BackoffWithJitterTest, BackoffWithJitterIncrementTest) {
    auto _ = FailPointEnableBlock{"returnMaxBackoffDelay"};

    // This array contains the expected values for backoff with jitter disabled.
    // The exponential backoff is bound by the max backoff so the last values are expected to be
    // exactly the max backoff.
    constexpr auto expectedValues = std::array{
        Milliseconds{0},
        Milliseconds{200},
        Milliseconds{400},
        Milliseconds{800},
        Milliseconds{1600},
        Milliseconds{3200},
        kMaxBackoff,
        kMaxBackoff,
        kMaxBackoff,
    };

    for (const auto expectedValue : expectedValues) {
        const auto backoff = backoffWithJitter.getBackoffDelayAndIncrementAttemptCount();
        ASSERT_EQ(backoff, expectedValue);
    }
}

TEST_F(BackoffWithJitterTest, BackoffWithJitterDoesJitter) {
    backoffWithJitter.setAttemptCount_forTest(4);

    constexpr std::uint32_t kKnownSeed = 0xc0ffee;
    backoffWithJitter.initRandomEngineWithSeed_forTest(kKnownSeed);

    constexpr std::size_t kNumAttempts = 4;
    std::set<Milliseconds> delays;
    for (std::size_t i = 0; i < kNumAttempts; ++i) {
        const auto delay = backoffWithJitter.getBackoffDelay();
        ASSERT_FALSE(delays.contains(delay));
        delays.insert(delay);
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
