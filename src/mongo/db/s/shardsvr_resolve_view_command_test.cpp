/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/shardsvr_resolve_view_command_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <boost/none.hpp>


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
