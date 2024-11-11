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

#include <boost/none.hpp>
#include <fmt/format.h>
#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/sharding_state.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

class DatabaseShardingStateTestWithMockedLoader
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
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        ShardServerTestFixtureWithCatalogCacheLoaderMock::tearDown();
    }

    class StaticCatalogClient final : public ShardingCatalogClientMock {
    public:
        StaticCatalogClient(std::vector<ShardType> shards) : _shards(std::move(shards)) {}

        StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
            OperationContext* opCtx,
            repl::ReadConcernLevel readConcern,
            bool excludeDraining) override {
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

TEST_F(DatabaseShardingStateTestWithMockedLoader, OnDbVersionMismatch) {
    const auto oldDb = createDatabase(UUID::gen(), Timestamp(1));
    const auto newDb = createDatabase(UUID::gen(), Timestamp(2));

    auto checkOnDbVersionMismatch = [&](const auto& newDb, bool expectRefresh) {
        const auto newDbVersion = newDb.getVersion();
        auto opCtx = operationContext();
        auto getActiveDbVersion = [&] {
            AutoGetDb autoDb(opCtx, kDbName, MODE_IS);
            const auto scopedDss =
                DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx, kDbName);
            return scopedDss->getDbVersion(opCtx);
        };

        getCatalogCacheLoaderMock()->setDatabaseRefreshReturnValue(newDb);
        ASSERT_OK(
            FilteringMetadataCache::get(opCtx)->onDbVersionMismatch(opCtx, kDbName, newDbVersion));

        auto activeDbVersion = getActiveDbVersion();
        ASSERT_TRUE(activeDbVersion);
        if (expectRefresh) {
            ASSERT_EQUALS(newDbVersion.getTimestamp(), activeDbVersion->getTimestamp());
        }
    };

    checkOnDbVersionMismatch(oldDb, true);
    checkOnDbVersionMismatch(newDb, true);
    checkOnDbVersionMismatch(oldDb, false);
}

TEST_F(DatabaseShardingStateTestWithMockedLoader, ForceDatabaseRefresh) {
    const auto uuid = UUID::gen();

    const auto oldDb = createDatabase(uuid, Timestamp(1));
    const auto newDb = createDatabase(uuid, Timestamp(2));

    auto checkForceDatabaseRefresh = [&](const auto& newDb, bool expectRefresh) {
        const auto newDbVersion = newDb.getVersion();
        auto opCtx = operationContext();

        getCatalogCacheLoaderMock()->setDatabaseRefreshReturnValue(newDb);
        ASSERT_OK(
            FilteringMetadataCache::get(opCtx)->onDbVersionMismatch(opCtx, kDbName, boost::none));

        boost::optional<DatabaseVersion> activeDbVersion = [&] {
            AutoGetDb autoDb(opCtx, kDbName, MODE_IS);
            const auto scopedDss =
                DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx, kDbName);
            return scopedDss->getDbVersion(opCtx);
        }();
        ASSERT_TRUE(activeDbVersion);
        if (expectRefresh) {
            ASSERT_EQ(newDbVersion.getTimestamp(), activeDbVersion->getTimestamp());
        }
    };

    checkForceDatabaseRefresh(oldDb, true);
    checkForceDatabaseRefresh(newDb, true);
    checkForceDatabaseRefresh(oldDb, false);
}

