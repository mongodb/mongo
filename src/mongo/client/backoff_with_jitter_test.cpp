// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/backoff_with_jitter.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

#include <cmath>
#include <cstdint>

#include <boost/none.hpp>
#include <boost/optional.hpp>

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

TEST_F(BackoffWithJitterTest, BackoffOverrideFirstAttemptDoublesOverrideValue) {
    auto _ = FailPointEnableBlock{"returnMaxBackoffDelay"};

    constexpr Milliseconds kOverride{1000};
    backoffWithJitter.setAttemptCount_forTest(1);

    const auto backoff = backoffWithJitter.getBackoffDelay(kOverride);
    ASSERT_EQ(backoff, std::min(kMaxBackoff, Milliseconds{kOverride.count() * 2}));
}

TEST_F(BackoffWithJitterTest, BackoffOverrideSecondAttemptQuadruplesOverrideValue) {
    auto _ = FailPointEnableBlock{"returnMaxBackoffDelay"};

    constexpr Milliseconds kOverride{1000};
    backoffWithJitter.setAttemptCount_forTest(2);

    const auto backoff = backoffWithJitter.getBackoffDelay(kOverride);
    ASSERT_EQ(backoff, std::min(kMaxBackoff, Milliseconds{kOverride.count() * 4}));
}

TEST_F(BackoffWithJitterTest, BackoffOverrideZeroAttemptCountReturnsZero) {
    auto _ = FailPointEnableBlock{"returnMaxBackoffDelay"};

    constexpr Milliseconds kOverride{1000};
    backoffWithJitter.setAttemptCount_forTest(0);

    const auto backoff = backoffWithJitter.getBackoffDelay(kOverride);
    ASSERT_EQ(backoff, Milliseconds{0});
}

TEST_F(BackoffWithJitterTest, BackoffOverrideNoneMatchesNoArgOverload) {
    auto _ = FailPointEnableBlock{"returnMaxBackoffDelay"};

    backoffWithJitter.setAttemptCount_forTest(3);

    const auto noArgResult = backoffWithJitter.getBackoffDelay();
    const auto overloadResult = backoffWithJitter.getBackoffDelay(boost::none);
    ASSERT_EQ(noArgResult, overloadResult);
    ASSERT_EQ(overloadResult, Milliseconds{800});
}

TEST_F(BackoffWithJitterTest, BackoffOverrideCapsAtMaxBackoff) {
    auto _ = FailPointEnableBlock{"returnMaxBackoffDelay"};

    constexpr Milliseconds kOverride{10000};
    backoffWithJitter.setAttemptCount_forTest(5);

    const auto backoff = backoffWithJitter.getBackoffDelay(kOverride);
    ASSERT_EQ(backoff, kMaxBackoff);
}

}  // namespace
}  // namespace mongo
