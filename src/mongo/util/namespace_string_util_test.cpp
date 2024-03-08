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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <initializer_list>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

namespace mongo {

TEST(AuthNamespaceStringUtil, Deserialize) {
    {
        RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
        ASSERT_THROWS_CODE(AuthNamespaceStringUtil::deserialize(TenantId{OID::gen()}, "foo", "bar"),
                           DBException,
                           ErrorCodes::InternalError);

        auto nss = AuthNamespaceStringUtil::deserialize(boost::none, "", "bar");
        ASSERT_EQ(nss.db_forTest(), "");
        ASSERT_EQ(nss.coll(), "bar");
        ASSERT_FALSE(nss.tenantId());
    }

    {
        RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
        TenantId tenant{OID::gen()};
        auto tenantNs = AuthNamespaceStringUtil::deserialize(tenant, "foo", "bar");
        ASSERT_EQ(tenantNs.tenantId(), tenant);
        ASSERT_EQ(tenantNs.db_forTest(), "foo");
        ASSERT_EQ(tenantNs.coll(), "bar");

        auto ns = AuthNamespaceStringUtil::deserialize(boost::none, "foo", "");
        ASSERT_FALSE(ns.tenantId());
        ASSERT_EQ(ns.db_forTest(), "foo");
        ASSERT_EQ(ns.coll(), "");
    }
}

// TenantID is not included in serialization when multitenancySupport and
// featureFlagRequireTenantID are enabled.
TEST(NamespaceStringUtilTest, SerializeMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    TenantId tenantId(OID::gen());
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tenantId, "foo.bar");
    ASSERT_EQ(NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()), "foo.bar");
}

// TenantID is included in serialization when multitenancySupport is enabled and
// featureFlagRequireTenantID is disabled.
TEST(NamespaceStringUtilTest, SerializeMultitenancySupportOnFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    TenantId tenantId(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tenantId, "foo.bar");
    ASSERT_EQ(NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()),
              tenantNsStr);
}

// Serialize correctly when multitenancySupport is disabled.
TEST(NamespaceStringUtilTest, SerializeMultitenancySupportOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(boost::none, "foo.bar");
    ASSERT_EQ(NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()), "foo.bar");
}

// Assert that if multitenancySupport and featureFlagRequireTenantID are on, then tenantId is set.
TEST(NamespaceStringUtilTest,
     DeserializeAssertTenantIdSetMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    ASSERT_THROWS_CODE(NamespaceStringUtil::deserialize(
                           boost::none, "foo.bar", SerializationContext::stateDefault()),
                       AssertionException,
                       6972100);
}

// Deserialize NamespaceString using the tenantID as a parameter to the NamespaceString constructor
// when multitenancySupport and featureFlagRequireTenantID are enabled and ns does not have prefixed
// tenantID.
TEST(NamespaceStringUtilTest,
     DeserializeNSSWithoutPrefixedTenantIDMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    TenantId tenantId(OID::gen());
    NamespaceString nss =
        NamespaceStringUtil::deserialize(tenantId, "foo.bar", SerializationContext::stateDefault());
    ASSERT_EQ(nss.ns_forTest(), "foo.bar");
    ASSERT(nss.tenantId());
    ASSERT_EQ(nss, NamespaceString::createNamespaceString_forTest(tenantId, "foo.bar"));
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
    ASSERT_THROWS_CODE(NamespaceStringUtil::deserialize(
                           tenantId2, tenantNsStr, SerializationContext::stateDefault()),
                       AssertionException,
                       6972101);
}

