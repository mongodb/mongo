/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/tenant_database_name.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(TenantDatabaseNameTest, MultitenancySupportDisabled) {
    TenantDatabaseName tdnWithoutTenant1(boost::none, "a");

    ASSERT(!tdnWithoutTenant1.tenantId());
    ASSERT_EQUALS(std::string("a"), tdnWithoutTenant1.dbName());
    ASSERT_EQUALS(std::string("a"), tdnWithoutTenant1.fullName());

    TenantId tenantId(OID::gen());
    TenantDatabaseName tdnWithTenant(tenantId, "a");
    ASSERT(tdnWithTenant.tenantId());
    ASSERT_EQUALS(tenantId, *tdnWithTenant.tenantId());
    ASSERT_EQUALS(std::string("a"), tdnWithTenant.dbName());
    ASSERT_EQUALS(std::string(tenantId.toString() + "_a"), tdnWithTenant.fullName());
}

TEST(TenantDatabaseNameTest, MultitenancySupportEnabledTenantIDNotRequired) {
    // TODO SERVER-62114 remove this test case.
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);

    TenantDatabaseName tdnWithoutTenant(boost::none, "a");
    ASSERT(!tdnWithoutTenant.tenantId());
    ASSERT_EQUALS(std::string("a"), tdnWithoutTenant.dbName());
    ASSERT_EQUALS(std::string("a"), tdnWithoutTenant.fullName());

    TenantId tenantId(OID::gen());
    TenantDatabaseName tdnWithTenant(tenantId, "a");
    ASSERT(tdnWithTenant.tenantId());
    ASSERT_EQUALS(tenantId, *tdnWithTenant.tenantId());
    ASSERT_EQUALS(std::string("a"), tdnWithTenant.dbName());
    ASSERT_EQUALS(std::string(tenantId.toString() + "_a"), tdnWithTenant.fullName());
}

/*
// TODO SERVER-65457 Re-enable these tests

DEATH_TEST(TenantDatabaseNameTest, TenantIDRequiredNoTenantIdAssigned, "invariant") {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);

    TenantDatabaseName tdnWithoutTenant(boost::none, "a");
}

TEST(TenantDatabaseNameTest, TenantIDRequiredBasic) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    // TODO SERVER-62114 Remove enabling this feature flag.
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    TenantId tenantId(OID::gen());
    TenantDatabaseName tdn(tenantId, "a");
    ASSERT(tdn.tenantId());
    ASSERT_EQUALS(tenantId, *tdn.tenantId());
    ASSERT_EQUALS(std::string("a"), tdn.dbName());
    ASSERT_EQUALS(std::string(tenantId.toString() + "_a"), tdn.fullName());
}
*/

TEST(TenantDatabaseNameTest, VerifyEqualsOperator) {
    TenantId tenantId(OID::gen());
    TenantDatabaseName tdn(tenantId, "a");
    ASSERT_TRUE(TenantDatabaseName(tenantId, "a") == tdn);
    ASSERT_TRUE(TenantDatabaseName(tenantId, "b") != tdn);

    TenantId otherTenantId = TenantId(OID::gen());
    ASSERT_TRUE(TenantDatabaseName(otherTenantId, "a") != tdn);
    ASSERT_TRUE(TenantDatabaseName(boost::none, "a") != tdn);
}

TEST(TenantDatabaseNameTest, VerifyHashFunction) {
    TenantId tenantId1(OID::gen());
    TenantId tenantId2(OID::gen());
    TenantDatabaseName tdn1 = TenantDatabaseName(tenantId1, "a");
    TenantDatabaseName tdn2 = TenantDatabaseName(tenantId2, "a");
    TenantDatabaseName tdn3 = TenantDatabaseName(boost::none, "a");

    stdx::unordered_map<TenantDatabaseName, std::string> dbMap;

    dbMap[tdn1] = "value T1 a1";
    ASSERT_EQUALS(dbMap[tdn1], "value T1 a1");
    dbMap[tdn1] = "value T1 a2";
    ASSERT_EQUALS(dbMap[tdn1], "value T1 a2");
    dbMap[TenantDatabaseName(tenantId1, "a")] = "value T1 a3";
    ASSERT_EQUALS(dbMap[tdn1], "value T1 a3");

    dbMap[tdn2] = "value T2 a1";
    ASSERT_EQUALS(dbMap[tdn2], "value T2 a1");
    dbMap[tdn2] = "value T2 a2";

    dbMap[tdn3] = "value no tenant a1";
    ASSERT_EQUALS(dbMap[tdn3], "value no tenant a1");
    dbMap[tdn3] = "value no tenant a2";

    // verify all key-value in map to ensure all data is correct.
    ASSERT_EQUALS(dbMap[tdn1], "value T1 a3");
    ASSERT_EQUALS(dbMap[tdn2], "value T2 a2");
    ASSERT_EQUALS(dbMap[tdn3], "value no tenant a2");
}

TEST(TenantDatabaseNameTest, VerifyCompareFunction) {
    TenantId tenantId1 = TenantId(OID::gen());
    TenantId tenantId2 = TenantId(OID::gen());

    // OID's generated by the same process are monotonically increasing.
    ASSERT(tenantId1 < tenantId2);

    TenantDatabaseName tdn1a = TenantDatabaseName(tenantId1, "a");
    TenantDatabaseName tdn1b = TenantDatabaseName(tenantId1, "b");
    TenantDatabaseName tdn2a = TenantDatabaseName(tenantId2, "a");
    TenantDatabaseName tdn3a = TenantDatabaseName(boost::none, "a");

    ASSERT(tdn1a < tdn1b);
    ASSERT(tdn1b < tdn2a);
    ASSERT(tdn3a != tdn1a);
    ASSERT(tdn1a != tdn2a);
}
}  // namespace
}  // namespace mongo
