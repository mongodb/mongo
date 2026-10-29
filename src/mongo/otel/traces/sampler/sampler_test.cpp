// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/sampler/sampler.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/time_support.h"

namespace mongo::otel::traces {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

TEST(SamplerTest, UnregisteredSpanIsNeverSampled) {
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0}, {});
    EXPECT_FALSE(sampler.shouldSample("unknown.span", 0.0));
}

TEST(SamplerTest, SeedBelowFactorSamples) {
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.5}, {});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
}

TEST(SamplerTest, SeedEqualToFactorDrops) {
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.5}, {});
    sampler.sampleByDefault(span_names::kTest1);
    // sampleValue < factor, so equality is not sampled.
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));
}

TEST(SamplerTest, SeedAboveFactorDrops) {
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.5}, {});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
}

TEST(SamplerTest, UpdateConfigChangesDecisionsImmediately) {
    TracingSamplerImpl sampler;
    sampler.sampleByDefault(span_names::kTest1);

    sampler.updateInternalConfig(SamplingParameters{.factor = 0.5}, {});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));

    sampler.updateInternalConfig(SamplingParameters{.factor = 0.1}, {});
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
}

TEST(SamplerTest, RegisterSpanMakesItSampleable) {
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0}, {});

    EXPECT_FALSE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));

    sampler.sampleByDefault(span_names::kTest2);

    EXPECT_TRUE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));
}

TEST(SamplerTest, RegisteredSpanUsesCurrentDefaultFactor) {
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.5}, {});
    sampler.sampleByDefault(span_names::kTest1);

    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
}

TEST(SamplerTest, ZeroFactorDropsAllRegisteredSpans) {
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.0}, {});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, OneFactorSamplesAllRegisteredSpans) {
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0}, {});
    sampler.sampleByDefault(span_names::kTest1);
    // Any value in [0, 1) is strictly less than 1.0.
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.999));
}

TEST(SamplerTest, DuplicateRegistrationHasNoEffect) {
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.5}, {});
    sampler.sampleByDefault(span_names::kTest1);
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
}

TEST(SamplerTest, MultipleSpansAreIndependentlySampled) {
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.5}, {});
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
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0, .rateLimits = {.maxTokens = 1}},
                                 {});
    sampler.sampleByDefault(span_names::kTest1);
    // First call consumes the single default token.
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    // Second call at the same frozen time finds the bucket empty.
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, RateLimitersAreIndependentPerSpan) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0, .rateLimits = {.maxTokens = 1}},
                                 {});
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
    sampler.updateInternalConfig(
        SamplingParameters{.factor = 1.0, .rateLimits = {.refillRate = 1000.0, .maxTokens = 1000}},
        {});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    // Reduce the existing limiter to a 1-token burst. BasicTokenBucket::reset preserves available
    // tokens but clamps them to the new burstSize (1), so exactly 1 more token can be consumed.
    sampler.updateInternalConfig(
        SamplingParameters{.factor = 1.0, .rateLimits = {.refillRate = 1.0, .maxTokens = 1}}, {});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, RateLimiterWithLargerBurstSize) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateInternalConfig(
        SamplingParameters{.factor = 1.0, .rateLimits = {.refillRate = 1000.0, .maxTokens = 1000}},
        {});
    sampler.sampleByDefault(span_names::kTest3);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(sampler.shouldSample(span_names::kTest3.getName(), 0.0));
    }
}

TEST(SamplerTest, PerSpanOverrideBeatsDefault) {
    TracingSamplerImpl sampler;
    sampler.sampleByDefault(span_names::kTest1);
    // Default is 0.0 (would never sample), but override for kTest1 is 1.0.
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.0},
                                 {{std::string(span_names::kTest1.getName()), {.factor = 1.0}}});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));
}

TEST(SamplerTest, PerSpanOverrideOnlyAffectsNamedSpan) {
    TracingSamplerImpl sampler;
    sampler.sampleByDefault(span_names::kTest1);
    sampler.sampleByDefault(span_names::kTest2);
    // Override only kTest1; kTest2 stays at default=0.0.
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.0},
                                 {{std::string(span_names::kTest1.getName()), {.factor = 1.0}}});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest2.getName(), 0.5));
}

TEST(SamplerTest, UpdateConfigWithNewOverrideTakesEffectImmediately) {
    TracingSamplerImpl sampler;
    sampler.sampleByDefault(span_names::kTest1);
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0}, {});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));

    // Add a per-span override that suppresses kTest1.
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0},
                                 {{std::string(span_names::kTest1.getName()), {.factor = 0.0}}});
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, OverrideOnUnregisteredSpanIsApplied) {
    TracingSamplerImpl sampler;
    // kTest1 is never registered via sampleByDefault.
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.0},
                                 {{std::string(span_names::kTest1.getName()), {.factor = 1.0}}});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));
}

