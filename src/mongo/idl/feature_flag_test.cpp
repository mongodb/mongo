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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/basic.h"

#include "mongo/idl/feature_flag_test_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

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
};


void FeatureFlagTest::setUp() {
    // Set common flags which test the version string to true
    _featureFlagBlender = getServerParameter("featureFlagBlender");
    ASSERT_OK(_featureFlagBlender->setFromString("true", boost::none));

    _featureFlagSpoon = getServerParameter("featureFlagSpoon");
    ASSERT_OK(_featureFlagSpoon->setFromString("true", boost::none));

    ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCV() == true);
    ASSERT(feature_flags::gFeatureFlagSpoon.isEnabledAndIgnoreFCV() == true);

    Test::setUp();
}

// Sanity check feature flags
TEST(IDLFeatureFlag, Basic) {
    // false is set by "default" attribute in the IDL file.
    ASSERT(feature_flags::gFeatureFlagToaster.isEnabledAndIgnoreFCV() == false);

    auto* featureFlagToaster = getServerParameter("featureFlagToaster");
    ASSERT_OK(featureFlagToaster->setFromString("true", boost::none));
    ASSERT(feature_flags::gFeatureFlagToaster.isEnabledAndIgnoreFCV() == true);
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
    ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCV() == false);
    ASSERT_NOT_OK(_featureFlagBlender->setFromString("alpha", boost::none));

    ASSERT_THROWS(feature_flags::gFeatureFlagBlender.getVersion(), AssertionException);
}

// Test feature flag server parameters are serialized correctly
TEST_F(FeatureFlagTest, ServerStatus) {
    {
        ASSERT_OK(_featureFlagBlender->setFromString("true", boost::none));
        ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCV() == true);

        BSONObjBuilder builder;

        _featureFlagBlender->append(nullptr, &builder, "blender", boost::none);

        ASSERT_BSONOBJ_EQ(
            builder.obj(),
            // (Generic FCV reference): feature flag test.
            BSON("blender" << BSON("value"
                                   << true << "version"
                                   << multiversion::toString(multiversion::GenericFCV::kLatest))));
    }

    {
        ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
        ASSERT(feature_flags::gFeatureFlagBlender.isEnabledAndIgnoreFCV() == false);

        BSONObjBuilder builder;

        _featureFlagBlender->append(nullptr, &builder, "blender", boost::none);

        ASSERT_BSONOBJ_EQ(builder.obj(), BSON("blender" << BSON("value" << false)));
    }
}

// Test feature flags are enabled and not enabled based on fcv
TEST_F(FeatureFlagTest, IsEnabledTrue) {
    // Test FCV checks with enabled flag
    // Test newest version
    // (Generic FCV reference): feature flag test
    serverGlobalParams.mutableFeatureCompatibility.setVersion(multiversion::GenericFCV::kLatest);

    ASSERT_TRUE(
        feature_flags::gFeatureFlagBlender.isEnabled(serverGlobalParams.featureCompatibility));
    ASSERT_TRUE(
        feature_flags::gFeatureFlagSpoon.isEnabled(serverGlobalParams.featureCompatibility));

    // Test oldest version
    // (Generic FCV reference): feature flag test
    serverGlobalParams.mutableFeatureCompatibility.setVersion(multiversion::GenericFCV::kLastLTS);

    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(serverGlobalParams.featureCompatibility));
    ASSERT_TRUE(
        feature_flags::gFeatureFlagSpoon.isEnabled(serverGlobalParams.featureCompatibility));
}

// Test feature flags are disabled regardless of fcv
TEST_F(FeatureFlagTest, IsEnabledFalse) {
    // Test FCV checks with disabled flag
    // Test newest version
    ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
    ASSERT_OK(_featureFlagSpoon->setFromString("false", boost::none));

    // (Generic FCV reference): feature flag test
    serverGlobalParams.mutableFeatureCompatibility.setVersion(multiversion::GenericFCV::kLatest);

    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(serverGlobalParams.featureCompatibility));
    ASSERT_FALSE(
        feature_flags::gFeatureFlagSpoon.isEnabled(serverGlobalParams.featureCompatibility));

    // Test oldest version
    // (Generic FCV reference): feature flag test
    serverGlobalParams.mutableFeatureCompatibility.setVersion(multiversion::GenericFCV::kLastLTS);

    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(serverGlobalParams.featureCompatibility));
    ASSERT_FALSE(
        feature_flags::gFeatureFlagSpoon.isEnabled(serverGlobalParams.featureCompatibility));
}

// Test that the RAIIServerParameterControllerForTest works correctly on a feature flag.
TEST_F(FeatureFlagTest, RAIIFeatureFlagController) {
    // Set false feature flag to true
    ASSERT_OK(_featureFlagBlender->setFromString("false", boost::none));
    {
        RAIIServerParameterControllerForTest controller("featureFlagBlender", true);
        ASSERT_TRUE(
            feature_flags::gFeatureFlagBlender.isEnabled(serverGlobalParams.featureCompatibility));
    }
    ASSERT_FALSE(
        feature_flags::gFeatureFlagBlender.isEnabled(serverGlobalParams.featureCompatibility));

    // Set true feature flag to false
    ASSERT_OK(_featureFlagBlender->setFromString("true", boost::none));
    {
        RAIIServerParameterControllerForTest controller("featureFlagBlender", false);
        ASSERT_FALSE(
            feature_flags::gFeatureFlagBlender.isEnabled(serverGlobalParams.featureCompatibility));
    }
    ASSERT_TRUE(
        feature_flags::gFeatureFlagBlender.isEnabled(serverGlobalParams.featureCompatibility));
}


}  // namespace
}  // namespace mongo
