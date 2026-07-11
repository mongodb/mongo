// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/feature_flag.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/feature_flag_test_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/ifr_sender_version.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <array>
#include <charconv>

#include <boost/optional.hpp>

namespace mongo {

namespace {

// Builds an IFRSenderVersion representing 'fcv' at (major, minor, 0, 0). Sufficient for the
// fromWire tests below, whose semantics only depend on the (major, minor) prefix.
std::unique_ptr<IFRSenderVersion> senderVersionFromFcv(
    multiversion::FeatureCompatibilityVersion fcv) {
    auto v = std::make_unique<IFRSenderVersion>();
    v->setMajor(multiversion::majorVersion(fcv));
    v->setMinor(multiversion::minorVersion(fcv));
    v->setPatch(0);
    v->setExtra(0);
    return v;
}

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

// As senderVersionFromFcv, but with explicit patch and extra components, to express a sender in
// the same release series as an FCV but at a specific patch / pre-release build.
std::unique_ptr<IFRSenderVersion> senderVersionFromFcvWith(
    multiversion::FeatureCompatibilityVersion fcv, int patch, int extra) {
    auto v = senderVersionFromFcv(fcv);
    v->setPatch(patch);
    v->setExtra(extra);
    return v;
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
                                   << "fcv_gated" << true << "currentlyEnabled" << true)));
    }

    {
        ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
        ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCVUnsafe() == false);

        BSONObjBuilder builder;

        _featureFlagBlender->append(nullptr, &builder, "blender", boost::none);

        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("blender" << BSON("value" << false << "fcv_gated" << true
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
                                        << "fcv_gated" << false << "currentlyEnabled" << true)));
    }

    {
        ASSERT_OK(_featureFlagFork->setFromString("false", boost::none));
        ASSERT(feature_flags::gFeatureFlagFork.isEnabled() == false);

        BSONObjBuilder builder;

        _featureFlagFork->append(nullptr, &builder, "fork", boost::none);

        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("fork" << BSON("value" << false << "fcv_gated" << false
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

static const ServerGlobalParams::FCVSnapshot kUnsetDefaultLastLTSBehavior(
    multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior);

DEATH_TEST(FeatureFlagDeathTest, IsEnabledUndefined, "tassert") {
    feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kUnsetDefaultLastLTSBehavior);
}

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

// Test that the unittest::ServerParameterGuard works correctly on a feature flag.
TEST_F(FeatureFlagTest, RAIIFeatureFlagController) {
    // Set false feature flag to true
    ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
    {
        unittest::ServerParameterGuard controller("featureFlagBlender", true);
        ASSERT_TRUE(
            feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kLatestFCVSnapshot));
    }
    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(kNoVersionContext, kLatestFCVSnapshot));

    // Set true feature flag to false
    ASSERT_OK(_featureFlagBlender->setFromString("true", boost::none));
    {
        unittest::ServerParameterGuard controller("featureFlagBlender", false);
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

TEST(IDLFeatureFlag, OperationFCVOnlyFCVGatedFeatureFlag) {
    // Only the VersionContext (Operation FCV) is required to check if the feature flag is enabled
    ASSERT_FALSE(feature_flags::gFeatureFlagOperationFCVOnly.isEnabled(kLastLTSVersionContext));
    ASSERT_TRUE(feature_flags::gFeatureFlagOperationFCVOnly.isEnabled(kLatestVersionContext));
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

TEST(IDLFeatureFlag, ReleaseIncrementalRolloutFeatureFlag) {
    // Because it is in the "release" state, "featureFlagReleaseForTest" should be enabled by
    // default.
    ASSERT(feature_flags::gFeatureFlagReleaseForTest.checkEnabled());

    // Verify that enabling the flag succeeds but has no effect.
    auto* featureFlagReleaseForTest = getServerParameter("featureFlagReleaseForTest");
    ASSERT_OK(featureFlagReleaseForTest->setFromString("true", boost::none));
    ASSERT(feature_flags::gFeatureFlagReleaseForTest.checkEnabled());

    // Verify that disabling the flag succeeds.
    ASSERT_OK(featureFlagReleaseForTest->setFromString("false", boost::none));
    ASSERT(!feature_flags::gFeatureFlagReleaseForTest.checkEnabled());

    // Check the flag's stats, which should account for all three calls to 'checkEnabled()' but only
    // one "toggle," because the first call to 'setFromString()' does not change the flag's value.
    auto firstFlagStats = readStatsFromFlag(feature_flags::gFeatureFlagReleaseForTest);
    ASSERT_BSONOBJ_EQ_UNORDERED(firstFlagStats,
                                BSONObjBuilder{}
                                    .append("name", "featureFlagReleaseForTest")
                                    .append("value", false)
                                    .append("falseChecks", 1)
                                    .append("trueChecks", 2)
                                    .append("numToggles", 1)
                                    .obj());

    // Check the flag's stats again, to ensure that we did not alter their state by observing them.
    // (I.e, 'appendFlagStats()' should not change the 'falseChecks' and 'trueChecks' values.)
    auto secondFlagStats = readStatsFromFlag(feature_flags::gFeatureFlagReleaseForTest);
    ASSERT_BSONOBJ_EQ_UNORDERED(firstFlagStats, secondFlagStats);
}

TEST(IDLFeatureFlag, IncrementalFeatureRolloutContext) {
    // Initialize flags.
    feature_flags::gFeatureFlagInDevelopmentForTest.setForServerParameter(false);
    feature_flags::gFeatureFlagReleaseForTest.setForServerParameter(true);

    // Query an IFR flag using an IFR context.
    IncrementalFeatureRolloutContext ifrContext;
    ASSERT(ifrContext.getSavedFlagValue(feature_flags::gFeatureFlagReleaseForTest));

    // Querying the flag via the same IFR context should produce the same result, even if the flag
    // changed its value.
    feature_flags::gFeatureFlagReleaseForTest.setForServerParameter(false);
    ASSERT(ifrContext.getSavedFlagValue(feature_flags::gFeatureFlagReleaseForTest));

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
                                      {"featureFlagReleaseForTest", true}};
    ASSERT_EQ(observedValues, expectedValues) << savedFlagsArray;
}

TEST(IDLFeatureFlag, IFRContextDisableFlagPreviouslyTrue) {
    auto& releaseFeatureFlag = feature_flags::gFeatureFlagReleaseForTest;
    releaseFeatureFlag.setForServerParameter(true);
    IncrementalFeatureRolloutContext ifrContext;
    ASSERT(ifrContext.getSavedFlagValue(releaseFeatureFlag));
    ifrContext.disableFlag(releaseFeatureFlag);
    ASSERT_FALSE(ifrContext.getSavedFlagValue(releaseFeatureFlag));
    ASSERT(releaseFeatureFlag.checkEnabled());
}

TEST(IDLFeatureFlag, IFRContextDisableFlagPreviouslyUnknown) {
    auto& developmentFeatureFlag = feature_flags::gFeatureFlagInDevelopmentForTest;
    developmentFeatureFlag.setForServerParameter(false);
    IncrementalFeatureRolloutContext ifrContext;
    ifrContext.disableFlag(developmentFeatureFlag);
    ASSERT_FALSE(ifrContext.getSavedFlagValue(developmentFeatureFlag));
}

TEST(IDLFeatureFlag, IFRContextDisableFlagPreviouslyFalse) {
    auto& developmentFeatureFlag = feature_flags::gFeatureFlagInDevelopmentForTest;
    developmentFeatureFlag.setForServerParameter(false);
    IncrementalFeatureRolloutContext ifrContext;
    developmentFeatureFlag.setForServerParameter(false);
    ASSERT_FALSE(ifrContext.getSavedFlagValue(developmentFeatureFlag));
    ifrContext.disableFlag(developmentFeatureFlag);
    ASSERT_FALSE(ifrContext.getSavedFlagValue(developmentFeatureFlag));
}

TEST(IDLFeatureFlag, ShouldSerializeOnOutgoingRequestsFalse) {
    ASSERT_FALSE(
        feature_flags::gFeatureFlagInDevelopmentForTest.shouldSerializeOnOutgoingRequests());
}

TEST(IDLFeatureFlag, ShouldSerializeOnOutgoingRequestsTrue) {
    ASSERT_TRUE(feature_flags::gFeatureFlagSerializeForTest.shouldSerializeOnOutgoingRequests());
}

// ---- fromWire tests ----

TEST(IDLFeatureFlag, FromWireContextIsMarkedFromWire) {
    auto wireCtx = IncrementalFeatureRolloutContext::fromWireForTest(std::span<const BSONObj>{});
    ASSERT_TRUE(wireCtx->isInstalledFromWire());

    IncrementalFeatureRolloutContext localCtx;
    ASSERT_FALSE(localCtx.isInstalledFromWire());
}

TEST(IDLFeatureFlag, FromWireEmptyPayload) {
    auto ctx = IncrementalFeatureRolloutContext::fromWireForTest(std::span<const BSONObj>{});
    ASSERT_TRUE(ctx->isInstalledFromWire());
}

TEST(IDLFeatureFlag, FromWireRecognizedFlagStoredAtWireValue) {
    feature_flags::gFeatureFlagSerializeForTest.setForServerParameter(false);

    std::vector<BSONObj> payload = {
        BSON("name" << "featureFlagSerializeForTest" << "value" << true)};
    auto ctx = IncrementalFeatureRolloutContext::fromWireForTest(payload);

    ASSERT_TRUE(ctx->isInstalledFromWire());
    // Wire value (true) overrides the local default (false set above).
    ASSERT_TRUE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagSerializeForTest));
}

