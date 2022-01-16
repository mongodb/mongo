/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/tenant_namespace.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(TenantNamespaceTest, TenantNamespaceMultitenancySupportDisabledBasic) {
    TenantNamespace tenantNs(boost::none, NamespaceString("a.b"));
    ASSERT(!tenantNs.tenantId());
    ASSERT_EQUALS(std::string("a"), tenantNs.db());
    ASSERT_EQUALS(std::string("b"), tenantNs.coll());
    ASSERT_EQUALS(std::string("a.b"), tenantNs.toString());
}

TEST(TenantNamespaceTest, TenantNamespaceParseFromDiskMultitenancySupportDisabled) {
    TenantNamespace tenantNs = TenantNamespace::parseTenantNamespaceFromDisk("a.b");
    ASSERT(!tenantNs.tenantId());
    ASSERT_EQUALS(std::string("a"), tenantNs.db());
    ASSERT_EQUALS(std::string("b"), tenantNs.coll());

    mongo::OID tenantId = OID::gen();
    std::string ns = tenantId.toString() + "_a.b";
    TenantNamespace tenantNs2 = TenantNamespace::parseTenantNamespaceFromDisk(ns);
    ASSERT(!tenantNs2.tenantId());
    ASSERT_EQUALS(std::string(tenantId.toString() + "_a"), tenantNs2.db());
    ASSERT_EQUALS(std::string("b"), tenantNs2.coll());
}

TEST(TenantNamespaceTest, TenantNamespaceMultitenancySupportEnabledFeatureFlagDisabledBasic) {
    // TODO SERVER-62114 Remove this test case.
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);

    // If the feature flag is disabled, it's acceptable for the tenantId not to exist.
    TenantNamespace tenantNs(boost::none, NamespaceString("a.b"));
    ASSERT(!tenantNs.tenantId());
    ASSERT_EQUALS(std::string("a"), tenantNs.db());
    ASSERT_EQUALS(std::string("b"), tenantNs.coll());
    ASSERT_EQUALS(std::string("a.b"), tenantNs.toString());

    // If the feature flag is disabled but a tenantId is given, the tenantId should be parsed
    // separately from the db name.
    mongo::OID tenantId = OID::gen();
    TenantNamespace tenantNs2(tenantId, NamespaceString("a.b"));
    ASSERT(tenantNs2.tenantId());
    ASSERT_EQUALS(tenantId, *tenantNs2.tenantId());
    ASSERT_EQUALS(std::string("a"), tenantNs2.db());
    ASSERT_EQUALS(std::string("b"), tenantNs2.coll());
    ASSERT_EQUALS(std::string(tenantId.toString() + "_a.b"), tenantNs2.toString());
}

DEATH_TEST(TenantNamespaceTest,
           TenantNamespaceMultitenancySupportEnabledTenantIDRequired,
           "invariant") {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    // TODO SERVER-62114 Remove enabling this feature flag.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    // A tenantId is not included, so the server should crash
    TenantNamespace(boost::none, NamespaceString("a.b"));
}

DEATH_TEST(TenantNamespaceTest,
           TenantNamespaceParseFromDiskMultitenancySupportEnabledTenantIDRequired,
           "invariant") {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    // TODO SERVER-62114 Remove enabling this feature flag.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    // A tenantId is not included, so the server should crash
    auto tenantNs = TenantNamespace::parseTenantNamespaceFromDisk("a.b");
}

TEST(TenantNamespaceTest, TenantNamespaceMultitenancySupportEnabledBasic) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    // TODO SERVER-62114 Remove enabling this feature flag.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    mongo::OID tenantId = OID::gen();
    TenantNamespace tenantNs(tenantId, NamespaceString("a.b"));
    ASSERT(tenantNs.tenantId());
    ASSERT_EQUALS(tenantId, *tenantNs.tenantId());
    ASSERT_EQUALS(std::string("a"), tenantNs.db());
    ASSERT_EQUALS(std::string("b"), tenantNs.coll());
    ASSERT_EQUALS(std::string(tenantId.toString() + "_a.b"), tenantNs.toString());
}

TEST(TenantNamespaceTest, TenantNamespaceParseFromDiskMultitenancySupportEnabled) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    // TODO SERVER-62114 Remove enabling this feature flag.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    mongo::OID tenantId = OID::gen();
    std::string tenantNsStr = str::stream() << tenantId << "_a.b";

    TenantNamespace tenantNs = TenantNamespace::parseTenantNamespaceFromDisk(tenantNsStr);
    ASSERT(tenantNs.tenantId());
    ASSERT_EQUALS(tenantId, *tenantNs.tenantId());
    ASSERT_EQUALS(std::string("a"), tenantNs.db());
    ASSERT_EQUALS(std::string("b"), tenantNs.coll());
}

}  // namespace
}  // namespace mongo
