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

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/s/balancer/migration_manager.h"
#include "mongo/db/s/balancer/migration_test_fixture.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/s/request_types/move_chunk_request.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using unittest::assertGet;

const MigrationSecondaryThrottleOptions kDefaultSecondaryThrottle =
    MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kDefault);

class MigrationManagerTest : public MigrationTestFixture {
protected:
    void setUp() override {
        MigrationTestFixture::setUp();
        _migrationManager = std::make_unique<MigrationManager>(getServiceContext());
        _migrationManager->startRecoveryAndAcquireDistLocks(operationContext());
        _migrationManager->finishRecovery(operationContext(), 0, kDefaultSecondaryThrottle);

        // Necessary because the migration manager may take a dist lock, which calls serverStatus
        // and will attempt to return the latest read write concern defaults.
        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());
    }

    void tearDown() override {
        checkMigrationsCollectionIsEmptyAndLocksAreUnlocked();
        _migrationManager->interruptAndDisableMigrations();
        _migrationManager->drainActiveMigrations();
        _migrationManager.reset();
        ConfigServerTestFixture::tearDown();
    }

    /**
     * Sets up mock network to expect a moveChunk command and returns a fixed BSON response or a
     * "returnStatus".
     */
    void expectMoveChunkCommand(const NamespaceString& nss,
                                const ChunkType& chunk,
                                const ShardId& toShardId,
                                const BSONObj& response) {
        onCommand([&nss, &chunk, &toShardId, &response](const RemoteCommandRequest& request) {
            NamespaceString nss(request.cmdObj.firstElement().valueStringData());

            const StatusWith<MoveChunkRequest> moveChunkRequestWithStatus =
                MoveChunkRequest::createFromCommand(nss, request.cmdObj);
            ASSERT_OK(moveChunkRequestWithStatus.getStatus());

            ASSERT_EQ(nss, moveChunkRequestWithStatus.getValue().getNss());
            ASSERT_BSONOBJ_EQ(chunk.getMin(), moveChunkRequestWithStatus.getValue().getMinKey());
            ASSERT_BSONOBJ_EQ(chunk.getMax(), moveChunkRequestWithStatus.getValue().getMaxKey());
            ASSERT_EQ(chunk.getShard(), moveChunkRequestWithStatus.getValue().getFromShardId());

            ASSERT_EQ(toShardId, moveChunkRequestWithStatus.getValue().getToShardId());

            return response;
        });
    }

    void expectMoveChunkCommand(const NamespaceString& nss,
                                const ChunkType& chunk,
                                const ShardId& toShardId,
                                const Status& returnStatus) {
        BSONObjBuilder resultBuilder;
        CommandHelpers::appendCommandStatusNoThrow(resultBuilder, returnStatus);
        expectMoveChunkCommand(nss, chunk, toShardId, resultBuilder.obj());
    }

    std::unique_ptr<MigrationManager> _migrationManager;
    ReadWriteConcernDefaultsLookupMock _lookupMock;
};

TEST_F(MigrationManagerTest, OneCollectionTwoMigrations) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    const auto collUUID = UUID::gen();
    ChunkVersion version(2, 0, OID::gen(), Timestamp(42));

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, collUUID, version);

    // Set up two chunks in the metadata.
    ChunkType chunk1 = setUpChunk(
        collName, collUUID, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version);
    version.incMinor();
    ChunkType chunk2 = setUpChunk(
        collName, collUUID, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version);

    // Going to request that these two chunks get migrated.
    const std::vector<MigrateInfo> migrationRequests{{kShardId1,
                                                      collName,
                                                      chunk1,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance},
                                                     {kShardId3,
                                                      collName,
                                                      chunk2,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance}};

    auto future = launchAsync([this, migrationRequests] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding a host to which to send the command.
        // Set up dummy hosts for the source shards.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(opCtx.get(), kShardId2)->setFindHostReturnValue(kShardHost2);

        MigrationStatuses migrationStatuses = _migrationManager->executeMigrationsForAutoBalance(
            opCtx.get(), migrationRequests, 0, kDefaultSecondaryThrottle, false);

        for (const auto& migrateInfo : migrationRequests) {
            ASSERT_OK(migrationStatuses.at(migrateInfo.getName()));
        }
    });

    // Expect two moveChunk commands.
    expectMoveChunkCommand(collName, chunk1, kShardId1, Status::OK());
    expectMoveChunkCommand(collName, chunk2, kShardId3, Status::OK());

    // Run the MigrationManager code.
    future.default_timed_get();
}