TEST(SamplerTest, PerSpanOverrideUsesItsOwnRateLimits) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.sampleByDefault(span_names::kTest1);
    sampler.updateInternalConfig(
        SamplingParameters{.factor = 1.0, .rateLimits = {.refillRate = 1000.0, .maxTokens = 1000}},
        {{std::string(span_names::kTest1.getName()),
          {.factor = 1.0, .rateLimits = {.refillRate = 1.0, .maxTokens = 1}}}});
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
    sampler.updateInternalConfig(
        SamplingParameters{.factor = 1.0, .rateLimits = {.refillRate = 1.0, .maxTokens = 2}},
        {{std::string(span_names::kTest1.getName()),
          {.factor = 1.0, .rateLimits = {.refillRate = 1.0, .maxTokens = 5}}},
         {std::string(span_names::kTest2.getName()),
          {.factor = 1.0, .rateLimits = {.refillRate = 1.0, .maxTokens = 10}}}});

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

TEST(SamplerTest, ExternalSpanSamplesWithoutSpanRegistration) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    // isExternal=true bypasses span name registration and sampling factor checks entirely.
    EXPECT_TRUE(sampler.shouldAcceptExternalTrace());
}

TEST(SamplerTest, ExternalSpanBlocksAfterTokensExhausted) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateExternalConfig({.refillRate = 1.0, .maxTokens = 1});
    EXPECT_TRUE(sampler.shouldAcceptExternalTrace());
    EXPECT_FALSE(sampler.shouldAcceptExternalTrace());
}

TEST(SamplerTest, ExternalAndInternalRateLimitersAreIndependent) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0, .rateLimits = {.maxTokens = 1}},
                                 {});
    sampler.updateExternalConfig({.maxTokens = 1});
    sampler.sampleByDefault(span_names::kTest1);

    // Consuming the external token does not affect the internal token.
    EXPECT_TRUE(sampler.shouldAcceptExternalTrace());
    EXPECT_FALSE(sampler.shouldAcceptExternalTrace());
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, UpdateExternalConfigChangesRateLimit) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateExternalConfig({.refillRate = 1.0, .maxTokens = 2});
    EXPECT_TRUE(sampler.shouldAcceptExternalTrace());
    EXPECT_TRUE(sampler.shouldAcceptExternalTrace());
    EXPECT_FALSE(sampler.shouldAcceptExternalTrace());
}

TEST(SamplerTest, GetExternalConfigReturnsCurrentValues) {
    TracingSamplerImpl sampler;
    sampler.updateExternalConfig({.refillRate = 5.0, .maxTokens = 20});
    auto externalRateLimits = sampler.getConfig().externalRateLimits;
    EXPECT_DOUBLE_EQ(externalRateLimits.refillRate, 5.0);
    EXPECT_EQ(externalRateLimits.maxTokens, 20);
}

TEST(SamplerTest, ExternalRateLimiterPreservedAfterRebuild) {
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateExternalConfig({.refillRate = 1.0, .maxTokens = 2});

    // Consume one of two external tokens.
    EXPECT_TRUE(sampler.shouldAcceptExternalTrace());

    // Trigger a rebuild; the external rate limiter shared_ptr is reused, not recreated.
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0}, {});

    // One token remains after the rebuild.
    EXPECT_TRUE(sampler.shouldAcceptExternalTrace());
    EXPECT_FALSE(sampler.shouldAcceptExternalTrace());
}

TEST(SamplerTest, StatsStartAtZero) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not configured";
    }
    TracingSamplerImpl sampler;
    const auto stats = sampler.getStats();
    EXPECT_EQ(stats.internalSpans.admitted, 0);
    EXPECT_EQ(stats.internalSpans.rejected, 0);
    EXPECT_EQ(stats.externalSpan.admitted, 0);
    EXPECT_EQ(stats.externalSpan.rejected, 0);
}

TEST(SamplerTest, InternalStatsRecordAdmissionAndRejection) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not configured";
    }
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0, .rateLimits = {.maxTokens = 1}},
                                 {});
    sampler.sampleByDefault(span_names::kTest1);

    // First call consumes the single token and is admitted.
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    auto stats = sampler.getStats();
    EXPECT_EQ(stats.internalSpans.admitted, 1);
    EXPECT_EQ(stats.internalSpans.rejected, 0);

    // Second call at the frozen time finds the bucket empty and is rejected.
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    stats = sampler.getStats();
    EXPECT_EQ(stats.internalSpans.admitted, 1);
    EXPECT_EQ(stats.internalSpans.rejected, 1);

    // External stats are untouched by internal sampling.
    EXPECT_EQ(stats.externalSpan.admitted, 0);
    EXPECT_EQ(stats.externalSpan.rejected, 0);
}

