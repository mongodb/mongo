// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(ShouldSetIsTimeseriesFieldTest, FCVInitializedFeatureFlagOff) {
    unittest::ServerParameterGuard featureFlag("featureFlagMarkTimeseriesEventsInOplog", false);
    EXPECT_FALSE(shouldSetIsTimeseriesField(VersionContext{}));
}

TEST(ShouldSetIsTimeseriesFieldTest, FCVInitializedFeatureFlagOn) {
    unittest::ServerParameterGuard featureFlag("featureFlagMarkTimeseriesEventsInOplog", true);
    EXPECT_TRUE(shouldSetIsTimeseriesField(VersionContext{}));
}

}  // namespace
}  // namespace mongo