// Deserialize NamespaceString when multitenancySupport is enabled and featureFlagRequireTenantID is
// disabled.
TEST(NamespaceStringUtilTest, DeserializeMultitenancySupportOnFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    TenantId tenantId(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
    NamespaceString nss = NamespaceStringUtil::deserialize(
        boost::none, tenantNsStr, SerializationContext::stateDefault());
    NamespaceString nss1 = NamespaceStringUtil::deserialize(
        tenantId, tenantNsStr, SerializationContext::stateDefault());
    ASSERT_EQ(nss.ns_forTest(), "foo.bar");
    ASSERT(nss.tenantId());
    ASSERT_EQ(nss, NamespaceString::createNamespaceString_forTest(tenantId, "foo.bar"));
    ASSERT_EQ(nss, nss1);
}

// Assert tenantID is not initialized when multitenancySupport is disabled.
TEST(NamespaceStringUtilTest, DeserializeMultitenancySupportOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    TenantId tenantId(OID::gen());
    ASSERT_THROWS_CODE(
        NamespaceStringUtil::deserialize(tenantId, "foo.bar", SerializationContext::stateDefault()),
        AssertionException,
        6972102);
}

// Deserialize NamespaceString with prefixed tenantId when multitenancySupport and
// featureFlagRequireTenantId are disabled.
TEST(NamespaceStringUtilTest,
     DeserializeWithTenantIdInStringMultitenancySupportOffFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    TenantId tenantId(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";
    std::string dbNameStr = str::stream() << tenantId.toString() << "_foo";
    NamespaceString nss = NamespaceStringUtil::deserialize(
        boost::none, tenantNsStr, SerializationContext::stateDefault());
    ASSERT_EQ(nss.tenantId(), boost::none);
    ASSERT_EQ(nss.dbName().toString_forTest(), dbNameStr);
}

TEST(NamespaceStringUtilTest, NamespaceStringToDatabaseNameRoundTrip) {
    struct Scenario {
        bool multitenancy;
        boost::optional<TenantId> tenant;
        std::string database;
    };

    for (auto& scenario : {
             Scenario{false, boost::none, "foo"},
             Scenario{true, boost::none, "config"},
             Scenario{true, TenantId{OID::gen()}, "foo"},
         }) {
        RAIIServerParameterControllerForTest mc("multitenancySupport", scenario.multitenancy);

        auto expected = NamespaceString::createNamespaceString_forTest(
            scenario.tenant, scenario.database, "bar");
        auto actual = NamespaceStringUtil::deserialize(expected.dbName(), "bar");

        ASSERT_EQ(actual, expected);
    }
}

// Deserialize NamespaceString when multitenancySupport and featureFlagRequireTenantID are disabled.
TEST(NamespaceStringUtilTest, DeserializeMultitenancySupportOffFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    NamespaceString nss = NamespaceStringUtil::deserialize(
        boost::none, "foo.bar", SerializationContext::stateDefault());
    ASSERT_EQ(nss.ns_forTest(), "foo.bar");
    ASSERT(!nss.tenantId());
    ASSERT_EQ(nss, NamespaceString::createNamespaceString_forTest(boost::none, "foo.bar"));
}

// We will focus on specific configurations of the SerializationContext ie. Command Request and
// Command Reply as this is a defaulted parameter where tests that don't specify this parameter
// already test the default codepath.

TEST(NamespaceStringUtilTest, SerializeExpectPrefixFalse_CommandReply) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    const std::string nsString = "foo.bar";
    const std::string nsPrefixString = str::stream() << tenantId.toString() << "_" << nsString;

    SerializationContext ctxt_noTenantId(SerializationContext::stateCommandReply());
    ctxt_noTenantId.setTenantIdSource(false);
    SerializationContext ctxt_withTenantId(SerializationContext::stateCommandReply());
    ctxt_withTenantId.setTenantIdSource(true);

    {  // No prefix, no tenantId.
        // request --> { ns: database.coll }
        auto nss = NamespaceString::createNamespaceString_forTest(boost::none, nsString);
        ASSERT_EQ(NamespaceStringUtil::serialize(nss, ctxt_noTenantId), nsString);
    }

    {  // Has prefix, no tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: tenantId_database.coll }
        auto nss = NamespaceString::createNamespaceString_forTest(boost::none, nsPrefixString);
        ASSERT_EQ(NamespaceStringUtil::serialize(nss, ctxt_noTenantId), nsPrefixString);
    }

    {  // No prefix, has tenantId.
        // request --> { ns: database.coll }
        auto nss = NamespaceString::createNamespaceString_forTest(tenantId, nsString);
        ASSERT_EQ(NamespaceStringUtil::serialize(nss, ctxt_withTenantId), nsString);
    }

    {  // Has prefix, has tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: tenantId_database.coll }
        auto nss = NamespaceString::createNamespaceString_forTest(tenantId, nsPrefixString);
        ASSERT_EQ(NamespaceStringUtil::serialize(nss, ctxt_withTenantId), nsPrefixString);
    }
}

// Serializing with SerializationContext, with an expectPrefix set to true
TEST(NamespaceStringUtilTest, SerializeExpectPrefixTrue_CommandReply) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    const std::string nsString = "foo.bar";
    const std::string nsPrefixString = str::stream() << tenantId.toString() << "_" << nsString;

    SerializationContext ctxt_noTenantId(SerializationContext::stateCommandReply());
    ctxt_noTenantId.setTenantIdSource(false);
    ctxt_noTenantId.setPrefixState(true);
    SerializationContext ctxt_withTenantId(SerializationContext::stateCommandReply());
    ctxt_withTenantId.setTenantIdSource(true);
    ctxt_withTenantId.setPrefixState(true);

    {  // No prefix, no tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: database.coll, expectPrefix: true }
        auto nss = NamespaceString::createNamespaceString_forTest(boost::none, nsString);
        ASSERT_EQ(NamespaceStringUtil::serialize(nss, ctxt_noTenantId), nsString);
    }

    {  // Has prefix, no tenantId.
        // request --> { ns: tenantId_database.coll, expectPrefix: true }
        auto nss = NamespaceString::createNamespaceString_forTest(boost::none, nsPrefixString);
        ASSERT_EQ(NamespaceStringUtil::serialize(nss, ctxt_noTenantId), nsPrefixString);
    }

    {  // No prefix, has tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: database.coll, expectPrefix: true }
        auto nss = NamespaceString::createNamespaceString_forTest(tenantId, nsString);
        ASSERT_EQ(NamespaceStringUtil::serialize(nss, ctxt_withTenantId), nsPrefixString);
    }

    {  // Has prefix, has tenantId.
        // request -->  { ns: tenantId_database.coll, expectPrefix: true }
        const std::string nsDoublePrefixString = str::stream()
            << tenantId.toString() << "_" << tenantId.toString() << "_" << nsString;
        auto nss = NamespaceString::createNamespaceString_forTest(tenantId, nsPrefixString);
        ASSERT_EQ(NamespaceStringUtil::serialize(nss, ctxt_withTenantId), nsDoublePrefixString);
    }
}

