/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/feature_flag.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_flag_test_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"
#include "mongo/util/version/releases.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace {

ServerParameter* getServerParameter(const std::string& name) {
    return ServerParameterSet::getNodeParameterSet()->get(name);
}

class FeatureFlagTest : public unittest::Test {

protected:
    void setUp() override;

protected:
    ServerParameter* _featureFlagBlender{nullptr};
    ServerParameter* _featureFlagSpoon{nullptr};
    ServerParameter* _featureFlagFork{nullptr};
};


void FeatureFlagTest::setUp() {
    // Set common flags which test the version string to true
    _featureFlagBlender = getServerParameter("featureFlagBlender");
    ASSERT_OK(_featureFlagBlender->setFromString("true", boost::none));

    _featureFlagSpoon = getServerParameter("featureFlagSpoon");
    ASSERT_OK(_featureFlagSpoon->setFromString("true", boost::none));

    _featureFlagFork = getServerParameter("featureFlagFork");

    ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCVUnsafe() == true);
    ASSERT(feature_flags::gFeatureFlagSpoon.isEnabledAndIgnoreFCVUnsafe() == true);

    Test::setUp();
}

boost::optional<multiversion::FeatureCompatibilityVersion> getFeatureFlagVersion(
    const FeatureFlag& flag) {
    BSONObjBuilder flagBuilder;
    flag.appendFlagValueAndMetadata(flagBuilder);
    BSONObj serializedFlag = flagBuilder.done();

    auto version = serializedFlag["version"];
    if (version.eoo()) {
        return {};
    }

    return multiversion::parseVersionForFeatureFlags(version.checkAndGetStringData());
}

// Sanity check feature flags
TEST(IDLFeatureFlag, Basic) {
    // false is set by "default" attribute in the IDL file.
    ASSERT(feature_flags::gFeatureFlagToaster.isEnabledAndIgnoreFCVUnsafe() == false);

    auto* featureFlagToaster = getServerParameter("featureFlagToaster");
    ASSERT_OK(featureFlagToaster->setFromString("true", boost::none));
    ASSERT(feature_flags::gFeatureFlagToaster.isEnabledAndIgnoreFCVUnsafe() == true);
    ASSERT_NOT_OK(featureFlagToaster->setFromString("alpha", boost::none));

    // (Generic FCV reference): feature flag test
    ASSERT(getFeatureFlagVersion(feature_flags::gFeatureFlagToaster) ==
           multiversion::GenericFCV::kLatest);
}

// Verify getVersion works correctly when enabled and not enabled
TEST_F(FeatureFlagTest, Version) {
    // (Generic FCV reference): feature flag test
    ASSERT(getFeatureFlagVersion(feature_flags::gFeatureFlagBlender) ==
           multiversion::GenericFCV::kLatest);

    // NOTE: if you are hitting this assertion, the version in feature_flag_test.idl may need to be
    // changed to match the current kLastLTS
    // (Generic FCV reference): feature flag test
    ASSERT(getFeatureFlagVersion(feature_flags::gFeatureFlagSpoon) ==
           multiversion::GenericFCV::kLastLTS);

    ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
    ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCVUnsafe() == false);
    ASSERT_NOT_OK(_featureFlagBlender->setFromString("alpha", boost::none));

    ASSERT(!getFeatureFlagVersion(feature_flags::gFeatureFlagBlender).has_value());
}