TEST(SamplerTest, InternalStatsUnchangedForUnregisteredSpan) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not configured";
    }
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0}, {});

    // An unregistered span returns before reaching the rate limiter, so no stat is updated.
    EXPECT_FALSE(sampler.shouldSample("unknown.span", 0.0));
    const auto stats = sampler.getStats();
    EXPECT_EQ(stats.internalSpans.admitted, 0);
    EXPECT_EQ(stats.internalSpans.rejected, 0);
}

TEST(SamplerTest, InternalStatsUnchangedWhenDroppedByFactor) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not configured";
    }
    TracingSamplerImpl sampler;
    sampler.updateInternalConfig(SamplingParameters{.factor = 0.5}, {});
    sampler.sampleByDefault(span_names::kTest1);

    // sampleValue >= factor is dropped before reaching the rate limiter, so no stat is updated.
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
    const auto stats = sampler.getStats();
    EXPECT_EQ(stats.internalSpans.admitted, 0);
    EXPECT_EQ(stats.internalSpans.rejected, 0);
}

TEST(SamplerTest, ExternalStatsRecordAdmissionAndRejection) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not configured";
    }
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateExternalConfig({.refillRate = 1.0, .maxTokens = 1});

    // First call consumes the single external token and is admitted.
    EXPECT_TRUE(sampler.shouldAcceptExternalTrace());
    auto stats = sampler.getStats();
    EXPECT_EQ(stats.externalSpan.admitted, 1);
    EXPECT_EQ(stats.externalSpan.rejected, 0);

    // Second call at the frozen time finds the bucket empty and is rejected.
    EXPECT_FALSE(sampler.shouldAcceptExternalTrace());
    stats = sampler.getStats();
    EXPECT_EQ(stats.externalSpan.admitted, 1);
    EXPECT_EQ(stats.externalSpan.rejected, 1);

    // Internal stats are untouched by external sampling.
    EXPECT_EQ(stats.internalSpans.admitted, 0);
    EXPECT_EQ(stats.internalSpans.rejected, 0);
}

TEST(SamplerTest, StatsAccumulateAcrossMultipleCalls) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not configured";
    }
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    // Internal limiter with 3 tokens, external limiter with 2 tokens; frozen clock, no refills.
    sampler.updateInternalConfig(
        SamplingParameters{.factor = 1.0, .rateLimits = {.refillRate = 1.0, .maxTokens = 3}}, {});
    sampler.updateExternalConfig({.refillRate = 1.0, .maxTokens = 2});
    sampler.sampleByDefault(span_names::kTest1);

    // 5 internal calls: 3 admitted, 2 rejected.
    for (int i = 0; i < 5; ++i) {
        sampler.shouldSample(span_names::kTest1.getName(), 0.0);
    }
    auto stats = sampler.getStats();
    EXPECT_EQ(stats.internalSpans.admitted, 3);
    EXPECT_EQ(stats.internalSpans.rejected, 2);

    // 4 external calls: 2 admitted, 2 rejected.
    for (int i = 0; i < 4; ++i) {
        sampler.shouldAcceptExternalTrace();
    }
    stats = sampler.getStats();
    EXPECT_EQ(stats.externalSpan.admitted, 2);
    EXPECT_EQ(stats.externalSpan.rejected, 2);
}

TEST(SamplerTest, StatsAccumulateAcrossDifferentSpans) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not configured";
    }
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    // Default internal limiters with 3 tokens, override limiter with 2 tokens.
    sampler.updateInternalConfig(
        SamplingParameters{.factor = 1.0, .rateLimits = {.refillRate = 1.0, .maxTokens = 3}},
        {{std::string(span_names::kTest3.getName()),
          {.factor = 1.0, .rateLimits = {.refillRate = 1.0, .maxTokens = 2}}}});
    sampler.sampleByDefault(span_names::kTest1);
    sampler.sampleByDefault(span_names::kTest2);

    // 5 internal calls:
    // 3 accepted and 2 rejected for kTest1.
    // 3 accepted and 2 rejected for kTest2.
    // 2 accepted and 3 rejected for kTest3.
    for (int i = 0; i < 5; ++i) {
        sampler.shouldSample(span_names::kTest1.getName(), 0.0);
        sampler.shouldSample(span_names::kTest2.getName(), 0.0);
        sampler.shouldSample(span_names::kTest3.getName(), 0.0);
    }
    auto stats = sampler.getStats();
    EXPECT_EQ(stats.internalSpans.admitted, 8);
    EXPECT_EQ(stats.internalSpans.rejected, 7);
}