TEST(NamespaceStringUtilTest, DeserializeExpectPrefixFalse_CommandRequest) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    const std::string nsString = "foo.bar";
    const std::string nsPrefixString = str::stream() << tenantId.toString() << "_" << nsString;

    SerializationContext ctxt_noTenantId(SerializationContext::stateCommandRequest());
    ctxt_noTenantId.setTenantIdSource(false);
    SerializationContext ctxt_withTenantId(SerializationContext::stateCommandRequest());
    ctxt_withTenantId.setTenantIdSource(true);

    {  // No prefix, no tenantId.  *** we shouldn't see this from Atlas Proxy in MT mode
        // request --> { ns: database.coll }
        ASSERT_THROWS_CODE(NamespaceStringUtil::deserialize(boost::none, nsString, ctxt_noTenantId),
                           AssertionException,
                           8423387);
    }

    {  // Has prefix, no tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: tenantId_database.coll }
        ASSERT_THROWS_CODE(
            NamespaceStringUtil::deserialize(boost::none, nsPrefixString, ctxt_noTenantId),
            AssertionException,
            8233501);
    }

    {  // No prefix, has tenantId.
        // request --> { ns: database.coll }
        auto nss = NamespaceStringUtil::deserialize(tenantId, nsString, ctxt_withTenantId);
        ASSERT_EQ(nss.tenantId(), tenantId);
        ASSERT_EQ(nss.toString_forTest(), nsString);
    }

    {  // Has prefix, has tenantId.  ** Allowed but the prefix is *NOT* stripped (even if it's a
        // valid tenant ID).
        // request --> { ns: tenantId_database.coll }
        auto nss = NamespaceStringUtil::deserialize(tenantId, "foo_bar.baz", ctxt_withTenantId);
        ASSERT_EQ(nss.tenantId(), tenantId);
        ASSERT_EQ(nss.toString_forTest(), "foo_bar.baz");

        nss = NamespaceStringUtil::deserialize(tenantId, nsPrefixString, ctxt_withTenantId);
        ASSERT_EQ(nss.tenantId(), tenantId);
        ASSERT_EQ(nss.toString_forTest(), nsPrefixString);
    }
}

