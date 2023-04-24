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

#include "mongo/db/database_name.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/database_name_util.h"

namespace mongo {

// TenantID is not included in serialization when multitenancySupport and
// featureFlagRequireTenantID are enabled.
TEST(DatabaseNameUtilTest, SerializeMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    TenantId tenantId(OID::gen());
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
    ASSERT_EQ(DatabaseNameUtil::serialize(dbName), "foo");
}

// TenantID is included in serialization when multitenancySupport is enabled and
// featureFlagRequireTenantID is disabled.
TEST(DatabaseNameUtilTest, SerializeMultitenancySupportOnFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    TenantId tenantId(OID::gen());
    std::string tenantDbStr = str::stream() << tenantId.toString() << "_foo";
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(tenantId, "foo");
    ASSERT_EQ(DatabaseNameUtil::serialize(dbName), tenantDbStr);
}

// Serialize correctly when multitenancySupport is disabled.
TEST(DatabaseNameUtilTest, SerializeMultitenancySupportOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "foo");
    ASSERT_EQ(DatabaseNameUtil::serialize(dbName), "foo");
}

// Assert that if multitenancySupport and featureFlagRequireTenantID are on, then tenantId is set.
// TODO SERVER-73025 Uncomment out the test case below.
/* TEST(DatabaseNameUtilTest,
     DeserializeAssertTenantIdSetMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    ASSERT_THROWS_CODE(
        DatabaseNameUtil::deserialize(boost::none, "foo"), AssertionException, 7005300);
}
*/

// If the database is an inernal db, it's acceptable not to have a tenantId even if
// multitenancySupport and featureFlagRequireTenantID are on.
TEST(DatabaseNameUtilTest,
     DeserializeInternalDbTenantIdSetMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    DatabaseName dbName = DatabaseNameUtil::deserialize(boost::none, "local");
    ASSERT_EQ(dbName, DatabaseName::kLocal);
}

// Deserialize DatabaseName using the tenantID as a parameter to the DatabaseName constructor
// when multitenancySupport and featureFlagRequireTenantID are enabled and ns does not have prefixed
// tenantID.
TEST(DatabaseNameUtilTest,
     DeserializeNSSWithoutPrefixedTenantIDMultitenancySupportOnFeatureFlagRequireTenantIDOn) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    TenantId tenantId(OID::gen());
    DatabaseName dbName = DatabaseNameUtil::deserialize(tenantId, "foo");
    ASSERT_EQ(dbName.db(), "foo");
    ASSERT(dbName.tenantId());
    ASSERT_EQ(dbName, DatabaseName::createDatabaseName_forTest(tenantId, "foo"));
}

// Deserialize DatabaseName when multitenancySupport is enabled and featureFlagRequireTenantID is
// disabled.
TEST(DatabaseNameUtilTest, DeserializeMultitenancySupportOnFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    TenantId tenantId(OID::gen());
    std::string tenantDbStr = str::stream() << tenantId.toString() << "_foo";
    DatabaseName dbName = DatabaseNameUtil::deserialize(boost::none, tenantDbStr);
    DatabaseName dbName1 = DatabaseNameUtil::deserialize(tenantId, tenantDbStr);
    ASSERT_EQ(dbName.db(), "foo");
    ASSERT(dbName.tenantId());
    ASSERT_EQ(dbName, DatabaseName::createDatabaseName_forTest(tenantId, "foo"));
    ASSERT_EQ(dbName, dbName1);
}

// Assert tenantID is not initialized when multitenancySupport is disabled.
TEST(DatabaseNameUtilTest, DeserializeMultitenancySupportOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    TenantId tenantId(OID::gen());
    ASSERT_THROWS_CODE(DatabaseNameUtil::deserialize(tenantId, "foo"), AssertionException, 7005302);
}

// Deserialize DatabaseName with prefixed tenantId when multitenancySupport and
// featureFlagRequireTenantId are disabled.
TEST(DatabaseNameUtilTest,
     DeserializeWithTenantIdInStringMultitenancySupportOffFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    TenantId tenantId(OID::gen());
    std::string dbNameStr = str::stream() << tenantId.toString() << "_foo";
    DatabaseName dbName = DatabaseNameUtil::deserialize(boost::none, dbNameStr);
    ASSERT_EQ(dbName.tenantId(), boost::none);
    ASSERT_EQ(dbName.db(), dbNameStr);
}