// Death tests live in a separate suite because gtest forbids mixing TEST and
// TEST_F (which DEATH_TEST_REGEX expands to) in the same suite.
DEATH_TEST_REGEX(IDLFeatureFlagDeathTests,
                 FromWireUnknownFlagSameSenderVersionErrors,
                 "Tripwire assertion.*13002300") {
    std::vector<BSONObj> payload = {BSON("name" << "unknownIfrFlagForTest9" << "value" << true)};
    ASSERT_THROWS_CODE(
        IncrementalFeatureRolloutContext::fromWireForTest(payload), AssertionException, 13002300);
}

TEST(IDLFeatureFlag, FromWireMalformedNonStringName) {
    std::vector<BSONObj> payload = {BSON("name" << 42 << "value" << true)};
    ASSERT_THROWS_CODE(
        IncrementalFeatureRolloutContext::fromWireForTest(payload), AssertionException, 11565102);
}

TEST(IDLFeatureFlag, FromWireMalformedNonBoolValue) {
    std::vector<BSONObj> payload = {
        BSON("name" << "featureFlagSerializeForTest" << "value" << "yes")};
    ASSERT_THROWS_CODE(
        IncrementalFeatureRolloutContext::fromWireForTest(payload), AssertionException, 11565103);
}

TEST(IDLFeatureFlag, FromWireAbsentFlagOlderSenderDisabled) {
    feature_flags::gFeatureFlagSerializeForTest.setForServerParameter(true);

    // (Generic FCV reference): Used for testing — simulates an older sender that predates the
    // flag.
    auto senderVersion = senderVersionFromFcv(multiversion::GenericFCV::kLastLTS);
    auto ctx = IncrementalFeatureRolloutContext::fromWire(std::span<const BSONObj>{},
                                                          std::move(senderVersion));
    ASSERT_TRUE(ctx->isInstalledFromWire());
    ASSERT_FALSE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagSerializeForTest));
}