TEST_F(MigrationManagerTest, TwoCollectionsTwoMigrationsEach) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up a database and two collections as sharded in the metadata.
    std::string dbName = "foo";
    const NamespaceString collName1(dbName, "bar");
    const auto collUUID1 = UUID::gen();
    const NamespaceString collName2(dbName, "baz");
    const auto collUUID2 = UUID::gen();
    ChunkVersion version1(2, 0, OID::gen(), Timestamp(12));
    ChunkVersion version2(2, 0, OID::gen(), Timestamp(24));

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName1, collUUID1, version1);
    setUpCollection(collName2, collUUID2, version2);

    // Set up two chunks in the metadata for each collection.
    ChunkType chunk1coll1 = setUpChunk(
        collName1, collUUID1, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version1);
    version1.incMinor();
    ChunkType chunk2coll1 = setUpChunk(
        collName1, collUUID1, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version1);

    ChunkType chunk1coll2 = setUpChunk(
        collName2, collUUID2, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version2);
    version2.incMinor();
    ChunkType chunk2coll2 = setUpChunk(
        collName2, collUUID2, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version2);

    // Going to request that these four chunks get migrated.
    const std::vector<MigrateInfo> migrationRequests{{kShardId1,
                                                      collName1,
                                                      chunk1coll1,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance},
                                                     {kShardId3,
                                                      collName1,
                                                      chunk2coll1,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance},
                                                     {kShardId1,
                                                      collName2,
                                                      chunk1coll2,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance},
                                                     {kShardId3,
                                                      collName2,
                                                      chunk2coll2,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance}};

    auto future = launchAsync([this, migrationRequests] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding a host to which to send the command.
        // Set up dummy hosts for the source shards.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(opCtx.get(), kShardId2)->setFindHostReturnValue(kShardHost2);

        MigrationStatuses migrationStatuses = _migrationManager->executeMigrationsForAutoBalance(
            opCtx.get(), migrationRequests, 0, kDefaultSecondaryThrottle, false);

        for (const auto& migrateInfo : migrationRequests) {
            ASSERT_OK(migrationStatuses.at(migrateInfo.getName()));
        }
    });

    // Expect four moveChunk commands.
    expectMoveChunkCommand(collName1, chunk1coll1, kShardId1, Status::OK());
    expectMoveChunkCommand(collName1, chunk2coll1, kShardId3, Status::OK());
    expectMoveChunkCommand(collName2, chunk1coll2, kShardId1, Status::OK());
    expectMoveChunkCommand(collName2, chunk2coll2, kShardId3, Status::OK());

    // Run the MigrationManager code.
    future.default_timed_get();
}

// The MigrationManager should fail the migration if a host is not found for the source shard.
// Scheduling a moveChunk command requires finding a host to which to send the command.
TEST_F(MigrationManagerTest, SourceShardNotFound) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    const auto collUUID = UUID::gen();
    ChunkVersion version(2, 0, OID::gen(), Timestamp(42));

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, collUUID, version);

    // Set up two chunks in the metadata.
    ChunkType chunk1 = setUpChunk(
        collName, collUUID, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version);
    version.incMinor();
    ChunkType chunk2 = setUpChunk(
        collName, collUUID, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version);

    // Going to request that these two chunks get migrated.
    const std::vector<MigrateInfo> migrationRequests{{kShardId1,
                                                      collName,
                                                      chunk1,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance},
                                                     {kShardId3,
                                                      collName,
                                                      chunk2,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance}};

    auto future = launchAsync([this, chunk1, chunk2, migrationRequests] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = cc().makeOperationContext();

        // Scheduling a moveChunk command requires finding a host to which to send the command. Set
        // up a dummy host for kShardHost0, and return an error for kShardHost3.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(opCtx.get(), kShardId2)
            ->setFindHostReturnValue(
                Status(ErrorCodes::ReplicaSetNotFound, "SourceShardNotFound generated error."));

        MigrationStatuses migrationStatuses = _migrationManager->executeMigrationsForAutoBalance(
            opCtx.get(), migrationRequests, 0, kDefaultSecondaryThrottle, false);

        ASSERT_OK(migrationStatuses.at(migrationRequests.front().getName()));
        ASSERT_EQ(ErrorCodes::ReplicaSetNotFound,
                  migrationStatuses.at(migrationRequests.back().getName()));
    });

    // Expect only one moveChunk command to be called.
    expectMoveChunkCommand(collName, chunk1, kShardId1, Status::OK());

    // Run the MigrationManager code.
    future.default_timed_get();
}

