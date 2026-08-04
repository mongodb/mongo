// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/metrics_policy_manager_default.h"

#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo {
namespace {

class MetricsPolicyManagerDefaultRegistrationTest : public unittest::Test {};

TEST_F(MetricsPolicyManagerDefaultRegistrationTest, IsAutoRegisteredOnServiceContextCreation) {
    // The default metrics policy manager is auto-registered via ConstructorActionRegisterer,
    // so it should be available on any ServiceContext.
    auto svcCtx = ServiceContext::make();
    auto& manager = MetricsPolicyManager::get(svcCtx.get());
    ASSERT_FALSE(manager.requiresFiltering(
        MetricsCategoryEnum::kServerStatus, /*opCtx=*/nullptr, /*forceFiltered=*/false));
}

class MetricsPolicyManagerDefaultFilteringTest
    : public unittest::Test,
      public testing::WithParamInterface<MetricsCategoryEnum> {};

TEST_P(MetricsPolicyManagerDefaultFilteringTest, DoesNotRequireFiltering) {
    auto manager = std::make_unique<MetricsPolicyManagerDefault>();
    ASSERT_FALSE(
        manager->requiresFiltering(GetParam(), /*opCtx=*/nullptr, /*forceFiltered=*/false));
}

TEST_P(MetricsPolicyManagerDefaultFilteringTest, GetAllowlistPathsThrowsIllegalOperation) {
    auto manager = std::make_unique<MetricsPolicyManagerDefault>();
    ASSERT_THROWS_CODE(
        manager->getAllowlistPaths(GetParam()), DBException, ErrorCodes::IllegalOperation);
}

TEST_P(MetricsPolicyManagerDefaultFilteringTest, GetAllowlistMatcherThrowsIllegalOperation) {
    auto manager = std::make_unique<MetricsPolicyManagerDefault>();
    ASSERT_THROWS_CODE(
        manager->getAllowlistMatcher(GetParam()), DBException, ErrorCodes::IllegalOperation);
}

INSTANTIATE_TEST_SUITE_P(AllCategories,
                         MetricsPolicyManagerDefaultFilteringTest,
                         testing::Values(MetricsCategoryEnum::kServerStatus,
                                         MetricsCategoryEnum::kReplSetGetStatus,
                                         MetricsCategoryEnum::kCollStats,
                                         MetricsCategoryEnum::kDbStats),
                         [](const testing::TestParamInfo<MetricsCategoryEnum>& info) {
                             return std::string{idlSerialize(info.param)};
                         });

}  // namespace
}  // namespace mongo
