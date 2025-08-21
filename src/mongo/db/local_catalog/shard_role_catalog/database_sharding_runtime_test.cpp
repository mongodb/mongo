/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_runtime.h"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state_factory_shard.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

class DatabaseShardingRuntimeTestWithMockedLoader
    : public ShardServerTestFixtureWithCatalogCacheLoaderMock {
public:
    const DatabaseName kDbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");

    const std::vector<ShardType> kShardList = {ShardType(kMyShardName.toString(), "Host0:12345")};

    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheLoaderMock::setUp();

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        for (const auto& shard : kShardList) {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            HostAndPort host(shard.getHost());
            targeter->setConnectionStringReturnValue(ConnectionString(host));
            targeter->setFindHostReturnValue(host);
            targeterFactory()->addTargeterToReturn(ConnectionString(host), std::move(targeter));
        }

        // Clear the previous instantiation of the DSSFactory to set up the DatabaseShardingRuntime.
        DatabaseShardingStateFactory::clear(getServiceContext());
        DatabaseShardingStateFactory::set(getServiceContext(),
                                          std::make_unique<DatabaseShardingStateFactoryShard>());
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        ShardServerTestFixtureWithCatalogCacheLoaderMock::tearDown();
    }

    class StaticCatalogClient final : public ShardingCatalogClientMock {
    public:
        StaticCatalogClient(std::vector<ShardType> shards) : _shards(std::move(shards)) {}

        repl::OpTimeWith<std::vector<ShardType>> getAllShards(OperationContext* opCtx,
                                                              repl::ReadConcernLevel readConcern,
                                                              BSONObj filter) override {
            return repl::OpTimeWith<std::vector<ShardType>>(_shards);
        }

        std::vector<CollectionType> getShardedCollections(OperationContext* opCtx,
                                                          const DatabaseName& dbName,
                                                          repl::ReadConcernLevel readConcernLevel,
                                                          const BSONObj& sort) override {
            return {};
        }

        std::vector<CollectionType> getCollections(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   repl::ReadConcernLevel readConcernLevel,
                                                   const BSONObj& sort) override {
            return _colls;
        }

        void setCollections(std::vector<CollectionType> colls) {
            _colls = std::move(colls);
        }

    private:
        const std::vector<ShardType> _shards;
        std::vector<CollectionType> _colls;
    };

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        return std::make_unique<StaticCatalogClient>(kShardList);
    }

    DatabaseType createDatabase(const UUID& uuid, const Timestamp& timestamp) {
        return DatabaseType(kDbName, kShardList[0].getName(), DatabaseVersion(uuid, timestamp));
    }
};

TEST_F(DatabaseShardingRuntimeTestWithMockedLoader, OnDbVersionMismatch) {
    const auto oldDb = createDatabase(UUID::gen(), Timestamp(1));
    const auto newDb = createDatabase(UUID::gen(), Timestamp(2));

    auto checkOnDbVersionMismatch = [&](const auto& newDb, bool expectRefresh) {
        const auto newDbVersion = newDb.getVersion();
        auto opCtx = operationContext();

        getCatalogCacheLoaderMock()->setDatabaseRefreshReturnValue(newDb);
        ASSERT_OK(
            FilteringMetadataCache::get(opCtx)->onDbVersionMismatch(opCtx, kDbName, newDbVersion));

        auto dbVersion = [&] {
            const auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, kDbName);
            return scopedDsr->getDbVersion();
        }();

        ASSERT_TRUE(dbVersion);
        if (expectRefresh) {
            ASSERT_EQUALS(newDbVersion.getTimestamp(), dbVersion->getTimestamp());
        }
    };

    checkOnDbVersionMismatch(oldDb, true);
    checkOnDbVersionMismatch(newDb, true);
    checkOnDbVersionMismatch(oldDb, false);
}