// TODO: Delete in 3.8
TEST_F(MigrationManagerTest, JumboChunkResponseBackwardsCompatibility) {
    // Set up one shard in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    const auto collUUID = UUID::gen();
    ChunkVersion version(2, 0, OID::gen(), Timestamp(42));

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, collUUID, version);

    // Set up a single chunk in the metadata.
    ChunkType chunk1 = setUpChunk(
        collName, collUUID, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    // Going to request that this chunk gets migrated.
    const std::vector<MigrateInfo> migrationRequests{{kShardId1,
                                                      collName,
                                                      chunk1,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance}};

    auto future = launchAsync([this, chunk1, migrationRequests] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = cc().makeOperationContext();

        // Scheduling a moveChunk command requires finding a host to which to send the command. Set
        // up a dummy host for kShardHost0.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);

        MigrationStatuses migrationStatuses = _migrationManager->executeMigrationsForAutoBalance(
            opCtx.get(), migrationRequests, 0, kDefaultSecondaryThrottle, false);

        ASSERT_EQ(ErrorCodes::ChunkTooBig,
                  migrationStatuses.at(migrationRequests.front().getName()));
    });

    // Expect only one moveChunk command to be called.
    expectMoveChunkCommand(collName, chunk1, kShardId1, BSON("ok" << 0 << "chunkTooBig" << true));

    // Run the MigrationManager code.
    future.default_timed_get();
}

TEST_F(MigrationManagerTest, InterruptMigration) {
    // Set up one shard in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    const auto collUUID = UUID::gen();
    ChunkVersion version(2, 0, OID::gen(), Timestamp(42));

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, collUUID, version);

    // Set up a single chunk in the metadata.
    ChunkType chunk = setUpChunk(
        collName, collUUID, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    auto future = launchAsync([&] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = cc().makeOperationContext();

        // Scheduling a moveChunk command requires finding a host to which to send the command. Set
        // up a dummy host for kShardHost0.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);

        ASSERT_EQ(
            ErrorCodes::BalancerInterrupted,
            _migrationManager->executeManualMigration(opCtx.get(),
                                                      {kShardId1,
                                                       collName,
                                                       chunk,
                                                       MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                       MigrateInfo::chunksImbalance},
                                                      0,
                                                      kDefaultSecondaryThrottle,
                                                      false));
    });

    // Wait till the move chunk request gets sent and pretend that it is stuck by never responding
    // to the request
    network()->enterNetwork();
    network()->blackHole(network()->getNextReadyRequest());
    network()->exitNetwork();

    // Now that the migration request is 'pending', try to cancel the migration manager. This should
    // succeed.
    _migrationManager->interruptAndDisableMigrations();

    // Ensure that cancellations get processed
    network()->enterNetwork();
    network()->runReadyNetworkOperations();
    network()->exitNetwork();

    // Ensure that the previously scheduled migration is cancelled
    future.default_timed_get();

    // Ensure that no new migrations can be scheduled
    ASSERT_EQ(ErrorCodes::BalancerInterrupted,
              _migrationManager->executeManualMigration(operationContext(),
                                                        {kShardId1,
                                                         collName,
                                                         chunk,
                                                         MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                         MigrateInfo::chunksImbalance},
                                                        0,
                                                        kDefaultSecondaryThrottle,
                                                        false));

    // Ensure that the migration manager is no longer handling any migrations.
    _migrationManager->drainActiveMigrations();

    // Check that the migration that was active when the migration manager was interrupted can be
    // found in config.migrations (and thus would be recovered if a migration manager were to start
    // up again).
    auto statusWithMigrationsQueryResponse =
        shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kMajorityReadConcern,
            MigrationType::ConfigNS,
            BSON(MigrationType::ns(collName.ns()) << MigrationType::min(chunk.getMin())),
            BSONObj(),
            boost::none);
    Shard::QueryResponse migrationsQueryResponse =
        uassertStatusOK(statusWithMigrationsQueryResponse);
    ASSERT_EQUALS(1U, migrationsQueryResponse.docs.size());

    ASSERT_OK(catalogClient()->removeConfigDocuments(
        operationContext(),
        MigrationType::ConfigNS,
        BSON(MigrationType::ns(collName.ns()) << MigrationType::min(chunk.getMin())),
        kMajorityWriteConcern));

    // Restore the migration manager back to the started state, which is expected by tearDown
    _migrationManager->startRecoveryAndAcquireDistLocks(operationContext());
    _migrationManager->finishRecovery(operationContext(), 0, kDefaultSecondaryThrottle);
}