// Test feature flag server parameters are serialized correctly
TEST_F(FeatureFlagTest, ServerStatus) {
    {
        ASSERT_OK(_featureFlagBlender->setFromString("true", boost::none));
        ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCVUnsafe() == true);

        BSONObjBuilder builder;

        _featureFlagBlender->append(nullptr, &builder, "blender", boost::none);

        ASSERT_BSONOBJ_EQ(
            builder.obj(),
            // (Generic FCV reference): feature flag test.
            BSON("blender" << BSON("value"
                                   << true << "version"
                                   << multiversion::toString(multiversion::GenericFCV::kLatest)
                                   << "shouldBeFCVGated" << true << "currentlyEnabled" << true)));
    }

    {
        ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
        ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCVUnsafe() == false);

        BSONObjBuilder builder;

        _featureFlagBlender->append(nullptr, &builder, "blender", boost::none);

        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("blender" << BSON("value" << false << "shouldBeFCVGated" << true
                                                         << "currentlyEnabled" << false)));
    }

    {
        ASSERT_OK(_featureFlagFork->setFromString("true", boost::none));
        ASSERT(feature_flags::gFeatureFlagFork.isEnabled() == true);

        BSONObjBuilder builder;

        _featureFlagFork->append(nullptr, &builder, "fork", boost::none);

        // (Generic FCV reference): feature flag test
        ASSERT_BSONOBJ_EQ(
            builder.obj(),
            BSON("fork" << BSON("value" << true << "version"
                                        << multiversion::toString(multiversion::GenericFCV::kLatest)
                                        << "shouldBeFCVGated" << false << "currentlyEnabled"
                                        << true)));
    }

    {
        ASSERT_OK(_featureFlagFork->setFromString("false", boost::none));
        ASSERT(feature_flags::gFeatureFlagFork.isEnabled() == false);

        BSONObjBuilder builder;

        _featureFlagFork->append(nullptr, &builder, "fork", boost::none);

        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("fork" << BSON("value" << false << "shouldBeFCVGated" << false
                                                      << "currentlyEnabled" << false)));
    }
}

// (Generic FCV reference): feature flag test
static const ServerGlobalParams::FCVSnapshot kLastLTSFCVSnapshot(
    multiversion::GenericFCV::kLastLTS);
static const ServerGlobalParams::FCVSnapshot kLastContinuousFCVSnapshot(
    multiversion::GenericFCV::kLastContinuous);
static const ServerGlobalParams::FCVSnapshot kLatestFCVSnapshot(multiversion::GenericFCV::kLatest);
static const ServerGlobalParams::FCVSnapshot kUninitializedFCVSnapshot(
    multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior);

// (Generic FCV reference): feature flag test
static const ServerGlobalParams::FCVSnapshot kUpgradingFromLastLTSToLatestFCVSnapshot(
    multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);
static const ServerGlobalParams::FCVSnapshot kDowngradingFromLatestToLastLTSFCVSnapshot(
    multiversion::GenericFCV::kDowngradingFromLatestToLastLTS);
static const ServerGlobalParams::FCVSnapshot kUpgradingFromLastContinuousToLatestFCVSnapshot(
    multiversion::GenericFCV::kUpgradingFromLastContinuousToLatest);
static const ServerGlobalParams::FCVSnapshot kDowngradingFromLatestToLastContinuousFCVSnapshot(
    multiversion::GenericFCV::kDowngradingFromLatestToLastContinuous);
static const ServerGlobalParams::FCVSnapshot kUpgradingFromLastLTSToLastContinuousFCVSnapshot(
    multiversion::GenericFCV::kUpgradingFromLastLTSToLastContinuous);

// Test feature flags are enabled and not enabled based on fcv
TEST_F(FeatureFlagTest, IsEnabledTrue) {
    // Test FCV checks with enabled flag
    // Test newest version
    ASSERT_TRUE(
        feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kLatestFCVSnapshot));
    ASSERT_TRUE(feature_flags::gFeatureFlagSpoon.isEnabled(kNoVersionContext, kLatestFCVSnapshot));

    // Test oldest version
    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kLastLTSFCVSnapshot));
    ASSERT_TRUE(feature_flags::gFeatureFlagSpoon.isEnabled(kNoVersionContext, kLastLTSFCVSnapshot));
}

// Test feature flags are disabled regardless of fcv
TEST_F(FeatureFlagTest, IsEnabledFalse) {
    // Test FCV checks with disabled flag
    // Test newest version
    ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
    ASSERT_OK(_featureFlagSpoon->setFromString("false", boost::none));

    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kLatestFCVSnapshot));
    ASSERT_FALSE(feature_flags::gFeatureFlagSpoon.isEnabled(kNoVersionContext, kLatestFCVSnapshot));

    // Test oldest version
    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kLastLTSFCVSnapshot));
    ASSERT_FALSE(
        feature_flags::gFeatureFlagSpoon.isEnabled(kNoVersionContext, kLastLTSFCVSnapshot));
}

