// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using ResolveRoleOption = auth::AuthorizationBackendInterface::ResolveRoleOption;

// Represents a mapping between a ResolveRoleOption bitfield and the expected assertions
// from calling ResolveRoleOption's comparison methods.
struct ResolveRoleOptionTestCase {
    ResolveRoleOption option;
    std::function<void(const ResolveRoleOption&)> assertions;
};

std::array<ResolveRoleOptionTestCase, 12> resolveRoleOptionTestCases{
    ResolveRoleOption::kRoles(),
    [](const ResolveRoleOption& option) {
        ASSERT_TRUE(option.shouldMineRoles());
        ASSERT_FALSE(option.shouldMinePrivileges());
        ASSERT_FALSE(option.shouldMineRestrictions());
        ASSERT_FALSE(option.shouldMineDirectOnly());
        ASSERT_FALSE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kPrivileges(),
    [](const ResolveRoleOption& option) {
        ASSERT_FALSE(option.shouldMineRoles());
        ASSERT_TRUE(option.shouldMinePrivileges());
        ASSERT_FALSE(option.shouldMineRestrictions());
        ASSERT_FALSE(option.shouldMineDirectOnly());
        ASSERT_FALSE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kRestrictions(),
    [](const ResolveRoleOption& option) {
        ASSERT_FALSE(option.shouldMineRoles());
        ASSERT_FALSE(option.shouldMinePrivileges());
        ASSERT_TRUE(option.shouldMineRestrictions());
        ASSERT_FALSE(option.shouldMineDirectOnly());
        ASSERT_FALSE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kAllInfo(),
    [](const ResolveRoleOption& option) {
        ASSERT_TRUE(option.shouldMineRoles());
        ASSERT_TRUE(option.shouldMinePrivileges());
        ASSERT_TRUE(option.shouldMineRestrictions());
        ASSERT_FALSE(option.shouldMineDirectOnly());
        ASSERT_FALSE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kRoles().setDirectOnly(true /* shouldEnable */),
    [](const ResolveRoleOption& option) {
        ASSERT_TRUE(option.shouldMineRoles());
        ASSERT_FALSE(option.shouldMinePrivileges());
        ASSERT_FALSE(option.shouldMineRestrictions());
        ASSERT_TRUE(option.shouldMineDirectOnly());
        ASSERT_FALSE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kPrivileges().setDirectOnly(true /* shouldEnable */),
    [](const ResolveRoleOption& option) {
        ASSERT_FALSE(option.shouldMineRoles());
        ASSERT_TRUE(option.shouldMinePrivileges());
        ASSERT_FALSE(option.shouldMineRestrictions());
        ASSERT_TRUE(option.shouldMineDirectOnly());
        ASSERT_FALSE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kRestrictions().setDirectOnly(true /* shouldEnable */),
    [](const ResolveRoleOption& option) {
        ASSERT_FALSE(option.shouldMineRoles());
        ASSERT_FALSE(option.shouldMinePrivileges());
        ASSERT_TRUE(option.shouldMineRestrictions());
        ASSERT_TRUE(option.shouldMineDirectOnly());
        ASSERT_FALSE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kAllInfo().setDirectOnly(true /* shouldEnable */),
    [](const ResolveRoleOption& option) {
        ASSERT_TRUE(option.shouldMineRoles());
        ASSERT_TRUE(option.shouldMinePrivileges());
        ASSERT_TRUE(option.shouldMineRestrictions());
        ASSERT_TRUE(option.shouldMineDirectOnly());
        ASSERT_FALSE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kRoles().setIgnoreUnknown(true /* shouldEnable */),
    [](const ResolveRoleOption& option) {
        ASSERT_TRUE(option.shouldMineRoles());
        ASSERT_FALSE(option.shouldMinePrivileges());
        ASSERT_FALSE(option.shouldMineRestrictions());
        ASSERT_FALSE(option.shouldMineDirectOnly());
        ASSERT_TRUE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kPrivileges().setIgnoreUnknown(true /* shouldEnable */),
    [](const ResolveRoleOption& option) {
        ASSERT_FALSE(option.shouldMineRoles());
        ASSERT_TRUE(option.shouldMinePrivileges());
        ASSERT_FALSE(option.shouldMineRestrictions());
        ASSERT_FALSE(option.shouldMineDirectOnly());
        ASSERT_TRUE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kRestrictions().setIgnoreUnknown(true /* shouldEnable */),
    [](const ResolveRoleOption& option) {
        ASSERT_FALSE(option.shouldMineRoles());
        ASSERT_FALSE(option.shouldMinePrivileges());
        ASSERT_TRUE(option.shouldMineRestrictions());
        ASSERT_FALSE(option.shouldMineDirectOnly());
        ASSERT_TRUE(option.shouldIgnoreUnknown());
    },
    ResolveRoleOption::kAllInfo().setIgnoreUnknown(true /* shouldEnable */),
    [](const ResolveRoleOption& option) {
        ASSERT_TRUE(option.shouldMineRoles());
        ASSERT_TRUE(option.shouldMinePrivileges());
        ASSERT_TRUE(option.shouldMineRestrictions());
        ASSERT_FALSE(option.shouldMineDirectOnly());
        ASSERT_TRUE(option.shouldIgnoreUnknown());
    },
};

TEST(ResolveRoleOptionTest, testResolveRoleOption) {
    for (const auto& testCase : resolveRoleOptionTestCases) {
        // Pass the ResolveRoleOption specified in testCase.option as an argument
        // to the lambda in testCase.assertions.
        testCase.assertions(testCase.option);
    }
}

TEST(ResolveRoleOptionTest, testDisableResolveRoleOptionFlags) {
    // Start with just the role flag bit set.
    auto option = ResolveRoleOption::kRoles();
    ASSERT_TRUE(option.shouldMineRoles());
    ASSERT_FALSE(option.shouldMineDirectOnly());
    ASSERT_FALSE(option.shouldIgnoreUnknown());

    // Set the directOnly flag on the existing option.
    option.setDirectOnly(true /* shouldEnable */);
    ASSERT_TRUE(option.shouldMineRoles());
    ASSERT_TRUE(option.shouldMineDirectOnly());
    ASSERT_FALSE(option.shouldIgnoreUnknown());

    // Set the ignoreUnknown flag on the existing option.
    option.setIgnoreUnknown(true /* shouldEnable */);
    ASSERT_TRUE(option.shouldMineRoles());
    ASSERT_TRUE(option.shouldMineDirectOnly());
    ASSERT_TRUE(option.shouldIgnoreUnknown());

    // Unset directOnly.
    option.setDirectOnly(false /* shouldEnable */);
    ASSERT_TRUE(option.shouldMineRoles());
    ASSERT_FALSE(option.shouldMineDirectOnly());
    ASSERT_TRUE(option.shouldIgnoreUnknown());

    // Finally, unset ignoreUnknown.
    option.setIgnoreUnknown(false /* shouldEnable */);
    ASSERT_TRUE(option.shouldMineRoles());
    ASSERT_FALSE(option.shouldMineDirectOnly());
    ASSERT_FALSE(option.shouldIgnoreUnknown());
}

}  // namespace
}  // namespace mongo