TEST_F(MigrationManagerTest, RestartMigrationManager) {
    // Set up one shard in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    const auto collUUID = UUID::gen();
    ChunkVersion version(2, 0, OID::gen(), Timestamp(42));

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, collUUID, version);

    // Set up a single chunk in the metadata.
    ChunkType chunk1 = setUpChunk(
        collName, collUUID, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    // Go through the lifecycle of the migration manager
    _migrationManager->interruptAndDisableMigrations();
    _migrationManager->drainActiveMigrations();
    _migrationManager->startRecoveryAndAcquireDistLocks(operationContext());
    _migrationManager->finishRecovery(operationContext(), 0, kDefaultSecondaryThrottle);

    auto future = launchAsync([&] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = cc().makeOperationContext();

        // Scheduling a moveChunk command requires finding a host to which to send the command. Set
        // up a dummy host for kShardHost0.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);

        ASSERT_OK(
            _migrationManager->executeManualMigration(opCtx.get(),
                                                      {kShardId1,
                                                       collName,
                                                       chunk1,
                                                       MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                       MigrateInfo::chunksImbalance},
                                                      0,
                                                      kDefaultSecondaryThrottle,
                                                      false));
    });

    // Expect only one moveChunk command to be called.
    expectMoveChunkCommand(collName, chunk1, kShardId1, Status::OK());

    // Run the MigrationManager code.
    future.default_timed_get();
}

TEST_F(MigrationManagerTest, MigrationRecovery) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    const auto collUUID = UUID::gen();
    ChunkVersion version(1, 0, OID::gen(), Timestamp(42));

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, collUUID, version);

    // Set up two chunks in the metadata and set up two fake active migrations by writing documents
    // to the config.migrations collection.
    ChunkType chunk1 = setUpChunk(
        collName, collUUID, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version);
    version.incMinor();
    ChunkType chunk2 = setUpChunk(
        collName, collUUID, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version);

    _migrationManager->interruptAndDisableMigrations();
    _migrationManager->drainActiveMigrations();

    setUpMigration(collName, chunk1, kShardId1.toString());
    setUpMigration(collName, chunk2, kShardId3.toString());

    // Mimic all config distlocks being released on config server stepup to primary.
    DistLockManager::get(operationContext())->unlockAll(operationContext());

    _migrationManager->startRecoveryAndAcquireDistLocks(operationContext());

    auto future = launchAsync([this] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding hosts to which to send the commands.
        // Set up dummy hosts for the source shards.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(opCtx.get(), kShardId2)->setFindHostReturnValue(kShardHost2);

        _migrationManager->finishRecovery(opCtx.get(), 0, kDefaultSecondaryThrottle);
    });

    // Expect two moveChunk commands.
    expectMoveChunkCommand(collName, chunk1, kShardId1, Status::OK());
    expectMoveChunkCommand(collName, chunk2, kShardId3, Status::OK());

    // Run the MigrationManager code.
    future.default_timed_get();
}

