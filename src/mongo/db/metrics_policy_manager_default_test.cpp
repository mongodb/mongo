// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/metrics_policy_manager_default.h"

#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class MetricsPolicyManagerDefaultTest : public unittest::Test {};

TEST_F(MetricsPolicyManagerDefaultTest, IsAutoRegisteredOnServiceContextCreation) {
    // The default metrics policy manager is auto-registered via ConstructorActionRegisterer,
    // so it should be available on any ServiceContext.
    auto svcCtx = ServiceContext::make();
    auto& manager = MetricsPolicyManager::get(svcCtx.get());
    ASSERT_FALSE(manager.requiresServerStatusFiltering(/*opCtx=*/nullptr));
}

//
// Tests for serverStatus.
//

TEST_F(MetricsPolicyManagerDefaultTest, DoesNotRequireServerStatusFiltering) {
    auto manager = std::make_unique<MetricsPolicyManagerDefault>();
    ASSERT_FALSE(manager->requiresServerStatusFiltering(/*opCtx=*/nullptr));
}

TEST_F(MetricsPolicyManagerDefaultTest, GetServerStatusAllowlistPathsThrowsIllegalOperation) {
    auto manager = std::make_unique<MetricsPolicyManagerDefault>();
    ASSERT_THROWS_CODE(
        manager->getServerStatusAllowlistPaths(), DBException, ErrorCodes::IllegalOperation);
}

TEST_F(MetricsPolicyManagerDefaultTest, GetServerStatusAllowlistMatcherThrowsIllegalOperation) {
    auto manager = std::make_unique<MetricsPolicyManagerDefault>();
    ASSERT_THROWS_CODE(
        manager->getServerStatusAllowlistMatcher(), DBException, ErrorCodes::IllegalOperation);
}

//
// Tests for replSetGetStatus.
//

TEST_F(MetricsPolicyManagerDefaultTest, DoesNotRequireReplSetGetStatusFiltering) {
    auto manager = std::make_unique<MetricsPolicyManagerDefault>();
    ASSERT_FALSE(manager->requiresReplSetGetStatusFiltering(/*opCtx=*/nullptr));
}

TEST_F(MetricsPolicyManagerDefaultTest, GetReplSetGetStatusAllowlistPathsThrowsIllegalOperation) {
    auto manager = std::make_unique<MetricsPolicyManagerDefault>();
    ASSERT_THROWS_CODE(
        manager->getReplSetGetStatusAllowlistPaths(), DBException, ErrorCodes::IllegalOperation);
}

TEST_F(MetricsPolicyManagerDefaultTest, GetReplSetGetStatusAllowlistMatcherThrowsIllegalOperation) {
    auto manager = std::make_unique<MetricsPolicyManagerDefault>();
    ASSERT_THROWS_CODE(
        manager->getReplSetGetStatusAllowlistMatcher(), DBException, ErrorCodes::IllegalOperation);
}

}  // namespace
}  // namespace mongo
