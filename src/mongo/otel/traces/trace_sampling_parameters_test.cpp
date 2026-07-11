// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/otel/traces/trace_sampling_parameters_gen.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

#include <boost/optional.hpp>
#include <gmock/gmock.h>

namespace mongo::otel::traces {
namespace {
using namespace std::literals::string_view_literals;

using mongo::unittest::match::StatusIsOK;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::FieldsAre;
using ::testing::Not;
using ::testing::Pair;

class TraceSamplingParametersTest : public unittest::Test {
public:
    OpenTelemetryTracingSamplingServerParameter param{"openTelemetryTracingSampling",
                                                      ServerParameterType::kRuntimeOnly};

    void setUp() override {
        TracingSampler::get().updateInternalConfig(SamplingParameters{},
                                                   StringMap<SamplingParameters>{});
    }

    Status setDefaultFactor(double factor) {
        // Wrap in a parent object to obtain a BSONElement of object type.
        auto storage = BSON("v" << BSON("defaultSampling" << BSON("samplingFactor" << factor)));
        return param.set(storage.firstElement(), /*tenantId=*/boost::none);
    }

    Status setRateLimit(double refillRate, int maxTokens) {
        auto storage =
            BSON("v" << BSON("defaultSampling"
                             << BSON("tokenBucketRateLimit" << BSON(
                                         "refillRate" << refillRate << "maxTokens" << maxTokens))));
        return param.set(storage.firstElement(), /*tenantId=*/boost::none);
    }

    double readDefaultFactor(const BSONObj& result) {
        return result["openTelemetryTracingSampling"]["defaultSampling"]["samplingFactor"].Double();
    }

    double readRefillRate(const BSONObj& result) {
        return result["openTelemetryTracingSampling"]["defaultSampling"]["tokenBucketRateLimit"]
                     ["refillRate"]
                         .Double();
    }

    int readMaxTokens(const BSONObj& result) {
        return result["openTelemetryTracingSampling"]["defaultSampling"]["tokenBucketRateLimit"]
                     ["maxTokens"]
                         .Int();
    }
};

TEST_F(TraceSamplingParametersTest, SetUpdatesGlobalSampler) {
    ASSERT_OK(setDefaultFactor(0.5));
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultSpans.factor, 0.5);
}

TEST_F(TraceSamplingParametersTest, AppendReflectsCurrentSamplerState) {
    ASSERT_OK(setDefaultFactor(0.75));

    BSONObjBuilder bob;
    param.append(nullptr, &bob, "openTelemetryTracingSampling"sv, /*tenantId=*/boost::none);

    EXPECT_DOUBLE_EQ(readDefaultFactor(bob.obj()), 0.75);
}

TEST_F(TraceSamplingParametersTest, AppendAfterMultipleSetsReflectsLastValue) {
    ASSERT_OK(setDefaultFactor(0.3));
    ASSERT_OK(setDefaultFactor(0.9));

    BSONObjBuilder bob;
    param.append(nullptr, &bob, "openTelemetryTracingSampling"sv, /*tenantId=*/boost::none);

    EXPECT_DOUBLE_EQ(readDefaultFactor(bob.obj()), 0.9);
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultSpans.factor, 0.9);
}

TEST_F(TraceSamplingParametersTest, NonObjectElementFails) {
    // Wrap in a parent object to obtain a BSONElement of non-object type.
    auto storage = BSON("v" << 42);
    EXPECT_THAT(param.set(storage.firstElement(), /*tenantId=*/boost::none), Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetFromStringWithValidJsonUpdatesGlobalSampler) {
    ASSERT_OK(param.setFromString(R"({"defaultSampling": {"samplingFactor": 0.5}})",
                                  /*tenantId=*/boost::none));
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultSpans.factor, 0.5);
}

TEST_F(TraceSamplingParametersTest, SetFromStringWithEmptyJsonUsesDefaults) {
    ASSERT_OK(param.setFromString("{}", /*tenantId=*/boost::none));
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultSpans.factor, 0.000045);
}