TEST(IDLFeatureFlag, FromWireOmittedOutgoingFlagDisabledWhenSenderPredatesIntro) {
    // Mixed payload: one flag is present on the wire and one active outgoing flag is omitted.
    // With a sender predating the omitted flag's introduction, the omitted flag must resolve to
    // false regardless of the local default, while the received flag retains its wire value and
    // the local checkEnabled() is unaffected.
    feature_flags::gFeatureFlagSerializeForTest.setForServerParameter(true);
    feature_flags::gFeatureFlagReleaseForTest.setForServerParameter(true);

    std::vector<BSONObj> payload = {
        BSON("name" << feature_flags::gFeatureFlagReleaseForTest.getName() << "value" << true)};
    // (Generic FCV reference): Used for testing — sender predates the kLatest-introduced flag.
    auto senderVersion = senderVersionFromFcv(multiversion::GenericFCV::kLastLTS);
    auto ctx = IncrementalFeatureRolloutContext::fromWire(payload, std::move(senderVersion));

    ASSERT_TRUE(ctx->isInstalledFromWire());
    ASSERT_TRUE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagReleaseForTest));
    ASSERT_FALSE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagSerializeForTest));
    ASSERT_TRUE(feature_flags::gFeatureFlagSerializeForTest.checkEnabled());
}

