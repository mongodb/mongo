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

namespace mongo::otel::traces {
namespace {

TEST(SamplerTest, UnregisteredSpanIsNeverSampled) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultFactor = 1.0});
    EXPECT_FALSE(sampler.shouldSample("unknown.span", 0.0));
}

TEST(SamplerTest, SeedBelowFactorSamples) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultFactor = 0.5});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
}

TEST(SamplerTest, SeedEqualToFactorDrops) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultFactor = 0.5});
    sampler.sampleByDefault(span_names::kTest1);
    // sampleValue < factor, so equality is not sampled.
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.5));
}

TEST(SamplerTest, SeedAboveFactorDrops) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultFactor = 0.5});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
}

TEST(SamplerTest, UpdateConfigChangesDecisionsImmediately) {
    TracingSamplerImpl sampler;
    sampler.sampleByDefault(span_names::kTest1);

    sampler.updateConfig(SamplingConfig{.defaultFactor = 0.5});
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));

    sampler.updateConfig(SamplingConfig{.defaultFactor = 0.1});
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
}

TEST(SamplerTest, RegisterSpanMakesItSampleable) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultFactor = 1.0});

    EXPECT_FALSE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));

    sampler.sampleByDefault(span_names::kTest2);

    EXPECT_TRUE(sampler.shouldSample(span_names::kTest2.getName(), 0.0));
}

TEST(SamplerTest, RegisteredSpanUsesCurrentDefaultFactor) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultFactor = 0.5});
    sampler.sampleByDefault(span_names::kTest1);

    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
}

TEST(SamplerTest, ZeroFactorDropsAllRegisteredSpans) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultFactor = 0.0});
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.0));
}

TEST(SamplerTest, OneFactorSamplesAllRegisteredSpans) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultFactor = 1.0});
    sampler.sampleByDefault(span_names::kTest1);
    // Any value in [0, 1) is strictly less than 1.0.
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.999));
}

TEST(SamplerTest, DuplicateRegistrationHasNoEffect) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultFactor = 0.5});
    sampler.sampleByDefault(span_names::kTest1);
    sampler.sampleByDefault(span_names::kTest1);
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
}

TEST(SamplerTest, MultipleSpansAreIndependentlySampled) {
    TracingSamplerImpl sampler;
    sampler.updateConfig(SamplingConfig{.defaultFactor = 0.5});
    sampler.sampleByDefault(span_names::kTest1);
    sampler.sampleByDefault(span_names::kTest2);

    EXPECT_TRUE(sampler.shouldSample(span_names::kTest1.getName(), 0.3));
    EXPECT_TRUE(sampler.shouldSample(span_names::kTest2.getName(), 0.3));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest1.getName(), 0.7));
    EXPECT_FALSE(sampler.shouldSample(span_names::kTest2.getName(), 0.7));
}

}  // namespace
}  // namespace mongo::otel::traces