// Test isEnabled variants with a fallback for uninitialized FCV
TEST_F(FeatureFlagTest, IsEnabledUseLastLTSFCVWhenUninitialized) {
    ASSERT_FALSE(feature_flags::gFeatureFlagBlender.isEnabledUseLastLTSFCVWhenUninitialized(
        kNoVersionContext, kLastLTSFCVSnapshot));
    ASSERT_TRUE(feature_flags::gFeatureFlagBlender.isEnabledUseLastLTSFCVWhenUninitialized(
        kNoVersionContext, kLatestFCVSnapshot));

    ASSERT_FALSE(feature_flags::gFeatureFlagBlender.isEnabledUseLastLTSFCVWhenUninitialized(
        kNoVersionContext, kUninitializedFCVSnapshot));
}

TEST_F(FeatureFlagTest, IsEnabledUseLatestFCVWhenUninitialized) {
    ASSERT_FALSE(feature_flags::gFeatureFlagBlender.isEnabledUseLatestFCVWhenUninitialized(
        kNoVersionContext, kLastLTSFCVSnapshot));
    ASSERT_TRUE(feature_flags::gFeatureFlagBlender.isEnabledUseLatestFCVWhenUninitialized(
        kNoVersionContext, kLatestFCVSnapshot));

    ASSERT_TRUE(feature_flags::gFeatureFlagBlender.isEnabledUseLatestFCVWhenUninitialized(
        kNoVersionContext, kUninitializedFCVSnapshot));
}

// (Generic FCV reference): feature flag test
static const VersionContext kLastLTSVersionContext{multiversion::GenericFCV::kLastLTS};
static const VersionContext kLatestVersionContext{multiversion::GenericFCV::kLatest};

// Test operation FCV during downgrade (FCV is kLastLTS but operations can still run on kLatest)
TEST_F(FeatureFlagTest, OperationFCVDowngrading) {
    // Both feature flags are enabled, as the effective FCV is kLatest
    ASSERT_TRUE(
        feature_flags::gFeatureFlagBlender.isEnabled(kLatestVersionContext, kLastLTSFCVSnapshot));
    ASSERT_TRUE(
        feature_flags::gFeatureFlagSpoon.isEnabled(kLatestVersionContext, kLastLTSFCVSnapshot));
}

// Test operation FCV when fully upgraded (both FCV and all operations running on kLatest)
TEST_F(FeatureFlagTest, OperationFCVFullyUpgraded) {
    // Both feature flags are enabled, as the effective FCV is kLatest
    ASSERT_TRUE(
        feature_flags::gFeatureFlagBlender.isEnabled(kLatestVersionContext, kLatestFCVSnapshot));
    ASSERT_TRUE(
        feature_flags::gFeatureFlagSpoon.isEnabled(kLatestVersionContext, kLatestFCVSnapshot));
}

// Test operation FCV when fully downgraded (both FCV and all operations running on kLastLTS)
TEST_F(FeatureFlagTest, OperationFCVFullyDowngraded) {
    // Feature flags with version=kLatest are disabled, as the effective FCV is kLastLTS
    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(kLastLTSVersionContext, kLastLTSFCVSnapshot));
    ASSERT_TRUE(
        feature_flags::gFeatureFlagSpoon.isEnabled(kLastLTSVersionContext, kLastLTSFCVSnapshot));
}

// Test operation FCV during upgrade (FCV is kLatest but operations can still run on kLastLTS)
TEST_F(FeatureFlagTest, OperationFCVUpgrade) {
    // Feature flags with version=kLatest are disabled, as the effective FCV is kLastLTS
    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(kLastLTSVersionContext, kLatestFCVSnapshot));
    ASSERT_TRUE(
        feature_flags::gFeatureFlagSpoon.isEnabled(kLastLTSVersionContext, kLatestFCVSnapshot));
}

