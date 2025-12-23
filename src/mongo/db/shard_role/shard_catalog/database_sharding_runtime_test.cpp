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

#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"

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
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state_factory_shard.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/unittest/death_test.h"
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

        shard_role_details::setRecoveryUnit(operationContext(),
                                            std::make_unique<RecoveryUnitMock>(),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

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

    class RecoveryUnitMock : public RecoveryUnitNoop {
        using ReadSource = RecoveryUnit::ReadSource;

    public:
        void setTimestampReadSource(ReadSource source,
                                    boost::optional<Timestamp> provided = boost::none) override {
            _source = source;
            _timestamp = provided;
        }
        ReadSource getTimestampReadSource() const override {
            return _source;
        };
        boost::optional<Timestamp> getPointInTimeReadTimestamp() override {
            return _timestamp;
        }

    private:
        ReadSource _source = ReadSource::kNoTimestamp;
        boost::optional<Timestamp> _timestamp;
    };

    /**
     * Runs the given callback function within a transaction with the given placementConflictTime.
     */
    template <typename Callable>
    void runWithinTxn(OperationContext* opCtx,
                      boost::optional<LogicalTime> placementConflictTime,
                      const std::vector<DatabaseName>& createdDatabases,
                      Callable&& func) {
        TxnNumber txnNumber{0};

        opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
        opCtx->setTxnNumber(txnNumber);
        opCtx->setInMultiDocumentTransaction();

        auto argsAtClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
        if (argsAtClusterTime.has_value()) {
            shard_role_details::getRecoveryUnit(operationContext())
                ->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                         argsAtClusterTime->asTimestamp());
        }

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);

        TransactionRuntimeContext transactionRuntimeContext;
        transactionRuntimeContext.setPlacementConflictTime(placementConflictTime);
        transactionRuntimeContext.setCreatedDatabases(createdDatabases);

        txnParticipant.beginOrContinue(opCtx,
                                       {txnNumber},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart,
                                       transactionRuntimeContext);

        txnParticipant.unstashTransactionResources(opCtx, "DummyCmd");
        func();
        txnParticipant.commitUnpreparedTransaction(opCtx);
        txnParticipant.stashTransactionResources(opCtx);

        opCtx->resetMultiDocumentTransactionState();
    }
};

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
            return scopedDsr->getDbVersion(opCtx);
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

        // If the database has been created within the current transaction, ignore conflict
        runWithinTxn(operationContext(), LogicalTime(Timestamp(8, 0)), {kDbName}, [&] {
            ASSERT_DOES_NOT_THROW(
                dsr->checkDbVersionOrThrow(operationContext(), installedDbVersion));
        });

        // When the feature flag 'AddTransactionRuntimeContextAsAGenericArgument' is disabled, if
        // received version has 'placementConflictTime' == Timestamp(0, 0), then ignore conflict.
        {
            RAIIServerParameterControllerForTest featureFlagController(
                "featureFlagAddTransactionRuntimeContextAsAGenericArgument", false);

            auto receivedVersionWithPlacementConflictTimeZero = installedDbVersion;
            receivedVersionWithPlacementConflictTimeZero.setPlacementConflictTime_DEPRECATED(
                LogicalTime(Timestamp{0, 0}));
            ASSERT_DOES_NOT_THROW(dsr->checkDbVersionOrThrow(
                operationContext(), receivedVersionWithPlacementConflictTimeZero));
        }

        repl::ReadConcernArgs::get(operationContext()) = previousReadConcern;
    }
}

