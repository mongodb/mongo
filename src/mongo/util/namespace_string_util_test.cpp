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

#include "mongo/platform/basic.h"

#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/namespace_string_util.h"

namespace mongo {

// TenantID is not included in serialization when multitenancySupport and
// featureFlagRequireTenantID are enabled.
TEST(NamespaceStringUtilTest, SerializeMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    TenantId tenantId(OID::gen());
    NamespaceString nss(tenantId, "foo.bar");
    ASSERT_EQ(NamespaceStringUtil::serialize(nss), "foo.bar");
}

// TenantID is included in serialization when multitenancySupport is enabled and
// featureFlagRequireTenantID is disabled.
TEST(NamespaceStringUtilTest, SerializeMultitenancySupportOnFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    TenantId tenantId(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
    NamespaceString nss(tenantId, "foo.bar");
    ASSERT_EQ(NamespaceStringUtil::serialize(nss), tenantNsStr);
}

// Serialize correctly when multitenancySupport is disabled.
TEST(NamespaceStringUtilTest, SerializeMultitenancySupportOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    NamespaceString nss(boost::none, "foo.bar");
    ASSERT_EQ(NamespaceStringUtil::serialize(nss), "foo.bar");
}

// Assert that if multitenancySupport and featureFlagRequireTenantID are on, then tenantId is set.
// TODO SERVER-70742 Uncomment out the massert below.
/* TEST(NamespaceStringUtilTest,
     DeserializeAssertTenantIdSetMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    ASSERT_THROWS_CODE(
        NamespaceStringUtil::deserialize(boost::none, "foo.bar"), AssertionException, 6972100);
} */

// Deserialize NamespaceString using the tenantID as a parameter to the NamespaceString constructor
// when multitenancySupport and featureFlagRequireTenantID are enabled and ns does not have prefixed
// tenantID.
TEST(NamespaceStringUtilTest,
     DeserializeNSSWithoutPrefixedTenantIDMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    TenantId tenantId(OID::gen());
    NamespaceString nss = NamespaceStringUtil::deserialize(tenantId, "foo.bar");
    ASSERT_EQ(nss.ns(), "foo.bar");
    ASSERT(nss.tenantId());
    ASSERT_EQ(nss, NamespaceString(tenantId, "foo.bar"));
}

// Assert that if multitenancySupport is enabled and featureFlagRequireTenantID is disabled,
// then tenantId parsed from ns and tenantID passed to NamespaceString object are equal.
TEST(NamespaceStringUtilTest,
     DeserializeAssertTenantIdSetMultitenancySupportOnFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    TenantId tenantId(OID::gen());
    TenantId tenantId2(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
    ASSERT_THROWS_CODE(
        NamespaceStringUtil::deserialize(tenantId2, tenantNsStr), AssertionException, 6972101);
}

// Deserialize NamespaceString when multitenancySupport is enabled and featureFlagRequireTenantID is
// disabled.
TEST(NamespaceStringUtilTest, DeserializeMultitenancySupportOnFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    TenantId tenantId(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
    NamespaceString nss = NamespaceStringUtil::deserialize(boost::none, tenantNsStr);
    NamespaceString nss1 = NamespaceStringUtil::deserialize(tenantId, tenantNsStr);
    ASSERT_EQ(nss.ns(), "foo.bar");
    ASSERT(nss.tenantId());
    ASSERT_EQ(nss, NamespaceString(tenantId, "foo.bar"));
    ASSERT_EQ(nss, nss1);
}

// Assert tenantID is not initialized when multitenancySupport is disabled.
TEST(NamespaceStringUtilTest, DeserializeMultitenancySupportOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    TenantId tenantId(OID::gen());
    ASSERT_THROWS_CODE(
        NamespaceStringUtil::deserialize(tenantId, "foo.bar"), AssertionException, 6972102);
}

// Deserialize NamespaceString with prefixed tenantId when multitenancySupport and
// featureFlagRequireTenantId are disabled.
TEST(NamespaceStringUtilTest,
     DeserializeWithTenantIdInStringMultitenancySupportOffFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    TenantId tenantId(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
    std::string dbNameStr = str::stream() << tenantId.toString() << "_foo";
    NamespaceString nss = NamespaceStringUtil::deserialize(boost::none, tenantNsStr);
    ASSERT_EQ(nss.tenantId(), boost::none);
    ASSERT_EQ(nss.dbName().db(), dbNameStr);
}

// Deserialize NamespaceString when multitenancySupport and featureFlagRequireTenantID are disabled.
TEST(NamespaceStringUtilTest, DeserializeMultitenancySupportOffFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    NamespaceString nss = NamespaceStringUtil::deserialize(boost::none, "foo.bar");
    ASSERT_EQ(nss.ns(), "foo.bar");
    ASSERT(!nss.tenantId());
    ASSERT_EQ(nss, NamespaceString(boost::none, "foo.bar"));
}

}  // namespace mongo