// Test that the RAIIServerParameterControllerForTest works correctly on a feature flag.
TEST_F(FeatureFlagTest, RAIIFeatureFlagController) {
    // Set false feature flag to true
    ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
    {
        RAIIServerParameterControllerForTest controller("featureFlagBlender", true);
        ASSERT_TRUE(
            feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kLatestFCVSnapshot));
    }
    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kLatestFCVSnapshot));

    // Set true feature flag to false
    ASSERT_OK(_featureFlagBlender->setFromString("true", boost::none));
    {
        RAIIServerParameterControllerForTest controller("featureFlagBlender", false);
        ASSERT_FALSE(
            feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kLatestFCVSnapshot));
    }
    ASSERT_TRUE(
        feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kLatestFCVSnapshot));
}

// Test feature flags that should not be FCV Gated
TEST_F(FeatureFlagTest, ShouldBeFCVGatedFalse) {
    // Test that feature flag that is enabled and not FCV gated will return true for isEnabled,
    // regardless of the FCV.
    // Test newest version
    // (Generic FCV reference): feature flag test
    ASSERT_OK(_featureFlagFork->setFromString("true", boost::none));

    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

    ASSERT_TRUE(feature_flags::gFeatureFlagFork.isEnabled());

    // Test oldest version
    // (Generic FCV reference): feature flag test
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastLTS);

    ASSERT_TRUE(feature_flags::gFeatureFlagFork.isEnabled());

    // Test uninitialized FCV
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior);

    ASSERT_TRUE(feature_flags::gFeatureFlagFork.isEnabled());
}

// Utility to fail a test if a checkable feature flag reference is unexpectedly checked as if it was
// wrapping an FCV-gated feature flag.
static constexpr auto fcvGatedFlagCheckMustNotBeCalled = [](FCVGatedFeatureFlag&) -> bool {
    MONGO_UNREACHABLE;
};

TEST_F(FeatureFlagTest, TestFCVGatedWithTransitionOnTransitionalFCV) {
    // (Generic FCV reference): feature flag test
    mongo::FCVGatedFeatureFlag featureFlagLatest{
        true /* enabled */,
        multiversion::toString(multiversion::GenericFCV::kLatest),
        true /* enableOnTransitionalFCV */};
    mongo::FCVGatedFeatureFlag featureFlagLastContinuous{
        true /* enabled */,
        multiversion::toString(multiversion::GenericFCV::kLastContinuous),
        true /* enableOnTransitionalFCV */};

    // Test newest version
    ASSERT_TRUE(featureFlagLatest.isEnabled(kNoVersionContext, kLatestFCVSnapshot));
    ASSERT_TRUE(featureFlagLastContinuous.isEnabled(kNoVersionContext, kLatestFCVSnapshot));

    // Test last continuous
    ASSERT_FALSE(featureFlagLatest.isEnabled(kNoVersionContext, kLastContinuousFCVSnapshot));
    ASSERT_TRUE(featureFlagLastContinuous.isEnabled(kNoVersionContext, kLastContinuousFCVSnapshot));

    // The behavior is not exactly the same if there are no lastContinuous versions, and everything
    // are compile-time constants, hence the `if constexpr` here. If kLastContinuous == kLastLTS,
    // then the tests below would fail. Those situations are impossible in production.
    // (Generic FCV reference): feature flag test
    if constexpr (multiversion::GenericFCV::kLastContinuous != multiversion::GenericFCV::kLastLTS) {
        // Test oldest version
        ASSERT_FALSE(featureFlagLatest.isEnabled(kNoVersionContext, kLastLTSFCVSnapshot));
        ASSERT_FALSE(featureFlagLastContinuous.isEnabled(kNoVersionContext, kLastLTSFCVSnapshot));

        // Test upgrading LastLTS -> LastContinuous
        ASSERT_FALSE(featureFlagLatest.isEnabled(kNoVersionContext,
                                                 kUpgradingFromLastLTSToLastContinuousFCVSnapshot));
        ASSERT_TRUE(featureFlagLastContinuous.isEnabled(
            kNoVersionContext, kUpgradingFromLastLTSToLastContinuousFCVSnapshot));
    } else {
        // Test oldest version
        ASSERT_FALSE(featureFlagLatest.isEnabled(kNoVersionContext, kLastLTSFCVSnapshot));
        ASSERT_TRUE(featureFlagLastContinuous.isEnabled(kNoVersionContext, kLastLTSFCVSnapshot));
    }

    // Test upgrading LastLTS -> Latest
    ASSERT_TRUE(
        featureFlagLatest.isEnabled(kNoVersionContext, kUpgradingFromLastLTSToLatestFCVSnapshot));
    ASSERT_TRUE(featureFlagLastContinuous.isEnabled(kNoVersionContext,
                                                    kUpgradingFromLastLTSToLatestFCVSnapshot));

    // Test upgrading LastContinuous -> Latest
    ASSERT_TRUE(featureFlagLatest.isEnabled(kNoVersionContext,
                                            kUpgradingFromLastContinuousToLatestFCVSnapshot));
    ASSERT_TRUE(featureFlagLastContinuous.isEnabled(
        kNoVersionContext, kUpgradingFromLastContinuousToLatestFCVSnapshot));

    // Test downgrading Latest -> LastContinuous
    ASSERT_TRUE(featureFlagLatest.isEnabled(kNoVersionContext,
                                            kDowngradingFromLatestToLastContinuousFCVSnapshot));
    ASSERT_TRUE(featureFlagLastContinuous.isEnabled(
        kNoVersionContext, kDowngradingFromLatestToLastContinuousFCVSnapshot));

    // Test downgrading Latest -> LastLTS
    ASSERT_TRUE(
        featureFlagLatest.isEnabled(kNoVersionContext, kDowngradingFromLatestToLastLTSFCVSnapshot));
    ASSERT_TRUE(featureFlagLastContinuous.isEnabled(kNoVersionContext,
                                                    kDowngradingFromLatestToLastLTSFCVSnapshot));
}