// Deserialize DatabaseName when multitenancySupport and featureFlagRequireTenantID are disabled.
TEST(DatabaseNameUtilTest, DeserializeMultitenancySupportOffFeatureFlagRequireTenantIDOff) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", false);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    DatabaseName dbName = DatabaseNameUtil::deserialize(boost::none, "foo");
    ASSERT_EQ(dbName.db(), "foo");
    ASSERT(!dbName.tenantId());
    ASSERT_EQ(dbName, DatabaseName::createDatabaseName_forTest(boost::none, "foo"));
}

// We will focus on specific configurations of the SerializationContext ie. Command Request and
// Command Reply as this is a defaulted parameter where tests that don't specify this parameter
// already test the default codepath.

TEST(DatabaseNameUtilTest, SerializeMissingExpectPrefix_CommandReply) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    const std::string dbnString = "foo";
    const std::string dbnPrefixString = str::stream() << tenantId.toString() << "_" << dbnString;

    SerializationContext ctxt_noTenantId(SerializationContext::stateCommandReply());
    ctxt_noTenantId.setTenantIdSource(false);
    SerializationContext ctxt_withTenantId(SerializationContext::stateCommandReply());
    ctxt_withTenantId.setTenantIdSource(true);

    // TODO SERVER-74284: call the serialize/deserialize entry points instead

    {  // No prefix, no tenantId.
        // request --> { ns: database.coll }
        auto dbName = DatabaseName(boost::none, dbnString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_noTenantId), dbnString);
    }

    {  // Has prefix, no tenantId.
        // request --> { ns: tenantId_database.coll }
        auto dbName = DatabaseName(boost::none, dbnPrefixString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_noTenantId), dbnPrefixString);
    }

    {  // No prefix, has tenantId.
        // request --> { ns: database.coll, $tenant: tenantId }
        auto dbName = DatabaseName(tenantId, dbnString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_withTenantId), dbnString);
    }

    {  // Has prefix, has tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: tenantId_database.coll, $tenant: tenantId }
        auto dbName = DatabaseName(tenantId, dbnPrefixString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_withTenantId),
                  dbnPrefixString);
    }
}

TEST(DatabaseNameUtilTest, SerializeExpectPrefixFalse_CommandReply) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    const std::string dbnString = "foo";
    const std::string dbnPrefixString = str::stream() << tenantId.toString() << "_" << dbnString;

    SerializationContext ctxt_noTenantId(SerializationContext::stateCommandReply());
    ctxt_noTenantId.setTenantIdSource(false);
    ctxt_noTenantId.setPrefixState(false);
    SerializationContext ctxt_withTenantId(SerializationContext::stateCommandReply());
    ctxt_withTenantId.setTenantIdSource(true);
    ctxt_withTenantId.setPrefixState(false);

    // TODO SERVER-74284: call the serialize/deserialize entry points instead

    {  // No prefix, no tenantId.
        // request --> { ns: database.coll, expectPrefix: false }
        auto dbName = DatabaseName(boost::none, dbnString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_noTenantId), dbnString);
    }

    {  // Has prefix, no tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: tenantId_database.coll, expectPrefix: false }
        auto dbName = DatabaseName(boost::none, dbnPrefixString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_noTenantId), dbnPrefixString);
    }

    {  // No prefix, has tenantId.
        // request --> { ns: database.coll, $tenant: tenantId, expectPrefix: false }
        auto dbName = DatabaseName(tenantId, dbnString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_withTenantId), dbnString);
    }

    {  // Has prefix, has tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: tenantId_database.coll, $tenant: tenantId, expectPrefix: false }
        auto dbName = DatabaseName(tenantId, dbnPrefixString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_withTenantId),
                  dbnPrefixString);
    }
}