TEST_F(DatabaseShardingRuntimeTestWithMockedLoader,
       CheckReceivedDatabaseVersionWithPlacementConflictTime) {
    OperationContext* opCtx = operationContext();

    const auto installedDbVersion = DatabaseVersion(UUID::gen(), Timestamp(10, 0));
    const auto placementConflictTimeToThrow = LogicalTime(Timestamp{8, 0});
    const auto placementConflictTimeToNOTThrow = LogicalTime({11, 0});

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

    // When the feature AddTransactionRuntimeContextAsAGenericArgument is disabled, if installed
    // database timestamp is greater than 'placementConflictTime' attached to the DbVersion, then
    // throw MigrationConflict. (Except if 'placementConflictTime' is Timestamp(0, 0)).
    {
        RAIIServerParameterControllerForTest featureFlagController(
            "featureFlagAddTransactionRuntimeContextAsAGenericArgument", false);

        auto receivedVersionWithGreaterPlacementConflictTime = installedDbVersion;
        receivedVersionWithGreaterPlacementConflictTime.setPlacementConflictTime_DEPRECATED(
            placementConflictTimeToNOTThrow);
        ASSERT_DOES_NOT_THROW(dsr->checkDbVersionOrThrow(
            operationContext(), receivedVersionWithGreaterPlacementConflictTime));

        auto receivedVersionWithLowerPlacementConflictTime = installedDbVersion;
        receivedVersionWithLowerPlacementConflictTime.setPlacementConflictTime_DEPRECATED(
            placementConflictTimeToThrow);
        ASSERT_THROWS_CODE(dsr->checkDbVersionOrThrow(
                               operationContext(), receivedVersionWithLowerPlacementConflictTime),
                           AssertionException,
                           ErrorCodes::MigrationConflict);

        auto receivedVersionWithZeroPlacementConflictTime = installedDbVersion;
        receivedVersionWithZeroPlacementConflictTime.setPlacementConflictTime_DEPRECATED(
            LogicalTime(Timestamp{0, 0}));
        ASSERT_DOES_NOT_THROW(dsr->checkDbVersionOrThrow(
            operationContext(), receivedVersionWithZeroPlacementConflictTime));
    }

    // When the feature AddTransactionRuntimeContextAsAGenericArgument is enabled, if the
    // installed database timestamp is greater than the 'placementConflictTime' returned by the
    // TransactionParticipant and the operation runs within a transaction, then throw
    // MigrationConflict. (Except if 'placementConflictTime' is Timestamp(0, 0)).
    {
        RAIIServerParameterControllerForTest featureFlagController(
            "featureFlagAddTransactionRuntimeContextAsAGenericArgument", true);

        // It should throw if the placementConflictTime is greater than the installed db timestamp
        runWithinTxn(opCtx, placementConflictTimeToNOTThrow, {}, [&] {
            ASSERT_DOES_NOT_THROW(
                dsr->checkDbVersionOrThrow(operationContext(), installedDbVersion));
        });

        // It should throw if the placementConflictTime is lower than the installed db timestamp
        runWithinTxn(opCtx, placementConflictTimeToThrow, {}, [&] {
            ASSERT_THROWS_CODE(dsr->checkDbVersionOrThrow(operationContext(), installedDbVersion),
                               AssertionException,
                               ErrorCodes::MigrationConflict);
        });

        // It should not throw if the database kDbName was created within the transaction.
        runWithinTxn(opCtx, placementConflictTimeToThrow, {kDbName}, [&] {
            ASSERT_DOES_NOT_THROW(
                dsr->checkDbVersionOrThrow(operationContext(), installedDbVersion));
        });

        // The placementConflictTime attached to the DatabaseVersion is ignored if the
        // featureFlagAddTransactionRuntimeContextAsAGenericArgument is enabled.
        {
            auto receivedVersionWithLowerPlacementConflictTime = installedDbVersion;
            receivedVersionWithLowerPlacementConflictTime.setPlacementConflictTime_DEPRECATED(
                placementConflictTimeToThrow);
            ASSERT_DOES_NOT_THROW(dsr->checkDbVersionOrThrow(
                operationContext(), receivedVersionWithLowerPlacementConflictTime));
        }
    }
}

using DatabaseShardingRuntimeTestWithMockedLoaderDeathTest =
    DatabaseShardingRuntimeTestWithMockedLoader;
