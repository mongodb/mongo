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

}  // namespace
}  // namespace mongo