// Serializing with SerializationContext, with an expectPrefix set to true
TEST(DatabaseNameUtilTest, SerializeExpectPrefixTrue_CommandReply) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    const std::string dbnString = "foo";
    const std::string dbnPrefixString = str::stream() << tenantId.toString() << "_" << dbnString;

    SerializationContext ctxt_noTenantId(SerializationContext::stateCommandReply());
    ctxt_noTenantId.setTenantIdSource(false);
    ctxt_noTenantId.setPrefixState(true);
    SerializationContext ctxt_withTenantId(SerializationContext::stateCommandReply());
    ctxt_withTenantId.setTenantIdSource(true);
    ctxt_withTenantId.setPrefixState(true);

    // TODO SERVER-74284: call the serialize/deserialize entry points instead

    {  // No prefix, no tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: database.coll, expectPrefix: true }
        auto dbName = DatabaseName(boost::none, dbnString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_noTenantId), dbnString);
    }

    {  // Has prefix, no tenantId.
        // request --> { ns: tenantId_database.coll, expectPrefix: true }
        auto dbName = DatabaseName(boost::none, dbnPrefixString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_noTenantId), dbnPrefixString);
    }

    {  // No prefix, has tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: database.coll, $tenant: tenantId, expectPrefix: true }
        auto dbName = DatabaseName(tenantId, dbnString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_noTenantId), dbnPrefixString);
    }

    {  // Has prefix, has tenantId.
        // request -->  { ns: tenantId_database.coll, $tenant: tenantId, expectPrefix: true }
        const std::string nsDoublePrefixString = str::stream()
            << tenantId.toString() << "_" << tenantId.toString() << "_" << dbnString;
        auto dbName = DatabaseName(tenantId, dbnPrefixString);
        ASSERT_EQ(DatabaseNameUtil::serializeForCommands(dbName, ctxt_noTenantId),
                  nsDoublePrefixString);
    }
}

TEST(DatabaseNameUtilTest, DeserializeMissingExpectPrefix_CommandRequest) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    const std::string dbnString = "foo";
    const std::string dbnPrefixString = str::stream() << tenantId.toString() << "_" << dbnString;

    SerializationContext ctxt_noTenantId(SerializationContext::stateCommandRequest());
    ctxt_noTenantId.setTenantIdSource(false);
    SerializationContext ctxt_withTenantId(SerializationContext::stateCommandRequest());
    ctxt_withTenantId.setTenantIdSource(true);

    // TODO SERVER-74284: call the serialize/deserialize entry points instead

    {  // No prefix, no tenantId.  *** we shouldn't see this from Atlas Proxy in MT mode
        // request --> { ns: database.coll }
        ASSERT_THROWS_CODE(
            DatabaseNameUtil::deserializeForCommands(boost::none, dbnString, ctxt_noTenantId),
            AssertionException,
            8423388);
    }

    {  // Has prefix, no tenantId.
        // request --> { ns: tenantId_database.coll }
        auto dbName =
            DatabaseNameUtil::deserializeForCommands(boost::none, dbnPrefixString, ctxt_noTenantId);
        ASSERT_EQ(dbName.tenantId(), tenantId);
        ASSERT_EQ(dbName.toString(), dbnString);
    }

    {  // No prefix, has tenantId.
        // request --> { ns: database.coll, $tenant: tenantId }
        auto dbName =
            DatabaseNameUtil::deserializeForCommands(tenantId, dbnString, ctxt_withTenantId);
        ASSERT_EQ(dbName.tenantId(), tenantId);
        ASSERT_EQ(dbName.toString(), dbnString);
    }

    {  // Has prefix, has tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: tenantId_database.coll, $tenant: tenantId }
        auto dbName =
            DatabaseNameUtil::deserializeForCommands(tenantId, dbnPrefixString, ctxt_withTenantId);
        ASSERT_EQ(dbName.tenantId(), tenantId);
        ASSERT_EQ(dbName.toString(), dbnPrefixString);
    }
}

