/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
