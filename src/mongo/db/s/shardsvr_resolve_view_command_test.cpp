// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/shardsvr_resolve_view_command_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_mock.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

class ShardsvrResolveViewCommandTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
    }

    // Database and version information.
    const DatabaseName dbNameTestDb = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const DatabaseVersion dbVersionTestDb{UUID::gen(), Timestamp(1, 0)};
    const DatabaseVersion wrongDbVersion{UUID::gen(), Timestamp(2, 0)};
    const ShardVersion shardVersion{};

    const NamespaceString nssView =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "view");
};

/**
 * Tests that ShardsvrResolveView fails when an incorrect database version is specified.
 */
TEST_F(ShardsvrResolveViewCommandTest, InvalidCommandWithWrongDBVersion) {
    OperationContext* opCtx = operationContext();

    {
        auto scopedDss = DatabaseShardingStateMock::acquire(opCtx, dbNameTestDb);
        scopedDss->expectFailureDbVersionCheckWithMismatchingVersion(dbVersionTestDb,
                                                                     wrongDbVersion);
    }

    ShardsvrResolveView cmd(nssView);

    DBDirectClient client(opCtx);
    BSONObj response;

    OperationShardingState::setShardRole(opCtx, nssView, shardVersion, wrongDbVersion);
    ASSERT_FALSE(client.runCommand(nssView.dbName(), cmd.toBSON(), response));
    ASSERT_EQ(response["code"].Int(), ErrorCodes::StaleDbVersion);

    auto scopedDss = DatabaseShardingStateMock::acquire(opCtx, dbNameTestDb);
    scopedDss->clearExpectedFailureDbVersionCheck();
}

/**
 * Tests that ShardsvrResolveView succeeds when no database version is specified. This results in
 * getDBVersion() returning null and the command will execute without an error (as expected).
 */
TEST_F(ShardsvrResolveViewCommandTest, ValidCommandWithNoDBVersion) {
    OperationContext* opCtx = operationContext();

    ShardsvrResolveView cmd(nssView);

    DBDirectClient client(opCtx);
    BSONObj response;

    ASSERT_TRUE(client.runCommand(nssView.dbName(), cmd.toBSON(), response));
    ASSERT_EQ(response["resolvedView"].Obj()["ns"].str(), nssView.toString_forTest());
}

/**
 * Tests that ShardsvrResolveView succeeds when a matching database version is specified.
 */
TEST_F(ShardsvrResolveViewCommandTest, ValidCommandWithMatchingDBVersion) {
    OperationContext* opCtx = operationContext();

    ShardsvrResolveView cmd(nssView);

    DBDirectClient client(opCtx);
    BSONObj response;

    // Matching DBVersion to what the database was initialized with.
    OperationShardingState::setShardRole(opCtx, nssView, shardVersion, dbVersionTestDb);
    ASSERT_TRUE(client.runCommand(nssView.dbName(), cmd.toBSON(), response));
    ASSERT_EQ(response["resolvedView"].Obj()["ns"].str(), nssView.toString_forTest());
}

/**
 * Tests that ShardsvrResolveView reports, via the isTimeseries reply field, when the namespace
 * resolves to a timeseries collection. The router uses this to reject search index commands
 * on timeseries.
 */
TEST_F(ShardsvrResolveViewCommandTest, ViewOverTimeseriesReportsIsTimeseriesTrue) {
    OperationContext* opCtx = operationContext();

    const NamespaceString nssTsColl =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "ts");
    const NamespaceString nssTsView =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "tsview");

    DBDirectClient client(opCtx);
    BSONObj response;
    ASSERT_TRUE(client.runCommand(
        dbNameTestDb,
        BSON("create" << nssTsColl.coll() << "timeseries" << BSON("timeField" << "t")),
        response));
    ASSERT_TRUE(client.runCommand(dbNameTestDb,
                                  BSON("create" << nssTsView.coll() << "viewOn" << nssTsColl.coll()
                                                << "pipeline" << BSONArray()),
                                  response));

    // Resolving a view onto a timeseries collection succeeds, resolves to the timeseries
    // collection namespace, and reports that the resolved namespace is a timeseries
    // collection.
    ShardsvrResolveView cmd(nssTsView);
    ASSERT_TRUE(client.runCommand(nssTsView.dbName(), cmd.toBSON(), response));
    ASSERT_EQ(response["resolvedView"].Obj()["ns"].str(), nssTsColl.toString_forTest());
    ASSERT(response.hasField("isTimeseries"));
    ASSERT_TRUE(response["isTimeseries"].trueValue());

    // The timeseries collection namespace itself is also reported as a timeseries collection.
    ShardsvrResolveView cmdTsColl(nssTsColl);
    ASSERT_TRUE(client.runCommand(nssTsColl.dbName(), cmdTsColl.toBSON(), response));
    ASSERT(response.hasField("isTimeseries"));
    ASSERT_TRUE(response["isTimeseries"].trueValue());
}

/**
 * Tests that ShardsvrResolveView reports isTimeseries false for a view that resolves to a
 * normal (non-timeseries) collection.
 */
TEST_F(ShardsvrResolveViewCommandTest, ViewOverNormalCollectionReportsIsTimeseriesFalse) {
    OperationContext* opCtx = operationContext();

    const NamespaceString nssNormalColl =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "normal");
    const NamespaceString nssNormalView =
        NamespaceString::createNamespaceString_forTest(dbNameTestDb, "normalview");

    DBDirectClient client(opCtx);
    BSONObj response;
    ASSERT_TRUE(client.runCommand(dbNameTestDb, BSON("create" << nssNormalColl.coll()), response));
    ASSERT_TRUE(
        client.runCommand(dbNameTestDb,
                          BSON("create" << nssNormalView.coll() << "viewOn" << nssNormalColl.coll()
                                        << "pipeline" << BSONArray()),
                          response));

    ShardsvrResolveView cmd(nssNormalView);
    ASSERT_TRUE(client.runCommand(nssNormalView.dbName(), cmd.toBSON(), response));
    ASSERT_EQ(response["resolvedView"].Obj()["ns"].str(), nssNormalColl.toString_forTest());
    ASSERT(response.hasField("isTimeseries"));
    ASSERT_FALSE(response["isTimeseries"].trueValue());

    ShardsvrResolveView cmdColl(nssNormalColl);
    ASSERT_TRUE(client.runCommand(nssNormalColl.dbName(), cmdColl.toBSON(), response));
    ASSERT(response.hasField("isTimeseries"));
    ASSERT_FALSE(response["isTimeseries"].trueValue());
}

}  // namespace
}  // namespace mongo
