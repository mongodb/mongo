/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <pcrecpp.h>

#include "mongo/bson/json.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using std::vector;

using CreateDatabaseTest = ConfigServerTestFixture;

TEST_F(CreateDatabaseTest, createDatabaseSuccess) {
    const std::string dbname = "db1";

    ShardType s0;
    s0.setName("shard0000");
    s0.setHost("ShardHost0:27017");
    ASSERT_OK(setupShards(vector<ShardType>{s0}));

    ShardType s1;
    s1.setName("shard0001");
    s1.setHost("ShardHost1:27017");
    ASSERT_OK(setupShards(vector<ShardType>{s1}));

    ShardType s2;
    s2.setName("shard0002");
    s2.setHost("ShardHost2:27017");
    ASSERT_OK(setupShards(vector<ShardType>{s2}));

    // Prime the shard registry with information about the existing shards
    shardRegistry()->reload(operationContext());

    // Set up all the target mocks return values.
    RemoteCommandTargeterMock::get(
        uassertStatusOK(shardRegistry()->getShard(operationContext(), s0.getName()))->getTargeter())
        ->setFindHostReturnValue(HostAndPort(s0.getHost()));
    RemoteCommandTargeterMock::get(
        uassertStatusOK(shardRegistry()->getShard(operationContext(), s1.getName()))->getTargeter())
        ->setFindHostReturnValue(HostAndPort(s1.getHost()));
    RemoteCommandTargeterMock::get(
        uassertStatusOK(shardRegistry()->getShard(operationContext(), s2.getName()))->getTargeter())
        ->setFindHostReturnValue(HostAndPort(s2.getHost()));

    // Now actually start the createDatabase work.

    auto future = launchAsync([this, dbname] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ShardingCatalogManager::get(opCtx.get())->createDatabase(opCtx.get(), dbname);
    });

    // Return size information about first shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(s0.getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        std::string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);
        ASSERT_FALSE(request.cmdObj.hasField(repl::ReadConcernArgs::kReadConcernFieldName));

        ASSERT_BSONOBJ_EQ(
            ReadPreferenceSetting(ReadPreference::PrimaryPreferred).toContainingBSON(),
            rpc::TrackingMetadata::removeTrackingData(request.metadata));

        return BSON("ok" << 1 << "totalSize" << 10);
    });

    // Return size information about second shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(s1.getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        std::string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);
        ASSERT_FALSE(request.cmdObj.hasField(repl::ReadConcernArgs::kReadConcernFieldName));

        ASSERT_BSONOBJ_EQ(
            ReadPreferenceSetting(ReadPreference::PrimaryPreferred).toContainingBSON(),
            rpc::TrackingMetadata::removeTrackingData(request.metadata));

        return BSON("ok" << 1 << "totalSize" << 1);
    });

    // Return size information about third shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(s2.getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        std::string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);

        ASSERT_BSONOBJ_EQ(
            ReadPreferenceSetting(ReadPreference::PrimaryPreferred).toContainingBSON(),
            rpc::TrackingMetadata::removeTrackingData(request.metadata));

        return BSON("ok" << 1 << "totalSize" << 100);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(CreateDatabaseTest, createDatabaseDBExists) {
    const std::string dbname = "db3";

    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");

    ASSERT_OK(setupShards(vector<ShardType>{shard}));

    setupDatabase(dbname, shard.getName(), false);

    ShardingCatalogManager::get(operationContext())->createDatabase(operationContext(), dbname);
}

TEST_F(CreateDatabaseTest, createDatabaseDBExistsDifferentCase) {
    const std::string dbname = "db4";
    const std::string dbnameDiffCase = "Db4";

    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");

    ASSERT_OK(setupShards(vector<ShardType>{shard}));

    setupDatabase(dbnameDiffCase, shard.getName(), false);

    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(operationContext())->createDatabase(operationContext(), dbname),
        AssertionException,
        ErrorCodes::DatabaseDifferCase);
}

TEST_F(CreateDatabaseTest, createDatabaseNoShards) {
    const std::string dbname = "db5";
    ASSERT_THROWS_CODE(
        ShardingCatalogManager::get(operationContext())->createDatabase(operationContext(), dbname),
        AssertionException,
        ErrorCodes::ShardNotFound);
}

}  // namespace
}  // namespace mongo