TEST(NamespaceStringUtilTest, DeserializeExpectPrefixTrue_CommandRequest) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    const std::string nsString = "foo.bar";
    const std::string nsPrefixString = str::stream() << tenantId.toString() << "_" << nsString;

    SerializationContext ctxt_noTenantId(SerializationContext::stateCommandRequest());
    ctxt_noTenantId.setTenantIdSource(false);
    ctxt_noTenantId.setPrefixState(true);
    SerializationContext ctxt_withTenantId(SerializationContext::stateCommandRequest());
    ctxt_withTenantId.setTenantIdSource(true);
    ctxt_withTenantId.setPrefixState(true);

    {  // No prefix, no tenantId.  *** we shouldn't see this from Atlas Proxy in MT mode
        // request --> { ns: database.coll, expectPrefix: true }
        ASSERT_THROWS_CODE(NamespaceStringUtil::deserialize(boost::none, nsString, ctxt_noTenantId),
                           AssertionException,
                           8423387);
    }

    {  // Has prefix, no tenantId.  Not valid.
        // request --> { ns: tenantId_database.coll, expectPrefix: true }
        ASSERT_THROWS_CODE(
            NamespaceStringUtil::deserialize(boost::none, nsPrefixString, ctxt_noTenantId),
            AssertionException,
            8233501);
    }

    {  // No prefix, has tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: database.coll, expectPrefix: true }
        ASSERT_THROWS_CODE(NamespaceStringUtil::deserialize(tenantId, nsString, ctxt_withTenantId),
                           AssertionException,
                           8423385);
    }

    {  // Has prefix, has tenantId.
        // request -->  { ns: tenantId_database.coll, expectPrefix: true }
        auto nss = NamespaceStringUtil::deserialize(tenantId, nsPrefixString, ctxt_withTenantId);
        ASSERT_EQ(nss.tenantId(), tenantId);
        ASSERT_EQ(nss.toString_forTest(), nsString);
    }
}

TEST(NamespaceStringUtilTest, ParseNSSWithTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    TenantId tenantId(OID::gen());
    std::string tenantNsStr = str::stream() << tenantId.toString() << "_foo.bar";

    NamespaceString nss =
        NamespaceStringUtil::parseFromStringExpectTenantIdInMultitenancyMode(tenantNsStr);
    ASSERT_EQ(nss.ns_forTest(), "foo.bar");
    ASSERT_EQ(nss.toStringWithTenantId_forTest(), tenantNsStr);
    ASSERT(nss.tenantId());
    ASSERT_EQ(*nss.tenantId(), tenantId);
}

TEST(NamespaceStringUtilTest, ParseFailPointData) {
    // Test fail point data only has name space.
    {
        BSONObjBuilder bob;
        bob.append("nss", "myDb.myColl");
        const auto fpNss = NamespaceStringUtil::parseFailPointData(bob.obj(), "nss"_sd);
        ASSERT_EQ(NamespaceString::createNamespaceString_forTest(boost::none, "myDb.myColl"),
                  fpNss);
    }
    // Test fail point data is empty.
    {
        auto fpNss = NamespaceStringUtil::parseFailPointData(BSONObj(), "nss"_sd);
        ASSERT_EQ(NamespaceString(), fpNss);
    }
}

TEST(NamespaceStringUtilTest, SerializingEmptyNamespaceSting) {
    const auto emptyNss = NamespaceString();
    const auto kEmptyNss = NamespaceString::kEmpty;
    const auto sc = SerializationContext::stateDefault();
    ASSERT_EQ(NamespaceStringUtil::serialize(emptyNss, sc),
              NamespaceStringUtil::serialize(kEmptyNss, sc));

    const auto emptyDbNss =
        NamespaceString::createNamespaceString_forTest(DatabaseName::kEmpty, "");
    ASSERT_EQ(NamespaceStringUtil::serialize(emptyDbNss, sc),
              NamespaceStringUtil::serialize(kEmptyNss, sc));
    {
        RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
        const TenantId tid = TenantId(OID::gen());
        const auto emptyTenantIdDbNss =
            NamespaceString(DatabaseName::createDatabaseName_forTest(tid, ""));
        const std::string expectedSerialization = str::stream() << tid.toString() << "_";
        ASSERT_EQ(NamespaceStringUtil::serialize(emptyTenantIdDbNss, sc), expectedSerialization);
    }
}

TEST(NamespaceStringUtilTest, CheckEmptyCollectionSerialize) {
    const auto serializeCtx = SerializationContext::stateDefault();
    const TenantId tenantId(OID::gen());

    const auto dbName = DatabaseName::createDatabaseName_forTest(tenantId, "dbTest");
    const auto nssEmptyColl = NamespaceString::createNamespaceString_forTest(dbName, "");
    const auto nssEmptySerialized = NamespaceStringUtil::serialize(nssEmptyColl, serializeCtx);
    ASSERT_EQ(nssEmptySerialized, "dbTest");
}

}  // namespace mongo