TEST_F(DatabaseShardingRuntimeTestWithMockedLoader, ForceDatabaseRefresh) {
    const auto uuid = UUID::gen();

    const auto oldDb = createDatabase(uuid, Timestamp(1));
    const auto newDb = createDatabase(uuid, Timestamp(2));

    auto checkForceDatabaseRefresh = [&](const auto& newDb, bool expectRefresh) {
        const auto newDbVersion = newDb.getVersion();
        auto opCtx = operationContext();

        getCatalogCacheLoaderMock()->setDatabaseRefreshReturnValue(newDb);
        ASSERT_OK(FilteringMetadataCache::get(opCtx)->forceDatabaseMetadataRefresh_DEPRECATED(
            opCtx, kDbName));

        auto dbVersion = [&] {
            const auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, kDbName);
            return scopedDsr->getDbVersion();
        }();

        ASSERT_TRUE(dbVersion);
        if (expectRefresh) {
            ASSERT_EQUALS(newDbVersion.getTimestamp(), dbVersion->getTimestamp());
        }
    };

    checkForceDatabaseRefresh(oldDb, true);
    checkForceDatabaseRefresh(newDb, true);
    checkForceDatabaseRefresh(oldDb, false);
}

TEST_F(DatabaseShardingRuntimeTestWithMockedLoader, CheckReceivedDatabaseVersion) {
    const auto installedDbVersion = DatabaseVersion(UUID::gen(), Timestamp(10, 0));

    // Install DSR
    {
        const auto dbInfoToInstall =
            DatabaseType(kDbName, kShardList[0].getName(), installedDbVersion);

        AutoGetDb autoDb(operationContext(), kDbName, MODE_IX);
        const auto dsr =
            DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(operationContext(), kDbName);
        dsr->setDbInfo_DEPRECATED(operationContext(), dbInfoToInstall);
    }

    const auto dsr = DatabaseShardingRuntime::acquireShared(operationContext(), kDbName);

    // If received version matches, then success.
    ASSERT_DOES_NOT_THROW(dsr->checkDbVersionOrThrow(operationContext(), installedDbVersion));

    // If received version timestamp does not match, then throw.
    {
        auto versionWithOlderTimestamp = installedDbVersion;
        versionWithOlderTimestamp.setTimestamp({9, 0});
        ASSERT_THROWS_CODE(
            dsr->checkDbVersionOrThrow(operationContext(), versionWithOlderTimestamp),
            AssertionException,
            ErrorCodes::StaleDbVersion);

        auto versionWithNewerTimestamp = installedDbVersion;
        versionWithNewerTimestamp.setTimestamp({10, 1});
        ASSERT_THROWS_CODE(
            dsr->checkDbVersionOrThrow(operationContext(), versionWithNewerTimestamp),
            AssertionException,
            ErrorCodes::StaleDbVersion);
    }

    // If received version lastMod does not match, then throw.
    {
        auto versionWithOlderLastMod = installedDbVersion;
        versionWithOlderLastMod.setLastMod(installedDbVersion.getLastMod() + 1);
        ASSERT_THROWS_CODE(dsr->checkDbVersionOrThrow(operationContext(), versionWithOlderLastMod),
                           AssertionException,
                           ErrorCodes::StaleDbVersion);

        auto versionWithNewerLastMod = installedDbVersion;
        versionWithNewerLastMod.setLastMod(installedDbVersion.getLastMod() - 1);
        ASSERT_THROWS_CODE(dsr->checkDbVersionOrThrow(operationContext(), versionWithNewerLastMod),
                           AssertionException,
                           ErrorCodes::StaleDbVersion);
    }

    // If installed database timestamp is greater than opCtx's atClusterTime, then throw
    // MigrationConflict. (Except if received 'placementConflictTime' is Timestamp(0, 0)).
    {
        const auto previousReadConcern = repl::ReadConcernArgs::get(operationContext());

        // Command atClusterTime is newer than db timestamp.
        repl::ReadConcernArgs cmdLevelReadConcern(repl::ReadConcernLevel::kSnapshotReadConcern);
        cmdLevelReadConcern.setArgsAtClusterTimeForSnapshot(Timestamp(11, 0));
        repl::ReadConcernArgs::get(operationContext()) = cmdLevelReadConcern;
        ASSERT_DOES_NOT_THROW(dsr->checkDbVersionOrThrow(operationContext(), installedDbVersion));

        // Command atClusterTime is older than db timestamp.
        cmdLevelReadConcern.setArgsAtClusterTimeForSnapshot(Timestamp(8, 0));
        repl::ReadConcernArgs::get(operationContext()) = cmdLevelReadConcern;
        ASSERT_THROWS_CODE(dsr->checkDbVersionOrThrow(operationContext(), installedDbVersion),
                           AssertionException,
                           ErrorCodes::MigrationConflict);

        // StaleDbVersion has precedence over MigrationConflict
        auto staleReceivedVersion = installedDbVersion;
        staleReceivedVersion.setTimestamp({9, 0});
        ASSERT_THROWS_CODE(dsr->checkDbVersionOrThrow(operationContext(), staleReceivedVersion),
                           AssertionException,
                           ErrorCodes::StaleDbVersion);

        // If received version has 'placementConflictTime' == Timestamp(0, 0), then ignore conflict.
        auto receivedVersionWithPlacementConflictTimeZero = installedDbVersion;
        receivedVersionWithPlacementConflictTimeZero.setPlacementConflictTime(
            LogicalTime(Timestamp{0, 0}));
        ASSERT_DOES_NOT_THROW(dsr->checkDbVersionOrThrow(
            operationContext(), receivedVersionWithPlacementConflictTimeZero));

        repl::ReadConcernArgs::get(operationContext()) = previousReadConcern;
    }

    // If installed database timestamp is greater than received 'placementConflictTime', then throw
    // MigrationConflict. (Except if 'placementConflictTime' is Timestamp(0, 0)).
    {
        auto receivedVersionWithGreaterPlacementConflictTime = installedDbVersion;
        receivedVersionWithGreaterPlacementConflictTime.setPlacementConflictTime(
            LogicalTime(Timestamp{11, 0}));
        ASSERT_DOES_NOT_THROW(dsr->checkDbVersionOrThrow(
            operationContext(), receivedVersionWithGreaterPlacementConflictTime));

        auto receivedVersionWithLowerPlacementConflictTime = installedDbVersion;
        receivedVersionWithLowerPlacementConflictTime.setPlacementConflictTime(
            LogicalTime(Timestamp{8, 0}));
        ASSERT_THROWS_CODE(dsr->checkDbVersionOrThrow(
                               operationContext(), receivedVersionWithLowerPlacementConflictTime),
                           AssertionException,
                           ErrorCodes::MigrationConflict);

        auto receivedVersionWithZeroPlacementConflictTime = installedDbVersion;
        receivedVersionWithZeroPlacementConflictTime.setPlacementConflictTime(
            LogicalTime(Timestamp{0, 0}));
        ASSERT_DOES_NOT_THROW(dsr->checkDbVersionOrThrow(
            operationContext(), receivedVersionWithZeroPlacementConflictTime));
    }
}

TEST_F(DatabaseShardingRuntimeTestWithMockedLoader,
       CheckReceivedDatabaseVersionWhenCriticalSectionActive) {
    // If critical section is active, then throw.
    AutoGetDb autoDb(operationContext(), kDbName, MODE_X);
    const auto dsr =
        DatabaseShardingRuntime::assertDbLockedAndAcquireExclusive(operationContext(), kDbName);
    dsr->enterCriticalSectionCatchUpPhase(BSONObj());
    dsr->enterCriticalSectionCommitPhase(BSONObj());

    ASSERT_THROWS_CODE(dsr->checkDbVersionOrThrow(operationContext(),
                                                  DatabaseVersion(UUID::gen(), Timestamp(10, 0))),
                       AssertionException,
                       ErrorCodes::StaleDbVersion);

    ASSERT_DOES_NOT_THROW(dsr->exitCriticalSection(BSONObj()));
}

}  // namespace
}  // namespace mongo