TEST(IDLFeatureFlag, FromWireAbsentFlagSameSenderUsesLocalDefault) {
    feature_flags::gFeatureFlagSerializeForTest.setForServerParameter(true);

    // (Generic FCV reference): Used for testing — sender is at the same FCV as the receiver.
    auto senderVersion = senderVersionFromFcv(multiversion::GenericFCV::kLatest);
    auto ctx = IncrementalFeatureRolloutContext::fromWire(std::span<const BSONObj>{},
                                                          std::move(senderVersion));
    ASSERT_TRUE(ctx->isInstalledFromWire());
    ASSERT_TRUE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagSerializeForTest));
}

TEST(IDLFeatureFlag, FromWireAbsentFlagPatchNewerSenderUsesLocalDefault) {
    // A sender in the same release series as the flag's introduction FCV but at a later patch
    // (e.g. 9.0.5 vs a 9.0-introduced flag) knows the flag. The absent flag must resolve to the
    // local default, not to false.
    feature_flags::gFeatureFlagSerializeForTest.setForServerParameter(true);

    auto senderVersion = std::make_unique<IFRSenderVersion>(makeLocalIFRSenderVersion());
    senderVersion->setPatch(senderVersion->getPatch() + 1);
    auto ctx = IncrementalFeatureRolloutContext::fromWire(std::span<const BSONObj>{},
                                                          std::move(senderVersion));
    ASSERT_TRUE(ctx->isInstalledFromWire());
    ASSERT_TRUE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagSerializeForTest));
}

TEST(IDLFeatureFlag, FromWireAbsentFlagPreReleaseSenderUsesLocalDefault) {
    // A release-candidate build of the introduction series (extra < 0, e.g. 9.0.0-rc2) is still a
    // 9.0 binary and knows the flag.
    feature_flags::gFeatureFlagSerializeForTest.setForServerParameter(true);

    // (Generic FCV reference): Used for testing — sender is a pre-release build of the same FCV
    // series.
    auto senderVersion = std::make_unique<IFRSenderVersion>(makeLocalIFRSenderVersion());
    senderVersion->setExtra(-23);
    auto ctx = IncrementalFeatureRolloutContext::fromWire(std::span<const BSONObj>{},
                                                          std::move(senderVersion));
    ASSERT_TRUE(ctx->isInstalledFromWire());
    ASSERT_TRUE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagSerializeForTest));
}

TEST(IDLFeatureFlag, FromWireAbsentFlagNoSenderVersionDisabled) {
    // A sender that predates the wire protocol supplies no version, so an absent flag it cannot
    // know about must resolve to false.
    feature_flags::gFeatureFlagSerializeForTest.setForServerParameter(true);

    auto ctx = IncrementalFeatureRolloutContext::fromWire(std::span<const BSONObj>{}, nullptr);
    ASSERT_TRUE(ctx->isInstalledFromWire());
    ASSERT_FALSE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagSerializeForTest));
}

// ---- getFlagsIntroducedSinceLastLTS / installForRequestWithoutIfrFlags tests ----

TEST(IDLFeatureFlag, GetFlagsIntroducedSinceLastLTSIncludesLatestSerializedFlag) {
    auto flags = IncrementalRolloutFeatureFlag::getFlagsIntroducedSinceLastLTS();
    ASSERT_TRUE(std::any_of(flags.begin(), flags.end(), [](auto* flag) {
        return flag->getName() == "featureFlagSerializeForTest";
    }));
    // Flags without a serialize_on_outgoing_requests version are never returned.
    ASSERT_FALSE(std::any_of(flags.begin(), flags.end(), [](auto* flag) {
        return flag->getName() == "featureFlagInDevelopmentForTest";
    }));
}

// ---- opCtx decoration tests ----

class IFRContextOpCtxTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = cc().makeOperationContext();
    }
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(IFRContextOpCtxTest, InstallForRequestWithoutIfrFlagsOnShardServerDisablesLatestFlags) {
    const auto origRole = serverGlobalParams.clusterRole;
    ScopeGuard restoreRole([origRole] { serverGlobalParams.clusterRole = origRole; });
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    // Local default is on — but a shard must not turn a release@latest feature on when no router
    // coordinated a value, so it is disabled.
    feature_flags::gFeatureFlagSerializeForTest.setForServerParameter(true);

    IncrementalFeatureRolloutContext::installForRequestWithoutIfrFlags(_opCtx.get());
    auto ctx = IncrementalFeatureRolloutContext::get(_opCtx.get());
    ASSERT_FALSE(ctx->isInstalledFromWire());
    ASSERT_FALSE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagSerializeForTest));
}

