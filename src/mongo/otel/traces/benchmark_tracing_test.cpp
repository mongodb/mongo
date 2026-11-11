// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/benchmark_tracing.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameter.h"
#include "mongo/otel/telemetry_context.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/tracer_provider_service.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo::otel::traces {
namespace {

bool samplingFlagValue() {
    BSONObjBuilder bob;
    ServerParameterSet::getNodeParameterSet()
        ->get("featureFlagOtelTraceSampling")
        ->append(nullptr, &bob, "v");
    return bob.obj()["v"]["value"].trueValue();
}

/** Starts and ends a root span named `name`, as a command's ingress span would be. */
void runRootSpan(const SpanName& name) {
    std::shared_ptr<TelemetryContext> telemetryCtx;
    auto span = Span::startIngressSpan(telemetryCtx, name);
}

class BenchmarkTracingTest : public unittest::Test {
protected:
    void setUp() override {
        if (!BenchmarkTracingSession::isSupported()) {
            GTEST_SKIP() << "OpenTelemetry is not compiled in";
        }
    }
};

TEST_F(BenchmarkTracingTest, UntracedExportsNothing) {
    BenchmarkTracingSession tracing(SamplingParameters{}, {std::string{"test_only.span1"}});
    runRootSpan(span_names::kTest1);
    EXPECT_EQ(tracing.exportedSpans(), 0);
}

TEST_F(BenchmarkTracingTest, SmallestFactorRegistersSpanAndMissesSampling) {
    BenchmarkTracingSession tracing(
        SamplingParameters{.factor = kSmallestSamplingFactor, .rateLimits = kUnlimitedRateLimit},
        {std::string{"test_only.span1"}});
    runRootSpan(span_names::kTest1);
    // The span name is registered with the sampler, but the roll (unless it is exactly 0.0, which
    // has probability 2^-53) misses the smallest factor, so nothing is exported.
    EXPECT_EQ(tracing.exportedSpans(), 0);
}

TEST_F(BenchmarkTracingTest, TracedExportsListedSpans) {
    BenchmarkTracingSession tracing(
        SamplingParameters{.factor = 1.0, .rateLimits = kUnlimitedRateLimit},
        {std::string{"test_only.span1"}});
    runRootSpan(span_names::kTest1);
    runRootSpan(span_names::kTest1);
    EXPECT_EQ(tracing.exportedSpans(), 2);
}

TEST_F(BenchmarkTracingTest, TracedDoesNotExportUnlistedSpans) {
    BenchmarkTracingSession tracing(
        SamplingParameters{.factor = 1.0, .rateLimits = kUnlimitedRateLimit},
        {std::string{"test_only.span1"}});
    runRootSpan(span_names::kTest1);
    runRootSpan(span_names::kTest2);
    EXPECT_EQ(tracing.exportedSpans(), 1);
}

TEST_F(BenchmarkTracingTest, TracedExportsMoreThanOneBatch) {
    // Spans are serialized in batches; the count must include spans in a partially filled batch.
    BenchmarkTracingSession tracing(
        SamplingParameters{.factor = 1.0, .rateLimits = kUnlimitedRateLimit},
        {std::string{"test_only.span1"}});
    for (int i = 0; i < 1000; ++i) {
        runRootSpan(span_names::kTest1);
    }
    EXPECT_EQ(tracing.exportedSpans(), 1000);
}

TEST_F(BenchmarkTracingTest, RestoresGlobalStateOnDestruction) {
    const bool flagBefore = samplingFlagValue();
    auto* serviceBefore = getGlobalTracerProviderService();
    {
        BenchmarkTracingSession tracing(
            SamplingParameters{.factor = 1.0, .rateLimits = kUnlimitedRateLimit},
            {std::string{"test_only.span1"}});
        EXPECT_TRUE(samplingFlagValue());
        EXPECT_NE(getGlobalTracerProviderService(), serviceBefore);
        EXPECT_EQ(TracingSampler::get().getConfig().defaultSpans.factor, 1.0);
        runRootSpan(span_names::kTest1);
    }
    EXPECT_EQ(samplingFlagValue(), flagBefore);
    EXPECT_EQ(getGlobalTracerProviderService(), serviceBefore);
    const auto config = TracingSampler::get().getConfig();
    EXPECT_EQ(config.defaultSpans.factor, kDefaultFactor);
    EXPECT_TRUE(config.perSpanOverrides.empty());
    EXPECT_EQ(config.externalRateLimits.refillRate, kDefaultRefillRate);
    EXPECT_EQ(config.externalRateLimits.maxTokens, kDefaultMaxTokens);

    // Nothing is traced once the benchmark tracing is gone.
    runRootSpan(span_names::kTest1);
}

DEATH_TEST(BenchmarkTracingDeathTest,
           TracedWithoutSpansFails,
           "Traced benchmark did not export any spans") {
    if (!BenchmarkTracingSession::isSupported()) {
        // Without OpenTelemetry there is nothing to misconfigure; die as expected.
        invariant(false, "Traced benchmark did not export any spans");
    }
    BenchmarkTracingSession tracing(
        SamplingParameters{.factor = 1.0, .rateLimits = kUnlimitedRateLimit},
        {std::string{"test_only.span1"}});
    // A span that is not sampled by this configuration, e.g. a misspelled command name.
    runRootSpan(span_names::kTest2);
}

DEATH_TEST(BenchmarkTracingDeathTest,
           UntracedWithSpansFails,
           "Untraced benchmark unexpectedly exported spans") {
    if (!BenchmarkTracingSession::isSupported()) {
        invariant(false, "Untraced benchmark unexpectedly exported spans");
    }
    BenchmarkTracingSession tracing;
    // Something else turning on sampling would make the "untraced" benchmark trace.
    TracingSampler::get().updateInternalConfig(SamplingParameters{.factor = 1.0},
                                               {{std::string{"test_only.span1"}, {.factor = 1.0}}});
    runRootSpan(span_names::kTest1);
}

}  // namespace
}  // namespace mongo::otel::traces
