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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/otel/traces/trace_sampling_parameters_gen.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional.hpp>
#include <gmock/gmock.h>

namespace mongo::otel::traces {
namespace {

using mongo::unittest::match::StatusIsOK;
using ::testing::Not;

class TraceSamplingParametersTest : public unittest::Test {
public:
    OpenTelemetryTracingSamplingServerParameter param{"openTelemetryTracingSampling",
                                                      ServerParameterType::kRuntimeOnly};

    void setUp() override {
        TracingSampler::get().updateConfig(SamplingConfig{});
    }

    Status setFactor(double factor) {
        // Wrap in a parent object to obtain a BSONElement of object type.
        auto storage = BSON("v" << BSON("defaultSampling" << BSON("samplingFactor" << factor)));
        return param.set(storage.firstElement(), /*tenantId=*/boost::none);
    }

    double readFactor(const BSONObj& result) {
        return result["openTelemetryTracingSampling"]["defaultSampling"]["samplingFactor"].Double();
    }
};

TEST_F(TraceSamplingParametersTest, SetUpdatesGlobalSampler) {
    ASSERT_OK(setFactor(0.5));
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultFactor, 0.5);
}

TEST_F(TraceSamplingParametersTest, AppendReflectsCurrentSamplerState) {
    ASSERT_OK(setFactor(0.75));

    BSONObjBuilder bob;
    param.append(nullptr, &bob, "openTelemetryTracingSampling"_sd, /*tenantId=*/boost::none);

    EXPECT_DOUBLE_EQ(readFactor(bob.obj()), 0.75);
}

TEST_F(TraceSamplingParametersTest, AppendAfterMultipleSetsReflectsLastValue) {
    ASSERT_OK(setFactor(0.3));
    ASSERT_OK(setFactor(0.9));

    BSONObjBuilder bob;
    param.append(nullptr, &bob, "openTelemetryTracingSampling"_sd, /*tenantId=*/boost::none);

    EXPECT_DOUBLE_EQ(readFactor(bob.obj()), 0.9);
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultFactor, 0.9);
}

TEST_F(TraceSamplingParametersTest, NonObjectElementFails) {
    // Wrap in a parent object to obtain a BSONElement of non-object type.
    auto storage = BSON("v" << 42);
    EXPECT_THAT(param.set(storage.firstElement(), /*tenantId=*/boost::none), Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetFromStringWithValidJsonUpdatesGlobalSampler) {
    ASSERT_OK(param.setFromString(R"({"defaultSampling": {"samplingFactor": 0.5}})",
                                  /*tenantId=*/boost::none));
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultFactor, 0.5);
}

TEST_F(TraceSamplingParametersTest, SetFromStringWithEmptyJsonUsesDefaults) {
    ASSERT_OK(param.setFromString("{}", /*tenantId=*/boost::none));
    EXPECT_DOUBLE_EQ(TracingSampler::get().getConfig().defaultFactor, 0.000045);
}

TEST_F(TraceSamplingParametersTest, SetWithOutOfRangeFactorFails) {
    EXPECT_THAT(setFactor(2.0), Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetFromStringWithOutOfRangeFactorFails) {
    EXPECT_THAT(param.setFromString(R"({"defaultSampling": {"samplingFactor": 2.0}})",
                                    /*tenantId=*/boost::none),
                Not(StatusIsOK()));
}

TEST_F(TraceSamplingParametersTest, SetFromStringWithInvalidJsonFails) {
    EXPECT_THAT(param.setFromString("not valid json", /*tenantId=*/boost::none), Not(StatusIsOK()));
}

}  // namespace
}  // namespace mongo::otel::traces
