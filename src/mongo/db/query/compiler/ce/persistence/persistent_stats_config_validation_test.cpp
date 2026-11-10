// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/persistence/persistent_stats_config_validation.h"

#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/ce/persistence/persistent_stats_config_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>

#include <boost/optional.hpp>

namespace mongo {
namespace {

PersistentStatsConfig makeConfig(boost::optional<std::int32_t> sampleSize,
                                 boost::optional<std::int32_t> joinSampleSize) {
    PersistentStatsSamplesConfig samples;
    samples.setSampleSize(sampleSize);
    samples.setJoinSampleSize(joinSampleSize);

    PersistentStatsConfig config;
    config.setSamples(samples);
    return config;
}

PersistentStatsConfigUpdate parsePersistentStatsConfigUpdate(const BSONObj& update) {
    return PersistentStatsConfigUpdate::parse(update, IDLParserContext("persistentStatsConfig"));
}

template <bool kFeatureFlagEnabled>
class PersistentStatsConfigValidationTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _featureFlag.emplace("featureFlagPersistentStats", kFeatureFlagEnabled);
        _opCtx = makeOperationContext();
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

private:
    boost::optional<unittest::ServerParameterGuard> _featureFlag;
    ServiceContext::UniqueOperationContext _opCtx;
};

using FeatureFlagDisabledTest = PersistentStatsConfigValidationTest<false>;
using FeatureFlagEnabledTest = PersistentStatsConfigValidationTest<true>;

TEST_F(FeatureFlagDisabledTest, RejectsWhenFeatureFlagIsDisabled) {
    ASSERT_EQ(validatePersistentStatsConfigIsApplicable(opCtx(), false).code(),
              ErrorCodes::InvalidOptions);
}

TEST_F(FeatureFlagEnabledTest, AcceptsACollection) {
    ASSERT_OK(validatePersistentStatsConfigIsApplicable(opCtx(), false));
}

TEST_F(FeatureFlagEnabledTest, RejectsViews) {
    ASSERT_EQ(validatePersistentStatsConfigIsApplicable(opCtx(), true).code(),
              ErrorCodes::InvalidOptions);
}

TEST(PersistentStatsConfigValidation, AcceptsAnySampleSize) {
    ASSERT_OK(validatePersistentStatsConfig(makeConfig(1000, boost::none)));
    ASSERT_OK(validatePersistentStatsConfig(makeConfig(boost::none, 2000)));
}

TEST(PersistentStatsConfigValidation, RejectsAConfigWithoutSampleSizes) {
    ASSERT_EQ(validatePersistentStatsConfig(PersistentStatsConfig{}).code(),
              ErrorCodes::InvalidOptions);
    ASSERT_EQ(validatePersistentStatsConfig(makeConfig(boost::none, boost::none)).code(),
              ErrorCodes::InvalidOptions);
}

TEST(PersistentStatsConfigUpdateValidation, RejectsAnUpdateWithoutSampleSizes) {
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              validatePersistentStatsConfigUpdate(parsePersistentStatsConfigUpdate(fromjson("{}")))
                  .code());
    ASSERT_EQ(ErrorCodes::InvalidOptions,
              validatePersistentStatsConfigUpdate(
                  parsePersistentStatsConfigUpdate(fromjson("{samples: {}}")))
                  .code());
    ASSERT_OK(validatePersistentStatsConfigUpdate(
        parsePersistentStatsConfigUpdate(fromjson("{samples: {sampleSize: null}}"))));
}

}  // namespace
}  // namespace mongo