TEST(DatabaseNameUtilTest, DeserializeExpectPrefixFalse_CommandRequest) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    const std::string dbnString = "foo";
    const std::string dbnPrefixString = str::stream() << tenantId.toString() << "_" << dbnString;

    SerializationContext ctxt_noTenantId(SerializationContext::stateCommandRequest());
    ctxt_noTenantId.setTenantIdSource(false);
    ctxt_noTenantId.setPrefixState(false);
    SerializationContext ctxt_withTenantId(SerializationContext::stateCommandRequest());
    ctxt_withTenantId.setTenantIdSource(true);
    ctxt_withTenantId.setPrefixState(false);

    // TODO SERVER-74284: call the serialize/deserialize entry points instead

    {  // No prefix, no tenantId.
        // request --> { ns: database.coll, expectPrefix: false }
        ASSERT_THROWS_CODE(
            DatabaseNameUtil::deserializeForCommands(boost::none, dbnString, ctxt_noTenantId),
            AssertionException,
            8423388);
    }

    {  // Has prefix, no tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: tenantId_database.coll, expectPrefix: false }
        auto dbName =
            DatabaseNameUtil::deserializeForCommands(boost::none, dbnPrefixString, ctxt_noTenantId);
        // This is an anomaly, when no tenantId is supplied, we actually ignore expectPrefix, so we
        // can't expect dbName.toString == dbnPrefixString as we will still attempt to parse the
        // prefix as usual.
        ASSERT_EQ(dbName.tenantId(), tenantId);
        ASSERT_EQ(dbName.toString(), dbnString);
    }

    {  // No prefix, has tenantId.
        // request --> { ns: database.coll, $tenant: tenantId, expectPrefix: false }
        auto dbName =
            DatabaseNameUtil::deserializeForCommands(tenantId, dbnString, ctxt_withTenantId);
        ASSERT_EQ(dbName.tenantId(), tenantId);
        ASSERT_EQ(dbName.toString(), dbnString);
    }

    {  // Has prefix, has tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: tenantId_database.coll, $tenant: tenantId, expectPrefix: false }
        auto dbName =
            DatabaseNameUtil::deserializeForCommands(tenantId, dbnPrefixString, ctxt_withTenantId);
        ASSERT_EQ(dbName.tenantId(), tenantId);
        ASSERT_EQ(dbName.toString(), dbnPrefixString);
    }
}

TEST(DatabaseNameUtilTest, DeserializeExpectPrefixTrue_CommandRequest) {
    RAIIServerParameterControllerForTest multitenanyController("multitenancySupport", true);
    TenantId tenantId(OID::gen());
    const std::string dbnString = "foo";
    const std::string dbnPrefixString = str::stream() << tenantId.toString() << "_" << dbnString;

    SerializationContext ctxt_noTenantId(SerializationContext::stateCommandRequest());
    ctxt_noTenantId.setTenantIdSource(false);
    ctxt_noTenantId.setPrefixState(true);
    SerializationContext ctxt_withTenantId(SerializationContext::stateCommandRequest());
    ctxt_withTenantId.setTenantIdSource(true);
    ctxt_withTenantId.setPrefixState(true);

    // TODO SERVER-74284: call the serialize/deserialize entry points in this test instead

    {  // No prefix, no tenantId.  *** we shouldn't see this from Atlas Proxy in MT mode
        // request --> { ns: database.coll, expectPrefix: true }
        ASSERT_THROWS_CODE(
            DatabaseNameUtil::deserializeForCommands(boost::none, dbnString, ctxt_noTenantId),
            AssertionException,
            8423388);
    }

    {  // Has prefix, no tenantId.
        // request --> { ns: tenantId_database.coll, expectPrefix: true }
        auto dbName =
            DatabaseNameUtil::deserializeForCommands(boost::none, dbnPrefixString, ctxt_noTenantId);
        ASSERT_EQ(dbName.tenantId(), tenantId);
        ASSERT_EQ(dbName.toString(), dbnString);
    }

    {  // No prefix, has tenantId.  *** we shouldn't see this from Atlas Proxy
        // request --> { ns: database.coll, $tenant: tenantId, expectPrefix: true }
        ASSERT_THROWS_CODE(
            DatabaseNameUtil::deserializeForCommands(tenantId, dbnString, ctxt_withTenantId),
            AssertionException,
            8423386);
    }

    {  // Has prefix, has tenantId.
        // request -->  { ns: tenantId_database.coll, $tenant: tenantId, expectPrefix: true }
        auto dbName =
            DatabaseNameUtil::deserializeForCommands(tenantId, dbnPrefixString, ctxt_withTenantId);
        ASSERT_EQ(dbName.tenantId(), tenantId);
        ASSERT_EQ(dbName.toString(), dbnString);
    }
}

}  // namespace mongo