BSONObj readStatsFromFlag(const IncrementalRolloutFeatureFlag& flag) {
    BSONArrayBuilder flagStatsBuilder;
    flag.appendFlagStats(flagStatsBuilder);
    BSONArray statsArray(flagStatsBuilder.done());

    ASSERT_EQ(statsArray.nFields(), 1);
    return statsArray.firstElement().Obj().getOwned();
}

TEST(IDLFeatureFlag, IncrementalRolloutFeatureFlag) {
    // Because it is in the "in_development" state, "featureFlagInDevelopmentForTest" should be
    // disabled by default.
    ASSERT(!feature_flags::gFeatureFlagInDevelopmentForTest.checkEnabled());

    // Verify that enabling the flag succeeds.
    auto* featureFlagInDevelopmentForTest = getServerParameter("featureFlagInDevelopmentForTest");
    ASSERT_OK(featureFlagInDevelopmentForTest->setFromString("true", boost::none));
    ASSERT(feature_flags::gFeatureFlagInDevelopmentForTest.checkEnabled());

    // Enable the flag a second time.
    ASSERT_OK(featureFlagInDevelopmentForTest->setFromString("true", boost::none));
    ASSERT(feature_flags::gFeatureFlagInDevelopmentForTest.checkEnabled());

    // Check the flag's stats, which should account for all three calls to 'checkEnabled()' but only
    // one "toggle," because the second call to 'setFromString()' does not change the flag's value.
    auto firstFlagStats = readStatsFromFlag(feature_flags::gFeatureFlagInDevelopmentForTest);
    ASSERT_BSONOBJ_EQ_UNORDERED(firstFlagStats,
                                BSONObjBuilder{}
                                    .append("name", "featureFlagInDevelopmentForTest")
                                    .append("value", true)
                                    .append("falseChecks", 1)
                                    .append("trueChecks", 2)
                                    .append("numToggles", 1)
                                    .obj());

    // Check the flag's stats again, to ensure that we did not alter their state by observing them.
    // (I.e, 'appendFlagStats()' should not change the 'falseChecks' and 'trueChecks' values.)
    auto secondFlagStats = readStatsFromFlag(feature_flags::gFeatureFlagInDevelopmentForTest);
    ASSERT_BSONOBJ_EQ_UNORDERED(firstFlagStats, secondFlagStats);
}

