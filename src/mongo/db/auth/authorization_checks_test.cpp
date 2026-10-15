// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/authorization_checks.h"

#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>

#include <boost/optional/optional.hpp>

namespace mongo::auth {
namespace {

const auto kMultiUpdateCoordinatorsNss = NamespaceString::kMultiUpdateCoordinatorsNamespace;
const auto kAuthNss = NamespaceString::kAuthForInternalNamespace;
const ActionSet kInsert{ActionType::insert};

TEST(AuthorizationChecksTest, IsRestrictedNamespace) {
    ASSERT_TRUE(isRestrictedNamespace(kMultiUpdateCoordinatorsNss));
    ASSERT_FALSE(isRestrictedNamespace(NamespaceString::createNamespaceString_forTest("test.foo")));

    // Same collection name in another database, and another collection in 'config'.
    ASSERT_FALSE(isRestrictedNamespace(NamespaceString::createNamespaceString_forTest(
        "test", kMultiUpdateCoordinatorsNss.coll())));
    ASSERT_FALSE(isRestrictedNamespace(
        NamespaceString::createNamespaceString_forTest("config", "someOtherCollection")));
}

// A tenanted request naming the same collection must not sidestep the check by carrying a tenant
// id.
TEST(AuthorizationChecksTest, IsRestrictedNamespaceIgnoresTenantId) {
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);

    const TenantId tenantId{OID::gen()};
    ASSERT_TRUE(isRestrictedNamespace(
        AuthNamespaceStringUtil::deserialize(tenantId,
                                             kMultiUpdateCoordinatorsNss.dbName().db(omitTenant),
                                             kMultiUpdateCoordinatorsNss.coll())));
}

TEST(AuthorizationChecksTest, NoImplicitPrivilegeForOrdinaryNamespace) {
    ASSERT_FALSE(getImplicitPrivilegeForRestrictedNamespace(
                     ResourcePattern::forExactNamespace(
                         NamespaceString::createNamespaceString_forTest("test.foo")),
                     kInsert)
                     .has_value());
}

TEST(AuthorizationChecksTest, ImplicitPrivilegeForMultiUpdateCoordinatorsNamespace) {
    auto privilege = getImplicitPrivilegeForRestrictedNamespace(
        ResourcePattern::forExactNamespace(kMultiUpdateCoordinatorsNss), kInsert);
    ASSERT_TRUE(privilege.has_value());
    ASSERT_EQ(privilege->getResourcePattern(), ResourcePattern::forExactNamespace(kAuthNss));
    ASSERT_TRUE(privilege->getActions() == kInsert);
}

TEST(AuthorizationChecksTest, ImplicitPrivilegeCarriesAllRequestedActions) {
    const ActionSet actions{ActionType::find, ActionType::update, ActionType::insert};
    auto privilege = getImplicitPrivilegeForRestrictedNamespace(
        ResourcePattern::forExactNamespace(kMultiUpdateCoordinatorsNss), actions);
    ASSERT_TRUE(privilege.has_value());
    ASSERT_TRUE(privilege->getActions() == actions);
}

TEST(AuthorizationChecksTest, NoImplicitPrivilegeForEmptyActionSet) {
    ASSERT_FALSE(getImplicitPrivilegeForRestrictedNamespace(
                     ResourcePattern::forExactNamespace(kMultiUpdateCoordinatorsNss), ActionSet{})
                     .has_value());
}

// A database or wildcard pattern must not pull in the implicit privilege; only an exact namespace
// match names the restricted collection.
TEST(AuthorizationChecksTest, NoImplicitPrivilegeForNonExactPatterns) {
    ASSERT_FALSE(
        getImplicitPrivilegeForRestrictedNamespace(
            ResourcePattern::forDatabaseName(kMultiUpdateCoordinatorsNss.dbName()), kInsert)
            .has_value());
    ASSERT_FALSE(getImplicitPrivilegeForRestrictedNamespace(
                     ResourcePattern::forAnyResource(boost::none), kInsert)
                     .has_value());
}

// A tenanted request naming the same collection must be caught, and must be redirected to the
// pseudo namespace within the same tenant.
TEST(AuthorizationChecksTest, ImplicitPrivilegeIsTenantAware) {
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);

    const TenantId tenantId{OID::gen()};
    const auto tenantedNss =
        AuthNamespaceStringUtil::deserialize(tenantId,
                                             kMultiUpdateCoordinatorsNss.dbName().db(omitTenant),
                                             kMultiUpdateCoordinatorsNss.coll());

    auto privilege = getImplicitPrivilegeForRestrictedNamespace(
        ResourcePattern::forExactNamespace(tenantedNss), kInsert);
    ASSERT_TRUE(privilege.has_value());
    ASSERT_EQ(privilege->getResourcePattern().tenantId(), tenantId);
    ASSERT_EQ(privilege->getResourcePattern().ns().coll(), kAuthNss.coll());
}

TEST(AuthorizationChecksTest, AddImplicitPrivilegesLeavesOrdinaryVectorUntouched) {
    PrivilegeVector privileges{
        Privilege(ResourcePattern::forExactNamespace(
                      NamespaceString::createNamespaceString_forTest("test.foo")),
                  kInsert)};
    const auto original = privileges;
    addImplicitPrivilegesForRestrictedNamespaces(&privileges);
    ASSERT_TRUE(privileges == original);
}

TEST(AuthorizationChecksTest, AddImplicitPrivilegesAppendsForRestrictedNamespace) {
    PrivilegeVector privileges{
        Privilege(ResourcePattern::forExactNamespace(kMultiUpdateCoordinatorsNss), kInsert)};
    addImplicitPrivilegesForRestrictedNamespaces(&privileges);

    ASSERT_EQ(privileges.size(), 2u);
    ASSERT_TRUE(std::any_of(privileges.begin(), privileges.end(), [](const Privilege& privilege) {
        return privilege.getResourcePattern() == ResourcePattern::forExactNamespace(kAuthNss) &&
            privilege.getActions() == kInsert;
    }));
}

// A pipeline may name the restricted namespace as a secondary source alongside others; only the
// restricted one gets an implicit privilege, and the caller's own privileges are preserved.
TEST(AuthorizationChecksTest, AddImplicitPrivilegesHandlesMixedVector) {
    PrivilegeVector privileges{
        Privilege(ResourcePattern::forExactNamespace(
                      NamespaceString::createNamespaceString_forTest("test.foo")),
                  ActionSet{ActionType::find}),
        Privilege(ResourcePattern::forExactNamespace(kMultiUpdateCoordinatorsNss),
                  ActionSet{ActionType::find})};
    addImplicitPrivilegesForRestrictedNamespaces(&privileges);

    ASSERT_EQ(privileges.size(), 3u);
    ASSERT_TRUE(std::any_of(privileges.begin(), privileges.end(), [](const Privilege& privilege) {
        return privilege.getResourcePattern() == ResourcePattern::forExactNamespace(kAuthNss);
    }));
}

TEST(AuthorizationChecksTest, AddImplicitPrivilegesIsIdempotent) {
    PrivilegeVector privileges{
        Privilege(ResourcePattern::forExactNamespace(kMultiUpdateCoordinatorsNss), kInsert)};
    addImplicitPrivilegesForRestrictedNamespaces(&privileges);
    const auto once = privileges;
    addImplicitPrivilegesForRestrictedNamespaces(&privileges);
    ASSERT_TRUE(privileges == once);
}

}  // namespace
}  // namespace mongo::auth