TEST_F(TraceSamplingParametersTest, SetWithOutOfRangeFactorFails) {
    EXPECT_THAT(setDefaultFactor(2.0), Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetFromStringWithOutOfRangeFactorFails) {
    EXPECT_THAT(param.setFromString(R"({"defaultSampling": {"samplingFactor": 2.0}})",
                                    /*tenantId=*/boost::none),
                Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetFromStringWithInvalidJsonFails) {
    EXPECT_THAT(param.setFromString("not valid json", /*tenantId=*/boost::none), Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetRateLimitUpdatesGlobalSampler) {
    ASSERT_OK(setRateLimit(2.0, 5));
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultSpans.rateLimits.refillRate, 2.0);
    EXPECT_EQ(TracingSampler::get().getConfig().defaultSpans.rateLimits.maxTokens, 5);
}

TEST_F(TraceSamplingParametersTest, AppendReflectsCurrentRateLimit) {
    ASSERT_OK(setRateLimit(3.0, 7));

    BSONObjBuilder bob;
    param.append(nullptr, &bob, "openTelemetryTracingSampling"sv, /*tenantId=*/boost::none);
    auto result = bob.obj();

    EXPECT_DOUBLE_EQ(readRefillRate(result), 3.0);
    EXPECT_EQ(readMaxTokens(result), 7);
}

TEST_F(TraceSamplingParametersTest, SetFromStringWithRateLimitUpdatesGlobalSampler) {
    ASSERT_OK(param.setFromString(
        R"({"defaultSampling": {"tokenBucketRateLimit": {"refillRate": 4.0, "maxTokens": 20}}})",
        /*tenantId=*/boost::none));
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultSpans.rateLimits.refillRate, 4.0);
    EXPECT_EQ(TracingSampler::get().getConfig().defaultSpans.rateLimits.maxTokens, 20);
}

TEST_F(TraceSamplingParametersTest, SetFromStringWithEmptyJsonUsesRateLimitDefaults) {
    ASSERT_OK(param.setFromString("{}", /*tenantId=*/boost::none));
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultSpans.rateLimits.refillRate, 1.0);
    EXPECT_EQ(TracingSampler::get().getConfig().defaultSpans.rateLimits.maxTokens, 10);
}

TEST_F(TraceSamplingParametersTest, SetUpdatesBothFactorAndRateLimit) {
    auto storage =
        BSON("v" << BSON("defaultSampling" << BSON(
                             "samplingFactor" << 0.5 << "tokenBucketRateLimit"
                                              << BSON("refillRate" << 2.0 << "maxTokens" << 5))));
    ASSERT_OK(param.set(storage.firstElement(), /*tenantId=*/boost::none));

    auto config = TracingSampler::get().getConfig();
    EXPECT_DOUBLE_EQ(config.defaultSpans.factor, 0.5);
    EXPECT_DOUBLE_EQ(config.defaultSpans.rateLimits.refillRate, 2.0);
    EXPECT_EQ(config.defaultSpans.rateLimits.maxTokens, 5);
}

