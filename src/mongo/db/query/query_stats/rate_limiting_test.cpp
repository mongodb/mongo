// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/rate_limiting.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_knob_descriptors_integration.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/query_settings/query_settings.h"
#include "mongo/db/query/query_settings/query_settings_context_test_util.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <thread>

namespace mongo {

class RateLimiterOpCtxTest : public ServiceContextTest {};

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
        rl.configureSampleBased(10 /* 1% */, /*seed=*/0);
        size_t requestCount = 10000;
        for (size_t i = 0; i < requestCount; i++) {
            if (rl.handle()) {
                numHandles++;
            }
        }
    }).join();
    ASSERT_APPROX_EQUAL((double)numHandles, 100, 5 /* variance for randomness */);
}

TEST(RateLimiterTest, OverrideHandleFractionalWindowBased) {
    size_t accepted = 0;
    size_t trials = 20000;
    double acceptanceRate = 0.5;
    std::thread([&accepted, trials, acceptanceRate] {
        auto rl = RateLimiter();
        rl.configureWindowBased(1);
        ASSERT_TRUE(rl.handle());
        for (size_t i = 0; i < trials; i++) {
            if (rl.handle(acceptanceRate)) {
                accepted++;
            }
        }
        ASSERT_FALSE(rl.handle(0.0));
        ASSERT_TRUE(rl.handle(1.0));
        // The override decisions above are stateless with respect to the window: the single
        // admitted request was spent up front, and the window neither gained nor lost budget.
        ASSERT_FALSE(rl.handle());
    }).join();
    ASSERT_APPROX_EQUAL(
        (double)accepted, (double)trials * acceptanceRate, trials * 0.04 /* 4% band */);
}

TEST(RateLimiterTest, OverrideHandleFractionalSampleBased) {
    size_t accepted = 0;
    size_t trials = 20000;
    double acceptanceRate = 0.5;
    std::thread([&accepted, trials, acceptanceRate] {
        auto rl = RateLimiter();
        rl.configureSampleBased(0 /* 0% */, 0 /* seed */);
        for (size_t i = 0; i < trials; i++) {
            if (rl.handle(acceptanceRate)) {
                accepted++;
            }
        }
        ASSERT_FALSE(rl.handle(0.0));
        ASSERT_TRUE(rl.handle(1.0));
        // The cluster-wide rate of 0 still rejects every request regardless of PRNG state.
        ASSERT_FALSE(rl.handle());
    }).join();
    ASSERT_APPROX_EQUAL(
        (double)accepted, (double)trials * acceptanceRate, trials * 0.04 /* 4% band */);
}

TEST(RateLimiterTest, SampleBasedFails) {
    auto rl = RateLimiter();
    rl.configureSampleBased(0 /* 0% */, /*seed=*/0);
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
        rl.configureSampleBased(10 /* 1% */, /*seed=*/0);
        ASSERT_EQ(rl.getSamplingRate(), 10);
        ASSERT_TRUE(rl.getPolicyType() == RateLimiter::PolicyType::kSampleBasedPolicy);
    }
}

TEST_F(RateLimiterOpCtxTest, HandleOpCtxWithoutKnobConsultsClusterPolicy) {
    auto opCtx = makeOperationContext();

    RateLimiter rl;
    rl.configureSampleBased(0 /* 0% */, 0 /* seed */);
    ASSERT_EQ(rl.handle(opCtx.get()), CollectDecision::kDisabled);

    rl.configureSampleBased(1000 /* 100% */, 0 /* seed */);
    ASSERT_EQ(rl.handle(opCtx.get()), CollectDecision::kCollected);
}

TEST_F(RateLimiterOpCtxTest, HandleOpCtxOverrideTakesPrecedenceOverClusterPolicy) {
    auto opCtx = makeOperationContext();

    RateLimiter rl;
    rl.setSamplingRateKnob(&query_knobs::kQueryStatsSampleRate);
    rl.configureSampleBased(0 /* 0% */, 0 /* seed */);

    // No query settings: the cluster-wide policy decides (disabled at rate 0).
    ASSERT_EQ(rl.handle(opCtx.get()), CollectDecision::kDisabled);

    // A full-rate override collects even though the cluster-wide policy would disable.
    query_settings::QuerySettingsGuardForTest collectGuard{
        opCtx.get(), BSON("queryKnobs" << BSON("queryStatsSampleRate" << 1.0))};
    ASSERT_EQ(rl.handle(opCtx.get()), CollectDecision::kCollected);

    // A zero-rate override rejects, and counts as rate limited (collection stays enabled). The
    // knob configuration is frozen at first resolution for an operation's lifetime, so reset it
    // to observe the swapped guard's settings.
    QueryKnobConfiguration::reset_forTest(opCtx.get());
    query_settings::QuerySettingsGuardForTest limitGuard{
        opCtx.get(), BSON("queryKnobs" << BSON("queryStatsSampleRate" << 0.0))};
    ASSERT_EQ(rl.handle(opCtx.get()), CollectDecision::kRateLimited);
}

TEST_F(RateLimiterOpCtxTest, HandleOpCtxWithoutKnobIgnoresSamplingRateOverride) {
    auto opCtx = makeOperationContext();

    // A limiter without a knob (the write-command limiter until SERVER-134995) must not consult
    // query settings, even when settings carry the sampling-rate knob.
    RateLimiter rl;
    rl.configureSampleBased(1000 /* 100% */, 0 /* seed */);
    query_settings::QuerySettingsGuardForTest settingsGuard{
        opCtx.get(), BSON("queryKnobs" << BSON("queryStatsSampleRate" << 0.0))};
    ASSERT_EQ(rl.handle(opCtx.get()), CollectDecision::kCollected);
}

}  // namespace mongo