TEST_F(MigrationManagerTest, FailMigrationRecovery) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    const auto collUUID = UUID::gen();
    ChunkVersion version(1, 0, OID::gen(), Timestamp(42));

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, collUUID, version);

    // Set up two chunks in the metadata.
    ChunkType chunk1 = setUpChunk(
        collName, collUUID, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version);
    version.incMinor();
    ChunkType chunk2 = setUpChunk(
        collName, collUUID, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version);

    _migrationManager->interruptAndDisableMigrations();
    _migrationManager->drainActiveMigrations();

    setUpMigration(collName, chunk1, kShardId1.toString());

    // Set up a fake active migration document that will fail MigrationType parsing -- missing
    // field.
    BSONObjBuilder builder;
    builder.append("_id", "testing");
    // No MigrationType::ns() field!
    builder.append(MigrationType::min(), chunk2.getMin());
    builder.append(MigrationType::max(), chunk2.getMax());
    builder.append(MigrationType::toShard(), kShardId3.toString());
    builder.append(MigrationType::fromShard(), chunk2.getShard().toString());
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), MigrationType::ConfigNS, builder.obj(), kMajorityWriteConcern));

    // Take the distributed lock for the collection, which should be released during recovery when
    // it fails. Any dist lock held by the config server will be released via proccessId, so the
    // session ID used here doesn't matter.
    ASSERT_OK(DistLockManager::get(operationContext())
                  ->lockDirect(operationContext(),
                               collName.ns(),
                               "MigrationManagerTest",
                               DistLockManager::kSingleLockAttemptTimeout));

    _migrationManager->startRecoveryAndAcquireDistLocks(operationContext());
    _migrationManager->finishRecovery(operationContext(), 0, kDefaultSecondaryThrottle);

    // MigrationManagerTest::tearDown checks that the config.migrations collection is empty and all
    // distributed locks are unlocked.
}

// Check that retriable / replset monitor altering errors returned from remote moveChunk commands
// sent to source shards are not returned to the caller (mongos), but instead converted into
// OperationFailed errors.
TEST_F(MigrationManagerTest, RemoteCallErrorConversionToOperationFailed) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    const auto collUUID = UUID::gen();
    ChunkVersion version(1, 0, OID::gen(), Timestamp(42));

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, collUUID, version);

    // Set up two chunks in the metadata.
    ChunkType chunk1 = setUpChunk(
        collName, collUUID, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version);
    version.incMinor();
    ChunkType chunk2 = setUpChunk(
        collName, collUUID, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version);

    // Going to request that these two chunks get migrated.
    const std::vector<MigrateInfo> migrationRequests{{kShardId1,
                                                      collName,
                                                      chunk1,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance},
                                                     {kShardId3,
                                                      collName,
                                                      chunk2,
                                                      MoveChunkRequest::ForceJumbo::kDoNotForce,
                                                      MigrateInfo::chunksImbalance}};

    auto future = launchAsync([&] {
        ThreadClient tc("Test", getServiceContext());
        auto opCtx = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding a host to which to send the command.
        // Set up dummy hosts for the source shards.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(opCtx.get(), kShardId2)->setFindHostReturnValue(kShardHost2);

        MigrationStatuses migrationStatuses = _migrationManager->executeMigrationsForAutoBalance(
            opCtx.get(), migrationRequests, 0, kDefaultSecondaryThrottle, false);

        ASSERT_EQ(ErrorCodes::OperationFailed,
                  migrationStatuses.at(migrationRequests.front().getName()));
        ASSERT_EQ(ErrorCodes::OperationFailed,
                  migrationStatuses.at(migrationRequests.back().getName()));
    });

    // Expect a moveChunk command that will fail with a retriable error.
    expectMoveChunkCommand(
        collName,
        chunk1,
        kShardId1,
        Status(ErrorCodes::NotPrimaryOrSecondary,
               "RemoteCallErrorConversionToOperationFailedCheck generated error."));

    // Expect a moveChunk command that will fail with a replset monitor updating error.
    expectMoveChunkCommand(
        collName,
        chunk2,
        kShardId3,
        Status(ErrorCodes::NetworkInterfaceExceededTimeLimit,
               "RemoteCallErrorConversionToOperationFailedCheck generated error."));

    // Run the MigrationManager code.
    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
