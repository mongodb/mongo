/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using unittest::assertGet;

using CreateDatabaseTest = ConfigServerTestFixture;

TEST_F(CreateDatabaseTest, createDatabaseSuccessWithoutCustomPrimary) {
    const std::string dbname = "db1";

    const std::vector<ShardType> shards{{"shard0000", "ShardHost0:27017"},
                                        {"shard0001", "ShardHost1:27017"},
                                        {"shard0002", "ShardHost2:27017"}};
    setupShards(shards);

    for (const auto& shard : shards) {
        targeterFactory()->addTargeterToReturn(ConnectionString(HostAndPort{shard.getHost()}), [&] {
            auto targeter = std::make_unique<RemoteCommandTargeterMock>();
            targeter->setFindHostReturnValue(HostAndPort{shard.getHost()});
            return targeter;
        }());
    }

    // Prime the shard registry with information about the existing shards
    shardRegistry()->reload(operationContext());

    auto future = launchAsync([this, dbname] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = tc->makeOperationContext();
        ShardingCatalogManager::get(opCtx.get())->createDatabase(opCtx.get(), dbname, ShardId());
    });

    // Return size information about first shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(shards[0].getHost(), request.target.toString());
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
        ASSERT_EQUALS(shards[1].getHost(), request.target.toString());
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
        ASSERT_EQUALS(shards[2].getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        std::string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);

        ASSERT_BSONOBJ_EQ(
            ReadPreferenceSetting(ReadPreference::PrimaryPreferred).toContainingBSON(),
            rpc::TrackingMetadata::removeTrackingData(request.metadata));

        return BSON("ok" << 1 << "totalSize" << 100);
    });

    // Return OK for _flushDatabaseCacheUpdates
    onCommand([&](const RemoteCommandRequest& request) {
        std::string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("_flushDatabaseCacheUpdates", cmdName);

        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(CreateDatabaseTest, createDatabaseSuccessWithCustomPrimary) {
    const ShardId primaryShardName("shard0002");
    const std::string dbname = "dbWithCustomPrimary1";

    const std::vector<ShardType> shards{{"shard0000", "ShardHost0:27017"},
                                        {"shard0001", "ShardHost1:27017"},
                                        {"shard0002", "ShardHost2:27017"}};
    setupShards(shards);

    for (const auto& shard : shards) {
        targeterFactory()->addTargeterToReturn(ConnectionString(HostAndPort{shard.getHost()}), [&] {
            auto targeter = std::make_unique<RemoteCommandTargeterMock>();
            targeter->setFindHostReturnValue(HostAndPort{shard.getHost()});
            return targeter;
        }());
    }

    // Prime the shard registry with information about the existing shards
    shardRegistry()->reload(operationContext());

    auto future = launchAsync([this, dbname, primaryShardName] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = tc->makeOperationContext();
        ShardingCatalogManager::get(opCtx.get())
            ->createDatabase(opCtx.get(), dbname, primaryShardName);
    });

    // Return OK for _flushDatabaseCacheUpdates
    onCommand([&](const RemoteCommandRequest& request) {
        std::string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("_flushDatabaseCacheUpdates", cmdName);

        return BSON("ok" << 1);
    });

    future.default_timed_get();

    auto databaseDoc = assertGet(findOneOnConfigCollection(
        operationContext(), DatabaseType::ConfigNS, BSON("_id" << dbname)));

    DatabaseType foundDatabase = assertGet(DatabaseType::fromBSON(databaseDoc));

    ASSERT_EQUALS(primaryShardName, foundDatabase.getPrimary());
}

TEST_F(CreateDatabaseTest,
       createDatabaseShardReturnsNamespaceNotFoundForFlushDatabaseCacheUpdates) {
    const std::string dbname = "db1";

    const std::vector<ShardType> shards{{"shard0000", "ShardHost0:27017"},
                                        {"shard0001", "ShardHost1:27017"},
                                        {"shard0002", "ShardHost2:27017"}};
    setupShards(shards);

    for (const auto& shard : shards) {
        targeterFactory()->addTargeterToReturn(ConnectionString(HostAndPort{shard.getHost()}), [&] {
            auto targeter = std::make_unique<RemoteCommandTargeterMock>();
            targeter->setFindHostReturnValue(HostAndPort{shard.getHost()});
            return targeter;
        }());
    }

    // Prime the shard registry with information about the existing shards
    shardRegistry()->reload(operationContext());

    auto future = launchAsync([this, dbname] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = tc->makeOperationContext();
        ShardingCatalogManager::get(opCtx.get())->createDatabase(opCtx.get(), dbname, ShardId());
    });

    // Return size information about first shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(shards[0].getHost(), request.target.toString());
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
        ASSERT_EQUALS(shards[1].getHost(), request.target.toString());
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
        ASSERT_EQUALS(shards[2].getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        std::string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);

        ASSERT_BSONOBJ_EQ(
            ReadPreferenceSetting(ReadPreference::PrimaryPreferred).toContainingBSON(),
            rpc::TrackingMetadata::removeTrackingData(request.metadata));

        return BSON("ok" << 1 << "totalSize" << 100);
    });

    // Return NamespaceNotFound for _flushDatabaseCacheUpdates
    onCommand([&](const RemoteCommandRequest& request) {
        std::string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("_flushDatabaseCacheUpdates", cmdName);

        return BSON("ok" << 0 << "code" << ErrorCodes::NamespaceNotFound << "errmsg"
                         << "dummy");
    });

    future.default_timed_get();
}

TEST_F(CreateDatabaseTest, createDatabaseDBExists) {
    const std::string dbname = "db3";
    const ShardType shard{"shard0", "shard0:12345"};
    setupShards({shard});
    setupDatabase(dbname, shard.getName(), false);

    targeterFactory()->addTargeterToReturn(ConnectionString(HostAndPort{shard.getHost()}), [&] {
        auto targeter = std::make_unique<RemoteCommandTargeterMock>();
        targeter->setFindHostReturnValue(HostAndPort{shard.getHost()});
        return targeter;
    }());

    // Prime the shard registry with information about the existing shard
    shardRegistry()->reload(operationContext());

    auto future = launchAsync([this, dbname] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = tc->makeOperationContext();
        ShardingCatalogManager::get(opCtx.get())->createDatabase(opCtx.get(), dbname, ShardId());
    });

    // Return OK for _flushDatabaseCacheUpdates
    onCommand([&](const RemoteCommandRequest& request) {
        std::string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("_flushDatabaseCacheUpdates", cmdName);

        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

TEST_F(CreateDatabaseTest, createDatabaseDBExistsDifferentCase) {
    const std::string dbname = "db4";

    setupShards({{"shard0", "shard0:12345"}});
    setupDatabase("DB4", ShardId("shard0"), false);

    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->createDatabase(operationContext(), dbname, ShardId()),
                       AssertionException,
                       ErrorCodes::DatabaseDifferCase);
}

TEST_F(CreateDatabaseTest, createDatabaseNoShards) {
    const std::string dbname = "db5";
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->createDatabase(operationContext(), dbname, ShardId()),
                       AssertionException,
                       ErrorCodes::ShardNotFound);
}

}  // namespace
}  // namespace mongo