TEST(IDLFeatureFlag, ReleasedIncrementalRolloutFeatureFlag) {
    // Because it is in the "released" state, "featureFlagReleasedForTest" should be enabled by
    // default.
    ASSERT(feature_flags::gFeatureFlagReleasedForTest.checkEnabled());

    // Verify that enabling the flag succeeds but has no effect.
    auto* featureFlagReleasedForTest = getServerParameter("featureFlagReleasedForTest");
    ASSERT_OK(featureFlagReleasedForTest->setFromString("true", boost::none));
    ASSERT(feature_flags::gFeatureFlagReleasedForTest.checkEnabled());

    // Verify that disabling the flag succeeds.
    ASSERT_OK(featureFlagReleasedForTest->setFromString("false", boost::none));
    ASSERT(!feature_flags::gFeatureFlagReleasedForTest.checkEnabled());

    // Check the flag's stats, which should account for all three calls to 'checkEnabled()' but only
    // one "toggle," because the first call to 'setFromString()' does not change the flag's value.
    auto firstFlagStats = readStatsFromFlag(feature_flags::gFeatureFlagReleasedForTest);
    ASSERT_BSONOBJ_EQ_UNORDERED(firstFlagStats,
                                BSONObjBuilder{}
                                    .append("name", "featureFlagReleasedForTest")
                                    .append("value", false)
                                    .append("falseChecks", 1)
                                    .append("trueChecks", 2)
                                    .append("numToggles", 1)
                                    .obj());

    // Check the flag's stats again, to ensure that we did not alter their state by observing them.
    // (I.e, 'appendFlagStats()' should not change the 'falseChecks' and 'trueChecks' values.)
    auto secondFlagStats = readStatsFromFlag(feature_flags::gFeatureFlagReleasedForTest);
    ASSERT_BSONOBJ_EQ_UNORDERED(firstFlagStats, secondFlagStats);
}

TEST(IDLFeatureFlag, IncrementalFeatureRolloutContext) {
    // Initialize flags.
    feature_flags::gFeatureFlagInDevelopmentForTest.setForServerParameter(false);
    feature_flags::gFeatureFlagReleasedForTest.setForServerParameter(true);

    // Query an IFR flag using an IFR context.
    IncrementalFeatureRolloutContext ifrContext;
    ASSERT(ifrContext.getSavedFlagValue(feature_flags::gFeatureFlagReleasedForTest));

    // Querying the flag via the same IFR context should produce the same result, even if the flag
    // changed its value.
    feature_flags::gFeatureFlagReleasedForTest.setForServerParameter(false);
    ASSERT(ifrContext.getSavedFlagValue(feature_flags::gFeatureFlagReleasedForTest));

    // Query a second flag in order to save its value to the context as well.
    ASSERT_FALSE(ifrContext.getSavedFlagValue(feature_flags::gFeatureFlagInDevelopmentForTest));

    // Write the IFR context as a BSON array and validate the result.
    BSONArrayBuilder savedFlagsBuilder;
    ifrContext.appendSavedFlagValues(savedFlagsBuilder);
    BSONArray savedFlagsArray(savedFlagsBuilder.done());

    StringMap<bool> observedValues;
    for (auto&& element : savedFlagsArray) {
        ASSERT(element.isABSONObj()) << savedFlagsArray;

        auto valueField = element["value"];
        ASSERT(valueField.isBoolean()) << savedFlagsArray;
        observedValues.insert({element["name"].str(), valueField.boolean()});
    }

    StringMap<bool> expectedValues = {{"featureFlagInDevelopmentForTest", false},
                                      {"featureFlagReleasedForTest", true}};
    ASSERT_EQ(observedValues, expectedValues) << savedFlagsArray;
}
}  // namespace
}  // namespace mongo
