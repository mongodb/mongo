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

#include "mongo/otel/traces/sampler/sampler.h"

#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo::otel::traces {
namespace {

TEST(SamplerTest, UnregisteredSpanIsNeverSampled) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 1.0}});
    EXPECT_FALSE(sampler.shouldSample("unknown.span", 0.0));
}

TEST(SamplerTest, SeedBelowFactorSamples) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 0.5}});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
}

TEST(SamplerTest, SeedEqualToFactorDrops) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 0.5}});
    sampler.sampleByDefault(span_names::kTest1);
    // sampleValue < factor, so equality is not sampled.
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));
}

TEST(SamplerTest, SeedAboveFactorDrops) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 0.5}});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
}

TEST(SamplerTest, UpdateConfigChangesDecisionsImmediately) {
    TracingSamplerImpl sampler;
    sampler.sampleByDefault(span_names::kTest1);

    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 0.5}});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));

    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 0.1}});
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
}

TEST(SamplerTest, RegisterSpanMakesItSampleable) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 1.0}});

    EXPECT_FALSE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));

    sampler.sampleByDefault(span_names::kTest2);

    EXPECT_TRUE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));
}

TEST(SamplerTest, RegisteredSpanUsesCurrentDefaultFactor) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 0.5}});
    sampler.sampleByDefault(span_names::kTest1);

    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
}

TEST(SamplerTest, ZeroFactorDropsAllRegisteredSpans) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 0.0}});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, OneFactorSamplesAllRegisteredSpans) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 1.0}});
    sampler.sampleByDefault(span_names::kTest1);
    // Any value in [0, 1) is strictly less than 1.0.
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.999));
}

TEST(SamplerTest, DuplicateRegistrationHasNoEffect) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 0.5}});
    sampler.sampleByDefault(span_names::kTest1);
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
}

TEST(SamplerTest, MultipleSpansAreIndependentlySampled) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 0.5}});
    sampler.sampleByDefault(span_names::kTest1);
    sampler.sampleByDefault(span_names::kTest2);

    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest2.getName(), 0.3));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest2.getName(), 0.7));
}

TEST(SamplerTest, RateLimiterBlocksAfterTokensExhausted) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 1.0, .maxTokens = 1}});
    sampler.sampleByDefault(span_names::kTest1);
    // First call consumes the single default token.
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    // Second call at the same frozen time finds the bucket empty.
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, RateLimitersAreIndependentPerSpan) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 1.0, .maxTokens = 1}});
    sampler.sampleByDefault(span_names::kTest1);
    sampler.sampleByDefault(span_names::kTest2);
    // Each span gets its own token bucket; consuming kTest1's token does not drain kTest2's.
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));
}

TEST(SamplerTest, ApplyRateLimitParamsClampsBurstOnExistingLimiter) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    // Create limiter with 1000-token bucket.
    sampler.updateConfig(
        SamplingConfig{.defaultSpans = {.factor = 1.0, .refillRate = 1000.0, .maxTokens = 1000}});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    // Reduce the existing limiter to a 1-token burst. BasicTokenBucket::reset preserves available
    // tokens but clamps them to the new burstSize (1), so exactly 1 more token can be consumed.
    sampler.updateConfig(
        SamplingConfig{.defaultSpans = {.factor = 1.0, .refillRate = 1.0, .maxTokens = 1}});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, RateLimiterWithLargerBurstSize) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateConfig(
        SamplingConfig{.defaultSpans = {.factor = 1.0, .refillRate = 1000.0, .maxTokens = 1000}});
    sampler.sampleByDefault(span_names::kTest3);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(sampler.shouldSample(span_names::kTest3.getName(), 0.0));
    }
}

TEST(SamplerTest, PerSpanOverrideBeatsDefault) {
    TracingSamplerImpl sampler;
    sampler.sampleByDefault(span_names::kTest1);
    // Default is 0.0 (would never sample), but override for kTest1 is 1.0.
    sampler.updateConfig(SamplingConfig{
        .defaultSpans = {.factor = 0.0},
        .perSpanOverrides = {{std::string(span_names::kTest1.getName()), {.factor = 1.0}}}});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));
}

TEST(SamplerTest, PerSpanOverrideOnlyAffectsNamedSpan) {
    TracingSamplerImpl sampler;
    sampler.sampleByDefault(span_names::kTest1);
    sampler.sampleByDefault(span_names::kTest2);
    // Override only kTest1; kTest2 stays at default=0.0.
    sampler.updateConfig(SamplingConfig{
        .defaultSpans = {.factor = 0.0},
        .perSpanOverrides = {{std::string(span_names::kTest1.getName()), {.factor = 1.0}}}});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest2.getName(), 0.5));
}

TEST(SamplerTest, UpdateConfigWithNewOverrideTakesEffectImmediately) {
    TracingSamplerImpl sampler;
    sampler.sampleByDefault(span_names::kTest1);
    sampler.updateConfig(SamplingConfig{.defaultSpans = {.factor = 1.0}});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));

    // Add a per-span override that suppresses kTest1.
    sampler.updateConfig(SamplingConfig{
        .defaultSpans = {.factor = 1.0},
        .perSpanOverrides = {{std::string(span_names::kTest1.getName()), {.factor = 0.0}}}});
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, OverrideOnUnregisteredSpanIsApplied) {
    TracingSamplerImpl sampler;
    // kTest1 is never registered via sampleByDefault.
    sampler.updateConfig(SamplingConfig{
        .defaultSpans = {.factor = 0.0},
        .perSpanOverrides = {{std::string(span_names::kTest1.getName()), {.factor = 1.0}}}});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));
}

TEST(SamplerTest, PerSpanOverrideUsesItsOwnRateLimits) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.sampleByDefault(span_names::kTest1);
    sampler.updateConfig(
        SamplingConfig{.defaultSpans = {.factor = 1.0, .refillRate = 1000.0, .maxTokens = 1000},
                       .perSpanOverrides = {{std::string(span_names::kTest1.getName()),
                                             {.factor = 1.0, .refillRate = 1.0, .maxTokens = 1}}}});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, DefaultAndPerSpanRateLimitersEnforceIndependentBudgets) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    // kTest3 uses the default limiter (2 tokens).
    // kTest1 and kTest2 have per-span overrides with 5 and 10 tokens respectively.
    // The clock is frozen throughout, so no tokens are refilled.
    sampler.sampleByDefault(span_names::kTest3);
    sampler.updateConfig(SamplingConfig{
        .defaultSpans = {.factor = 1.0, .refillRate = 1.0, .maxTokens = 2},
        .perSpanOverrides = {{std::string(span_names::kTest1.getName()),
                              {.factor = 1.0, .refillRate = 1.0, .maxTokens = 5}},
                             {std::string(span_names::kTest2.getName()),
                              {.factor = 1.0, .refillRate = 1.0, .maxTokens = 10}}}});

    // Default rate limiter (kTest3): exactly 2 tokens.
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest3.getName(), 0.0));
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest3.getName(), 0.0));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest3.getName(), 0.0));

    // Per-span override (kTest1): exactly 5 tokens, independent of the others.
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    }
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));

    // Per-span override (kTest2): exactly 10 tokens, independent of the others.
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));
    }
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));
}


}  // namespace
}  // namespace mongo::otel::traces