DEATH_TEST_REGEX_F(DatabaseShardingRuntimeTestWithMockedLoaderDeathTest,
                   TestsShouldTassertIfPlacementConflictTimeIsNotPresentInTxns,
                   "Tripwire assertion.*9758701") {
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
    ASSERT_DOES_NOT_THROW(dsr->checkDbVersionOrThrow(operationContext(), installedDbVersion));

    runWithinTxn(operationContext(), boost::none, {}, [&]() {
        ScopedSetShardRole scopedSetShardRole{operationContext(),
                                              NamespaceString{kDbName},
                                              boost::none,
                                              installedDbVersion /* databaseVersion */};
        dsr->checkDbVersionOrThrow(operationContext(), installedDbVersion);
    });
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

class BypassDatabaseMetadataAccessTest : public ServiceContextMongoDTest {};

TEST_F(BypassDatabaseMetadataAccessTest, BypassRead) {
    auto opCtxPtr = makeOperationContext();
    auto opCtx = opCtxPtr.get();
    auto& opDbMetadata = OperationDatabaseMetadata::get(opCtx);

    ASSERT_FALSE(opDbMetadata.getBypassReadDbMetadataAccess());
    ASSERT_FALSE(opDbMetadata.getBypassWriteDbMetadataAccess());

    {
        BypassDatabaseMetadataAccess bypass(
            opCtx, BypassDatabaseMetadataAccess::Type::kReadOnly);  // NOLINT
        ASSERT_TRUE(opDbMetadata.getBypassReadDbMetadataAccess());
        ASSERT_FALSE(opDbMetadata.getBypassWriteDbMetadataAccess());
    }

    ASSERT_FALSE(opDbMetadata.getBypassReadDbMetadataAccess());
    ASSERT_FALSE(opDbMetadata.getBypassWriteDbMetadataAccess());
}

TEST_F(BypassDatabaseMetadataAccessTest, BypassWrite) {
    auto opCtxPtr = makeOperationContext();
    auto opCtx = opCtxPtr.get();
    auto& opDbMetadata = OperationDatabaseMetadata::get(opCtx);

    ASSERT_FALSE(opDbMetadata.getBypassReadDbMetadataAccess());
    ASSERT_FALSE(opDbMetadata.getBypassWriteDbMetadataAccess());

    {
        BypassDatabaseMetadataAccess bypass(
            opCtx, BypassDatabaseMetadataAccess::Type::kWriteOnly);  // NOLINT
        ASSERT_FALSE(opDbMetadata.getBypassReadDbMetadataAccess());
        ASSERT_TRUE(opDbMetadata.getBypassWriteDbMetadataAccess());
    }

    ASSERT_FALSE(opDbMetadata.getBypassReadDbMetadataAccess());
    ASSERT_FALSE(opDbMetadata.getBypassWriteDbMetadataAccess());
}

TEST_F(BypassDatabaseMetadataAccessTest, BypassReadAndWrite) {
    auto opCtxPtr = makeOperationContext();
    auto opCtx = opCtxPtr.get();
    auto& opDbMetadata = OperationDatabaseMetadata::get(opCtx);

    ASSERT_FALSE(opDbMetadata.getBypassReadDbMetadataAccess());
    ASSERT_FALSE(opDbMetadata.getBypassWriteDbMetadataAccess());

    {
        BypassDatabaseMetadataAccess bypass(
            opCtx, BypassDatabaseMetadataAccess::Type::kReadAndWrite);  // NOLINT
        ASSERT_TRUE(opDbMetadata.getBypassReadDbMetadataAccess());
        ASSERT_TRUE(opDbMetadata.getBypassWriteDbMetadataAccess());
    }

    ASSERT_FALSE(opDbMetadata.getBypassReadDbMetadataAccess());
    ASSERT_FALSE(opDbMetadata.getBypassWriteDbMetadataAccess());
}

}  // namespace
}  // namespace mongo
