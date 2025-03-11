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

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/feature_flag_test_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/version_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/version/releases.h"

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

// Sanity check feature flags
TEST(IDLFeatureFlag, Basic) {
    // false is set by "default" attribute in the IDL file.
    ASSERT(feature_flags::gFeatureFlagToaster.isEnabledAndIgnoreFCVUnsafe() == false);

    auto* featureFlagToaster = getServerParameter("featureFlagToaster");
    ASSERT_OK(featureFlagToaster->setFromString("true", boost::none));
    ASSERT(feature_flags::gFeatureFlagToaster.isEnabledAndIgnoreFCVUnsafe() == true);
    ASSERT_NOT_OK(featureFlagToaster->setFromString("alpha", boost::none));

    // (Generic FCV reference): feature flag test
    ASSERT(feature_flags::gFeatureFlagToaster.getVersion() == multiversion::GenericFCV::kLatest);
}

// Verify getVersion works correctly when enabled and not enabled
TEST_F(FeatureFlagTest, Version) {
    // (Generic FCV reference): feature flag test
    ASSERT(feature_flags::gFeatureFlagBlender.getVersion() == multiversion::GenericFCV::kLatest);

    // NOTE: if you are hitting this assertion, the version in feature_flag_test.idl may need to be
    // changed to match the current kLastLTS
    // (Generic FCV reference): feature flag test
    ASSERT(feature_flags::gFeatureFlagSpoon.getVersion() == multiversion::GenericFCV::kLastLTS);

    ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
    ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCVUnsafe() == false);
    ASSERT_NOT_OK(_featureFlagBlender->setFromString("alpha", boost::none));

    ASSERT_THROWS(feature_flags::gFeatureFlagBlender.getVersion(), AssertionException);
}

// Test feature flag server parameters are serialized correctly
TEST_F(FeatureFlagTest, ServerStatus) {
    {
        ASSERT_OK(_featureFlagBlender->setFromString("true", boost::none));
        ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCVUnsafe() == true);

        BSONObjBuilder builder;

        _featureFlagBlender->append(nullptr, &builder, "blender", boost::none);

        ASSERT_BSONOBJ_EQ(builder.obj(),
                          // (Generic FCV reference): feature flag test.
                          BSON("blender" << BSON("value" << true << "version"
                                                         << multiversion::toString(
                                                                multiversion::GenericFCV::kLatest)
                                                         << "shouldBeFCVGated" << true)));
    }

    {
        ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
        ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCVUnsafe() == false);

        BSONObjBuilder builder;

        _featureFlagBlender->append(nullptr, &builder, "blender", boost::none);

        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("blender" << BSON("value" << false << "shouldBeFCVGated" << true)));
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
                                        << "shouldBeFCVGated" << false)));
    }

    {
        ASSERT_OK(_featureFlagFork->setFromString("false", boost::none));
        ASSERT(feature_flags::gFeatureFlagFork.isEnabled() == false);

        BSONObjBuilder builder;

        _featureFlagFork->append(nullptr, &builder, "fork", boost::none);

        ASSERT_BSONOBJ_EQ(builder.obj(),
                          BSON("fork" << BSON("value" << false << "shouldBeFCVGated" << false)));
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

// Test none and default-constructed values of a checkable feature flag reference.
// As they mean that a feature is not conditional on a feature flag, they always return true.
TEST_F(FeatureFlagTest, NoneAsCheckableFeatureFlagRef) {
    ASSERT_TRUE(CheckableFeatureFlagRef().isEnabled(fcvGatedFlagCheckMustNotBeCalled));
    ASSERT_TRUE(kDoesNotRequireFeatureFlag.isEnabled(fcvGatedFlagCheckMustNotBeCalled));
}

// Tests wrapping a binary-compatible feature flag inside a checkable feature flag reference.
// It should return whether the feature flag is enabled without invoking the callback.
TEST_F(FeatureFlagTest, BinaryCompatibleAsCheckableFeatureFlagRef) {
    CheckableFeatureFlagRef checkableFeatureFlagRefFork(feature_flags::gFeatureFlagFork);
    ASSERT_OK(_featureFlagFork->setFromString("false", boost::none));
    ASSERT_FALSE(checkableFeatureFlagRefFork.isEnabled(fcvGatedFlagCheckMustNotBeCalled));
    ASSERT_OK(_featureFlagFork->setFromString("true", boost::none));
    ASSERT_TRUE(checkableFeatureFlagRefFork.isEnabled(fcvGatedFlagCheckMustNotBeCalled));
}

// Tests wrapping an FCV-gated feature flag inside a checkable feature flag reference.
// It should invoke the callback to check whether the feature flag is enabled and forward its value.
TEST_F(FeatureFlagTest, FCVGatedAsCheckableFeatureFlagRef) {
    CheckableFeatureFlagRef checkableFeatureFlagRefBlender(feature_flags::gFeatureFlagBlender);
    checkableFeatureFlagRefBlender.isEnabled([](auto& fcvGatedFlag) -> bool {
        ASSERT_EQUALS(&fcvGatedFlag, &feature_flags::gFeatureFlagBlender);
        return true;
    });
    ASSERT_FALSE(checkableFeatureFlagRefBlender.isEnabled([](auto&) { return false; }));
    ASSERT_TRUE(checkableFeatureFlagRefBlender.isEnabled([](auto&) { return true; }));
}

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


}  // namespace
}  // namespace mongo