TEST_F(DatabaseShardingStateTestWithMockedLoader, CheckReceivedDatabaseVersion) {
    const auto installedDbVersion = DatabaseVersion(UUID::gen(), Timestamp(10, 0));

    // Install DSS
    {
        const auto dbInfoToInstall =
            DatabaseType(kDbName, kShardList[0].getName(), installedDbVersion);

        AutoGetDb autoDb(operationContext(), kDbName, MODE_IX);
        const auto dss =
            DatabaseShardingState::assertDbLockedAndAcquireExclusive(operationContext(), kDbName);
        dss->setDbInfo(operationContext(), dbInfoToInstall);
    }

    const auto dss = DatabaseShardingState::acquireShared(operationContext(), kDbName);

    // If received version matches, then success.
    ASSERT_DOES_NOT_THROW(dss->assertMatchingDbVersion(operationContext(), installedDbVersion));

    // If received version timestamp does not match, then throw.
    {
        auto versionWithOlderTimestamp = installedDbVersion;
        versionWithOlderTimestamp.setTimestamp({9, 0});
        ASSERT_THROWS_CODE(
            dss->assertMatchingDbVersion(operationContext(), versionWithOlderTimestamp),
            AssertionException,
            ErrorCodes::StaleDbVersion);

        auto versionWithNewerTimestamp = installedDbVersion;
        versionWithNewerTimestamp.setTimestamp({10, 1});
        ASSERT_THROWS_CODE(
            dss->assertMatchingDbVersion(operationContext(), versionWithNewerTimestamp),
            AssertionException,
            ErrorCodes::StaleDbVersion);
    }

    // If received version lastMod does not match, then throw.
    {
        auto versionWithOlderLastMod = installedDbVersion;
        versionWithOlderLastMod.setLastMod(installedDbVersion.getLastMod() + 1);
        ASSERT_THROWS_CODE(
            dss->assertMatchingDbVersion(operationContext(), versionWithOlderLastMod),
            AssertionException,
            ErrorCodes::StaleDbVersion);

        auto versionWithNewerLastMod = installedDbVersion;
        versionWithNewerLastMod.setLastMod(installedDbVersion.getLastMod() - 1);
        ASSERT_THROWS_CODE(
            dss->assertMatchingDbVersion(operationContext(), versionWithNewerLastMod),
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
        ASSERT_DOES_NOT_THROW(dss->assertMatchingDbVersion(operationContext(), installedDbVersion));

        // Command atClusterTime is older than db timestamp.
        cmdLevelReadConcern.setArgsAtClusterTimeForSnapshot(Timestamp(8, 0));
        repl::ReadConcernArgs::get(operationContext()) = cmdLevelReadConcern;
        ASSERT_THROWS_CODE(dss->assertMatchingDbVersion(operationContext(), installedDbVersion),
                           AssertionException,
                           ErrorCodes::MigrationConflict);

        // StaleDbVersion has precedence over MigrationConflict
        auto staleReceivedVersion = installedDbVersion;
        staleReceivedVersion.setTimestamp({9, 0});
        ASSERT_THROWS_CODE(dss->assertMatchingDbVersion(operationContext(), staleReceivedVersion),
                           AssertionException,
                           ErrorCodes::StaleDbVersion);

        // If received version has 'placementConflictTime' == Timestamp(0, 0), then ignore conflict.
        auto receivedVersionWithPlacementConflictTimeZero = installedDbVersion;
        receivedVersionWithPlacementConflictTimeZero.setPlacementConflictTime(
            LogicalTime(Timestamp{0, 0}));
        ASSERT_DOES_NOT_THROW(dss->assertMatchingDbVersion(
            operationContext(), receivedVersionWithPlacementConflictTimeZero));

        repl::ReadConcernArgs::get(operationContext()) = previousReadConcern;
    }

    // If installed database timestamp is greater than received 'placementConflictTime', then throw
    // MigrationConflict. (Except if 'placementConflictTime' is Timestamp(0, 0)).
    {
        auto receivedVersionWithGreaterPlacementConflictTime = installedDbVersion;
        receivedVersionWithGreaterPlacementConflictTime.setPlacementConflictTime(
            LogicalTime(Timestamp{11, 0}));
        ASSERT_DOES_NOT_THROW(dss->assertMatchingDbVersion(
            operationContext(), receivedVersionWithGreaterPlacementConflictTime));

        auto receivedVersionWithLowerPlacementConflictTime = installedDbVersion;
        receivedVersionWithLowerPlacementConflictTime.setPlacementConflictTime(
            LogicalTime(Timestamp{8, 0}));
        ASSERT_THROWS_CODE(dss->assertMatchingDbVersion(
                               operationContext(), receivedVersionWithLowerPlacementConflictTime),
                           AssertionException,
                           ErrorCodes::MigrationConflict);

        auto receivedVersionWithZeroPlacementConflictTime = installedDbVersion;
        receivedVersionWithZeroPlacementConflictTime.setPlacementConflictTime(
            LogicalTime(Timestamp{0, 0}));
        ASSERT_DOES_NOT_THROW(dss->assertMatchingDbVersion(
            operationContext(), receivedVersionWithZeroPlacementConflictTime));
    }
}

TEST_F(DatabaseShardingStateTestWithMockedLoader,
       CheckReceivedDatabaseVersionWhenCriticalSectionActive) {
    // If critical section is active, then throw.
    AutoGetDb autoDb(operationContext(), kDbName, MODE_X);
    const auto dss =
        DatabaseShardingState::assertDbLockedAndAcquireExclusive(operationContext(), kDbName);
    dss->enterCriticalSectionCatchUpPhase(operationContext(), BSONObj());
    dss->enterCriticalSectionCommitPhase(operationContext(), BSONObj());

    ASSERT_THROWS_CODE(dss->assertMatchingDbVersion(operationContext(),
                                                    DatabaseVersion(UUID::gen(), Timestamp(10, 0))),
                       AssertionException,
                       ErrorCodes::StaleDbVersion);

    ASSERT_DOES_NOT_THROW(dss->exitCriticalSection(operationContext(), BSONObj()));
}

}  // namespace
}  // namespace mongo
