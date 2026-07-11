// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/rate_limiting.h"

#include "mongo/unittest/unittest.h"

#include <thread>

namespace mongo {

TEST(RateLimiterTest, SlidingWindowSucceeds) {
    auto rl = RateLimiter();
    rl.configureWindowBased(1);
    ASSERT_TRUE(rl.handle());
}

TEST(RateLimiterTest, SlidingWindowFails) {
    auto rl = RateLimiter();
    rl.configureWindowBased(0);
    ASSERT_FALSE(rl.handle());
}

TEST(RateLimiterTest, SlidingWindowSucceedsThenFails) {
    auto rl = RateLimiter();
    rl.configureWindowBased(1);
    ASSERT_TRUE(rl.handle());
    ASSERT_FALSE(rl.handle());
    ASSERT_FALSE(rl.handle());
}

TEST(RateLimiterTest, SampleBasedSucceeds) {
    // Run in a fresh thread to get a clean thread-local PRNG, seeded deterministically with 0.
    size_t numHandles = 0;
    std::thread([&numHandles] {
        auto rl = RateLimiter();
        rl.configureSampleBased(10 /* 1% */, 0 /* seed */);
        size_t requestCount = 10000;
        for (size_t i = 0; i < requestCount; i++) {
            if (rl.handle()) {
                numHandles++;
            }
        }
    }).join();
    ASSERT_APPROX_EQUAL((double)numHandles, 100, 5 /* variance for randomness */);
}

TEST(RateLimiterTest, SampleBasedFails) {
    auto rl = RateLimiter();
    rl.configureSampleBased(0 /* 0% */, 0 /* seed */);
    ASSERT_FALSE(rl.handle());
}

TEST(RateLimiterTest, PolicyGetter) {
    {
        auto rl = RateLimiter();
        rl.configureWindowBased(100);
        ASSERT_EQ(rl.getSamplingRate(), 100);
        ASSERT_TRUE(rl.getPolicyType() == RateLimiter::PolicyType::kWindowBasedPolicy);
    }
    {
        auto rl = RateLimiter();
        rl.configureSampleBased(10 /* 1% */, 0 /* seed */);
        ASSERT_EQ(rl.getSamplingRate(), 10);
        ASSERT_TRUE(rl.getPolicyType() == RateLimiter::PolicyType::kSampleBasedPolicy);
    }
}

}  // namespace mongo