TEST_F(IFRContextOpCtxTest, InstallForRequestWithoutIfrFlagsOnShardServerDefersUntilConsulted) {
    const auto origRole = serverGlobalParams.clusterRole;
    ScopeGuard restoreRole([origRole] { serverGlobalParams.clusterRole = origRole; });
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    feature_flags::gFeatureFlagSerializeForTest.setForServerParameter(true);

    IncrementalFeatureRolloutContext::installForRequestWithoutIfrFlags(_opCtx.get());
    // Deferred: until something consults a flag, the context is unmaterialized and treated as
    // absent, so no ifrFlags would be forwarded downstream.
    ASSERT_FALSE(IncrementalFeatureRolloutContext::isInstalled(_opCtx.get()));
    ASSERT_FALSE(IncrementalFeatureRolloutContext::tryGet(_opCtx.get()));
    // Consulting via get() materializes the shard-default (release@latest flag forced off).
    auto ctx = IncrementalFeatureRolloutContext::get(_opCtx.get());
    ASSERT_TRUE(ctx);
    ASSERT_FALSE(ctx->isInstalledFromWire());
    ASSERT_FALSE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagSerializeForTest));
    ASSERT_TRUE(IncrementalFeatureRolloutContext::isInstalled(_opCtx.get()));
}

TEST_F(IFRContextOpCtxTest, InstallForRequestWithoutIfrFlagsOnReplicaSetUsesLocalDefaults) {
    const auto origRole = serverGlobalParams.clusterRole;
    ScopeGuard restoreRole([origRole] { serverGlobalParams.clusterRole = origRole; });
    // A standalone / plain replica set has no sibling nodes to diverge from.
    serverGlobalParams.clusterRole = ClusterRole::None;
    feature_flags::gFeatureFlagSerializeForTest.setForServerParameter(true);

    IncrementalFeatureRolloutContext::installForRequestWithoutIfrFlags(_opCtx.get());
    auto ctx = IncrementalFeatureRolloutContext::get(_opCtx.get());
    ASSERT_FALSE(ctx->isInstalledFromWire());
    // No saved value pinned — a checkEnabled() fallback returns the local default (true).
    ASSERT_TRUE(ctx->getSavedFlagValue(feature_flags::gFeatureFlagSerializeForTest));
}

TEST_F(IFRContextOpCtxTest, TryGetOnFreshOpCtxReturnsNull) {
    ASSERT_FALSE(IncrementalFeatureRolloutContext::tryGet(_opCtx.get()));
}

TEST_F(IFRContextOpCtxTest, GetOnFreshOpCtxLazilyConstructs) {
    auto ctx = IncrementalFeatureRolloutContext::get(_opCtx.get());
    ASSERT_TRUE(ctx);
    ASSERT_FALSE(ctx->isInstalledFromWire());
}

TEST_F(IFRContextOpCtxTest, GetOnFreshOpCtxMakesTryGetNonNull) {
    IncrementalFeatureRolloutContext::get(_opCtx.get());
    ASSERT_TRUE(IncrementalFeatureRolloutContext::tryGet(_opCtx.get()));
}

TEST_F(IFRContextOpCtxTest, SetAndGetRoundTrip) {
    auto wireCtx = IncrementalFeatureRolloutContext::fromWireForTest(std::span<const BSONObj>{});
    IncrementalFeatureRolloutContext::set(_opCtx.get(), wireCtx);

    auto retrieved = IncrementalFeatureRolloutContext::get(_opCtx.get());
    ASSERT_EQ(retrieved, wireCtx);
    ASSERT_TRUE(retrieved->isInstalledFromWire());
}

TEST_F(IFRContextOpCtxTest, TryGetAfterSetReturnsSameContext) {
    auto wireCtx = IncrementalFeatureRolloutContext::fromWireForTest(std::span<const BSONObj>{});
    IncrementalFeatureRolloutContext::set(_opCtx.get(), wireCtx);

    ASSERT_EQ(IncrementalFeatureRolloutContext::tryGet(_opCtx.get()), wireCtx);
}

}  // namespace
}  // namespace mongo