TEST(SamplerTest, InternalOtelMetricsAggregatedAcrossSpans) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not configured";
    }

    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    // Two default spans with a 1-token limiter each
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0, .rateLimits = {.maxTokens = 1}},
                                 {});
    sampler.sampleByDefault(span_names::kTest1);
    sampler.sampleByDefault(span_names::kTest2);

    for (int i = 0; i < 2; ++i) {
        sampler.shouldSample(span_names::kTest1.getName(), 0.0);
        sampler.shouldSample(span_names::kTest2.getName(), 0.0);
    }

    // All internal spans report into a single shared instrument pair, so admissions and rejections
    // are aggregated across spans: each of the two spans admits its first call and rejects its
    // second, giving 2 admitted and 2 rejected in total.
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kOtelTracingSamplerInternalSpanRateLimiterSuccessfulAdmissions),
              2);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kOtelTracingSamplerInternalSpanRateLimiterRejectedAdmissions),
              2);

    // The external instruments stay untouched.
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kOtelTracingSamplerExternalSpanRateLimiterSuccessfulAdmissions),
              0);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kOtelTracingSamplerExternalSpanRateLimiterRejectedAdmissions),
              0);
}

TEST(SamplerTest, ExternalOtelMetricsRecordAdmissionAndRejection) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not configured";
    }

    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    sampler.updateExternalConfig({.refillRate = 1.0, .maxTokens = 1});

    // First call is admitted, second is rejected.
    sampler.shouldAcceptExternalTrace();
    sampler.shouldAcceptExternalTrace();

    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kOtelTracingSamplerExternalSpanRateLimiterSuccessfulAdmissions),
              1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kOtelTracingSamplerExternalSpanRateLimiterRejectedAdmissions),
              1);
}

TEST(SamplerTest, ConcurrentAccessTest) {
    unittest::ThreadAssertionMonitor monitor;
    monitor
        .spawnController([&] {
            TracingSamplerImpl sampler;
            sampler.sampleByDefault(span_names::kTest1);
            sampler.updateInternalConfig(SamplingParameters{.factor = 1.0}, {});

            std::vector<stdx::thread> threads;
            auto launchThread = [&](auto f) {
                threads.push_back(monitor.spawn([f = std::move(f)] { f(); }));
            };

            launchThread(
                [&] { sampler.updateInternalConfig(SamplingParameters{.factor = 0.5}, {}); });
            launchThread([&] { sampler.sampleByDefault(span_names::kTest2); });
            launchThread([&] { sampler.shouldSample(span_names::kTest1.getName(), 0.3); });
            launchThread([&] { sampler.shouldAcceptExternalTrace(); });
            launchThread(
                [&] { sampler.updateExternalConfig({.refillRate = 2.5, .maxTokens = 15}); });

            for (auto& t : threads)
                t.join();
        })
        .join();
}

TEST(SamplerTest, InternalRateLimiterNotCreatedUntilSpanFires) {
    TracingSamplerImpl sampler;

    sampler.sampleByDefault(span_names::kTest1);
    sampler.updateInternalConfig(SamplingParameters{.factor = 1.0},
                                 {{std::string(span_names::kTest2.getName()), {.factor = 1.0}}});

    // Rate limiters are created lazily, so until a span actually fires, none exist.
    EXPECT_EQ(sampler.getNumInternalRateLimiters(), 0);

    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
    EXPECT_EQ(sampler.getNumInternalRateLimiters(), 1);

    EXPECT_TRUE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));
    EXPECT_EQ(sampler.getNumInternalRateLimiters(), 2);
}

TEST(SamplerTest, ConcurrentFirstSightCreatesSingleInternalRateLimiter) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        GTEST_SKIP() << "OTel not configured";
    }
    TickSourceMock<Milliseconds> clock;
    TracingSamplerImpl sampler(&clock);
    // Give the span a 1-token bucket and freeze the clock so no tokens ever refill.
    sampler.sampleByDefault(span_names::kTest1);
    sampler.updateInternalConfig(
        SamplingParameters{.factor = 1.0, .rateLimits = {.refillRate = 1.0, .maxTokens = 1}}, {});

    constexpr int kNumThreads = 16;
    unittest::threadAssertionMonitoredTest([&](auto& monitor) {
        std::vector<stdx::thread> threads;
        for (int i = 0; i < kNumThreads; ++i) {
            threads.push_back(
                monitor.spawn([&] { sampler.shouldSample(span_names::kTest1.getName(), 0.0); }));
        }
        for (auto& t : threads) {
            t.join();
        }
    });

    EXPECT_EQ(sampler.getNumInternalRateLimiters(), 1);

    // All test threads above fire the same span, so if they share one limiter, we get exactly one
    // admission.
    const auto stats = sampler.getStats();
    EXPECT_EQ(stats.internalSpans.admitted, 1);
    EXPECT_EQ(stats.internalSpans.rejected, kNumThreads - 1);
}

}  // namespace
}  // namespace mongo::otel::traces