TEST_F(TraceSamplingParametersTest, SetWithNegativeRefillRateFails) {
    EXPECT_THAT(setRateLimit(-1.0, 10), Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetWithNegativeMaxTokensFails) {
    EXPECT_THAT(setRateLimit(1.0, -1), Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetWithZeroRefillRateFails) {
    EXPECT_THAT(setRateLimit(0.0, 10), Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetWithZeroMaxTokensFails) {
    EXPECT_THAT(setRateLimit(1.0, 0), Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetWithSamplesArrayUpdatesperSpanOverrides) {
    auto storage =
        BSON("v" << BSON("defaultSampling"
                         << BSON("samplingFactor" << 0.5) << "samples"
                         << BSON_ARRAY(BSON("spanSelection" << BSON("name" << "test_only.span1")
                                                            << "samplingStrategy"
                                                            << BSON("samplingFactor" << 0.8)))));
    ASSERT_OK(param.set(storage.firstElement(), /*tenantId=*/boost::none));
    EXPECT_DOUBLE_EQ(
        TracingSampler::get().getConfig().perSpanOverrides.at("test_only.span1").factor, 0.8);
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultSpans.factor, 0.5);
}

TEST_F(TraceSamplingParametersTest, AppendRoundTripsSamples) {
    auto storage =
        BSON("v" << BSON("defaultSampling"
                         << BSON("samplingFactor" << 0.3) << "samples"
                         << BSON_ARRAY(BSON("spanSelection" << BSON("name" << "test_only.span1")
                                                            << "samplingStrategy"
                                                            << BSON("samplingFactor" << 0.9)))));
    ASSERT_OK(param.set(storage.firstElement(), /*tenantId=*/boost::none));

    BSONObjBuilder bob;
    param.append(nullptr, &bob, "openTelemetryTracingSampling"sv, /*tenantId=*/boost::none);
    auto result = bob.obj();

    EXPECT_DOUBLE_EQ(readDefaultFactor(result), 0.3);
    auto samples = result["openTelemetryTracingSampling"]["samples"].Array();
    ASSERT_EQ(samples.size(), 1u);
    EXPECT_EQ(samples[0]["spanSelection"]["name"].String(), "test_only.span1");
    EXPECT_DOUBLE_EQ(samples[0]["samplingStrategy"]["samplingFactor"].Double(), 0.9);
}

TEST_F(TraceSamplingParametersTest, SamplesOnlyConfigIsApplied) {
    // No defaultSampling field — only a samples array.
    auto storage = BSON(
        "v" << BSON("samples" << BSON_ARRAY(BSON(
                        "spanSelection" << BSON("name" << "test_only.span1") << "samplingStrategy"
                                        << BSON("samplingFactor" << 0.7)))));
    ASSERT_OK(param.set(storage.firstElement(), /*tenantId=*/boost::none));
    EXPECT_DOUBLE_EQ(
        TracingSampler::get().getConfig().perSpanOverrides.at("test_only.span1").factor, 0.7);
}

TEST_F(TraceSamplingParametersTest, SamplesDefaultToDefaultFactorWhenNoSamplingStrategyIsProvided) {
    auto storage = BSON(
        "v" << BSON("defaultSampling"
                    << BSON("samplingFactor" << 0.3) << "samples"
                    << BSON_ARRAY(BSON("spanSelection" << BSON("name" << "test_only.span1")))));
    ASSERT_OK(param.set(storage.firstElement(), /*tenantId=*/boost::none));
    EXPECT_DOUBLE_EQ(
        TracingSampler::get().getConfig().perSpanOverrides.at("test_only.span1").factor, 0.3);
}

TEST_F(TraceSamplingParametersTest, ErrorWhenSpanSelectionIsMissing) {
    auto storage =
        BSON("v" << BSON("samples"
                         << BSON_ARRAY(BSON("samplingStrategy" << BSON("samplingFactor" << 0.3)))));
    EXPECT_THAT(param.set(storage.firstElement(), /*tenantId=*/boost::none), Not(StatusIsOK()));
}

MATCHER_P(HasRefillRate, rate, "") {
    return arg.rateLimits.refillRate == rate;
}
MATCHER_P(HasMaxTokens, tokens, "") {
    return arg.rateLimits.maxTokens == tokens;
}

TEST_F(TraceSamplingParametersTest, SetWithSamplesArrayUpdatesPerSpanRateLimits) {
    auto storage =
        BSON("v" << BSON(
                 "defaultSampling"
                 << BSON("samplingFactor" << 0.5) << "samples"
                 << BSON_ARRAY(BSON("spanSelection"
                                    << BSON("name" << "test_only.span1") << "samplingStrategy"
                                    << BSON("samplingFactor"
                                            << 0.8 << "tokenBucketRateLimit"
                                            << BSON("refillRate" << 3.0 << "maxTokens" << 15))))));
    ASSERT_OK(param.set(storage.firstElement(), /*tenantId=*/boost::none));
    auto config = TracingSampler::get().getConfig();
    EXPECT_THAT(config.perSpanOverrides,
                ElementsAre(Pair("test_only.span1", AllOf(HasRefillRate(3.0), HasMaxTokens(15)))));
}

TEST_F(TraceSamplingParametersTest, DefaultsToDefaultRateLimitWhenNoSamplingStrategyIsProvided) {
    auto storage = BSON(
        "v" << BSON("defaultSampling"
                    << BSON("samplingFactor" << 0.3 << "tokenBucketRateLimit"
                                             << BSON("refillRate" << 5.0 << "maxTokens" << 20))
                    << "samples"
                    << BSON_ARRAY(BSON("spanSelection" << BSON("name" << "test_only.span1")))));
    ASSERT_OK(param.set(storage.firstElement(), /*tenantId=*/boost::none));
    auto config = TracingSampler::get().getConfig();
    EXPECT_THAT(config.perSpanOverrides,
                ElementsAre(Pair("test_only.span1", AllOf(HasRefillRate(5.0), HasMaxTokens(20)))));
}

TEST_F(TraceSamplingParametersTest, AppendRoundTripsPerSpanRateLimits) {
    auto storage = BSON(
        "v" << BSON("defaultSampling"
                    << BSON("samplingFactor" << 0.5) << "samples"
                    << BSON_ARRAY(
                           BSON("spanSelection"
                                << BSON("name" << "test_only.span1") << "samplingStrategy"
                                << BSON("samplingFactor"
                                        << 0.8 << "tokenBucketRateLimit"
                                        << BSON("refillRate" << 3.0 << "maxTokens" << 15)))
                           << BSON("spanSelection"
                                   << BSON("name" << "test_only.span2") << "samplingStrategy"
                                   << BSON("samplingFactor"
                                           << 0.6 << "tokenBucketRateLimit"
                                           << BSON("refillRate" << 5.0 << "maxTokens" << 25))))));
    ASSERT_OK(param.set(storage.firstElement(), /*tenantId=*/boost::none));

    BSONObjBuilder bob;
    param.append(nullptr, &bob, "openTelemetryTracingSampling"sv, /*tenantId=*/boost::none);
    auto result = bob.obj();

    auto samples = result["openTelemetryTracingSampling"]["samples"].Array();
    ASSERT_EQ(samples.size(), 2u);

    // Find each span by name since append order may vary.
    BSONElement span1, span2;
    for (const auto& s : samples) {
        auto name = s["spanSelection"]["name"].String();
        if (name == "test_only.span1")
            span1 = s;
        else if (name == "test_only.span2")
            span2 = s;
    }

    ASSERT_FALSE(span1.eoo());
    EXPECT_DOUBLE_EQ(span1["samplingStrategy"]["tokenBucketRateLimit"]["refillRate"].Double(), 3.0);
    EXPECT_EQ(span1["samplingStrategy"]["tokenBucketRateLimit"]["maxTokens"].Int(), 15);

    ASSERT_FALSE(span2.eoo());
    EXPECT_DOUBLE_EQ(span2["samplingStrategy"]["tokenBucketRateLimit"]["refillRate"].Double(), 5.0);
    EXPECT_EQ(span2["samplingStrategy"]["tokenBucketRateLimit"]["maxTokens"].Int(), 25);
}

class TraceExternalTracingParametersTest : public unittest::Test {
public:
    OpenTelemetryExternalTracingServerParameter param{"openTelemetryExternalTracing",
                                                      ServerParameterType::kRuntimeOnly};

    void setUp() override {
        TracingSampler::get().updateExternalConfig({kDefaultRefillRate, kDefaultMaxTokens});
    }

    Status setRateLimit(double refillRate, int maxTokens) {
        auto storage =
            BSON("v" << BSON("tokenBucketRateLimit"
                             << BSON("refillRate" << refillRate << "maxTokens" << maxTokens)));
        return param.set(storage.firstElement(), /*tenantId=*/boost::none);
    }

    RateLimitParams readExternalRateLimit(const BSONObj& result) {
        auto rl = result["openTelemetryExternalTracing"]["tokenBucketRateLimit"];
        return {rl["refillRate"].Double(), rl["maxTokens"].Int()};
    }
};

TEST_F(TraceExternalTracingParametersTest, SetUpdatesGlobalSampler) {
    ASSERT_OK(setRateLimit(2.0, 5));
    EXPECT_THAT(TracingSampler::get().getConfig().externalRateLimits, FieldsAre(2.0, 5));
}

TEST_F(TraceExternalTracingParametersTest, AppendReflectsCurrentState) {
    ASSERT_OK(setRateLimit(3.0, 7));

    BSONObjBuilder bob;
    param.append(nullptr, &bob, "openTelemetryExternalTracing"sv, /*tenantId=*/boost::none);

    EXPECT_THAT(readExternalRateLimit(bob.obj()), FieldsAre(3.0, 7));
}

TEST_F(TraceExternalTracingParametersTest, AppendAfterMultipleSetsReflectsLastValue) {
    ASSERT_OK(setRateLimit(2.0, 5));
    ASSERT_OK(setRateLimit(4.0, 15));

    BSONObjBuilder bob;
    param.append(nullptr, &bob, "openTelemetryExternalTracing"sv, /*tenantId=*/boost::none);

    EXPECT_THAT(readExternalRateLimit(bob.obj()), FieldsAre(4.0, 15));
}

TEST_F(TraceExternalTracingParametersTest, NonObjectElementFails) {
    auto storage = BSON("v" << 42);
    EXPECT_THAT(param.set(storage.firstElement(), /*tenantId=*/boost::none), Not(StatusIsOK()));
}

TEST_F(TraceExternalTracingParametersTest, SetFromStringWithValidJsonUpdatesGlobalSampler) {
    ASSERT_OK(
        param.setFromString(R"({"tokenBucketRateLimit": {"refillRate": 5.0, "maxTokens": 20}})",
                            /*tenantId=*/boost::none));
    EXPECT_THAT(TracingSampler::get().getConfig().externalRateLimits, FieldsAre(5.0, 20));
}

TEST_F(TraceExternalTracingParametersTest, SetFromStringWithEmptyJsonUsesDefaults) {
    ASSERT_OK(param.setFromString("{}", /*tenantId=*/boost::none));
    EXPECT_THAT(TracingSampler::get().getConfig().externalRateLimits,
                FieldsAre(kDefaultRefillRate, kDefaultMaxTokens));
}

TEST_F(TraceExternalTracingParametersTest, SetFromStringWithInvalidJsonFails) {
    EXPECT_THAT(param.setFromString("not valid json", /*tenantId=*/boost::none), Not(StatusIsOK()));
}

TEST_F(TraceExternalTracingParametersTest, SetWithNegativeRefillRateFails) {
    EXPECT_THAT(setRateLimit(-1.0, 10), Not(StatusIsOK()));
}

TEST_F(TraceExternalTracingParametersTest, SetWithZeroRefillRateFails) {
    EXPECT_THAT(setRateLimit(0.0, 10), Not(StatusIsOK()));
}

TEST_F(TraceExternalTracingParametersTest, SetWithNegativeMaxTokensFails) {
    EXPECT_THAT(setRateLimit(1.0, -1), Not(StatusIsOK()));
}

TEST_F(TraceExternalTracingParametersTest, SetWithZeroMaxTokensFails) {
    EXPECT_THAT(setRateLimit(1.0, 0), Not(StatusIsOK()));
}

}  // namespace
}  // namespace mongo::otel::traces
