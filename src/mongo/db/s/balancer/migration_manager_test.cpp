/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/balancer/migration_manager.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/request_types/move_chunk_request.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using std::vector;
using unittest::assertGet;

const auto kShardId0 = ShardId("shard0");
const auto kShardId1 = ShardId("shard1");
const auto kShardId2 = ShardId("shard2");
const auto kShardId3 = ShardId("shard3");

const HostAndPort kShardHost0 = HostAndPort("TestHost0", 12345);
const HostAndPort kShardHost1 = HostAndPort("TestHost1", 12346);
const HostAndPort kShardHost2 = HostAndPort("TestHost2", 12347);
const HostAndPort kShardHost3 = HostAndPort("TestHost3", 12348);

const MigrationSecondaryThrottleOptions kDefaultSecondaryThrottle =
    MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kDefault);

const long long kMaxSizeMB = 100;
const std::string kPattern = "_id";

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(15));

class MigrationManagerTest : public ConfigServerTestFixture {
protected:
    /**
     * Returns the mock targeter for the specified shard. Useful to use like so
     *
     *     shardTargeterMock(opCtx, shardId)->setFindHostReturnValue(shardHost);
     *
     * Then calls to RemoteCommandTargeterMock::findHost will return HostAndPort "shardHost" for
     * Shard "shardId".
     *
     * Scheduling a command requires a shard host target. The command will be caught by the mock
     * network, but sending the command requires finding the shard's host.
     */
    std::shared_ptr<RemoteCommandTargeterMock> shardTargeterMock(OperationContext* opCtx,
                                                                 ShardId shardId);

    /**
     * Inserts a document into the config.databases collection to indicate that "dbName" is sharded
     * with primary "primaryShard".
     */
    void setUpDatabase(const std::string& dbName, const ShardId primaryShard);

    /**
     * Inserts a document into the config.collections collection to indicate that "collName" is
     * sharded with version "version". The shard key pattern defaults to "_id".
     */
    void setUpCollection(const NamespaceString& collName, ChunkVersion version);

    /**
     * Inserts a document into the config.chunks collection so that the chunk defined by the
     * parameters exists. Returns a ChunkType defined by the parameters.
     */
    ChunkType setUpChunk(const NamespaceString& collName,
                         const BSONObj& chunkMin,
                         const BSONObj& chunkMax,
                         const ShardId& shardId,
                         const ChunkVersion& version);

    /**
     * Inserts a document into the config.migrations collection as an active migration.
     */
    void setUpMigration(const ChunkType& chunk, const ShardId& toShard);

    /**
     * Asserts that config.migrations is empty and config.locks contains no locked documents other
     * than the balancer's, both of which should be true if the MigrationManager is inactive and
     * behaving properly.
     */
    void checkMigrationsCollectionIsEmptyAndLocksAreUnlocked();

    /**
     * Sets up mock network to expect a moveChunk command and return a fixed BSON response or a
     * "returnStatus".
     */
    void expectMoveChunkCommand(const ChunkType& chunk,
                                const ShardId& toShardId,
                                const BSONObj& response);
    void expectMoveChunkCommand(const ChunkType& chunk,
                                const ShardId& toShardId,
                                const Status& returnStatus);

    // Random static initialization order can result in X constructor running before Y constructor
    // if X and Y are defined in different source files. Defining variables here to enforce order.
    const BSONObj kShard0 =
        BSON(ShardType::name(kShardId0.toString()) << ShardType::host(kShardHost0.toString())
                                                   << ShardType::maxSizeMB(kMaxSizeMB));
    const BSONObj kShard1 =
        BSON(ShardType::name(kShardId1.toString()) << ShardType::host(kShardHost1.toString())
                                                   << ShardType::maxSizeMB(kMaxSizeMB));
    const BSONObj kShard2 =
        BSON(ShardType::name(kShardId2.toString()) << ShardType::host(kShardHost2.toString())
                                                   << ShardType::maxSizeMB(kMaxSizeMB));
    const BSONObj kShard3 =
        BSON(ShardType::name(kShardId3.toString()) << ShardType::host(kShardHost3.toString())
                                                   << ShardType::maxSizeMB(kMaxSizeMB));

    const KeyPattern kKeyPattern = KeyPattern(BSON(kPattern << 1));

    std::unique_ptr<MigrationManager> _migrationManager;

private:
    void setUp() override;
    void tearDown() override;
};

void MigrationManagerTest::setUp() {
    ConfigServerTestFixture::setUp();
    _migrationManager = stdx::make_unique<MigrationManager>(getServiceContext());
    _migrationManager->startRecoveryAndAcquireDistLocks(operationContext());
    _migrationManager->finishRecovery(operationContext(), 0, kDefaultSecondaryThrottle);
}

void MigrationManagerTest::tearDown() {
    checkMigrationsCollectionIsEmptyAndLocksAreUnlocked();
    _migrationManager->interruptAndDisableMigrations();
    _migrationManager->drainActiveMigrations();
    _migrationManager.reset();
    ConfigServerTestFixture::tearDown();
}

std::shared_ptr<RemoteCommandTargeterMock> MigrationManagerTest::shardTargeterMock(
    OperationContext* opCtx, ShardId shardId) {
    return RemoteCommandTargeterMock::get(
        uassertStatusOK(shardRegistry()->getShard(opCtx, shardId))->getTargeter());
}

void MigrationManagerTest::setUpDatabase(const std::string& dbName, const ShardId primaryShard) {
    DatabaseType db(dbName, primaryShard, true, databaseVersion::makeNew());
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), DatabaseType::ConfigNS, db.toBSON(), kMajorityWriteConcern));
}

void MigrationManagerTest::setUpCollection(const NamespaceString& collName, ChunkVersion version) {
    CollectionType coll;
    coll.setNs(collName);
    coll.setEpoch(version.epoch());
    coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(version.toLong()));
    coll.setKeyPattern(kKeyPattern);
    coll.setUnique(false);
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), CollectionType::ConfigNS, coll.toBSON(), kMajorityWriteConcern));
}

ChunkType MigrationManagerTest::setUpChunk(const NamespaceString& collName,
                                           const BSONObj& chunkMin,
                                           const BSONObj& chunkMax,
                                           const ShardId& shardId,
                                           const ChunkVersion& version) {
    ChunkType chunk;
    chunk.setNS(collName);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);
    chunk.setShard(shardId);
    chunk.setVersion(version);
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ChunkType::ConfigNS, chunk.toConfigBSON(), kMajorityWriteConcern));
    return chunk;
}

void MigrationManagerTest::setUpMigration(const ChunkType& chunk, const ShardId& toShard) {
    BSONObjBuilder builder;
    builder.append(MigrationType::ns(), chunk.getNS().ns());
    builder.append(MigrationType::min(), chunk.getMin());
    builder.append(MigrationType::max(), chunk.getMax());
    builder.append(MigrationType::toShard(), toShard.toString());
    builder.append(MigrationType::fromShard(), chunk.getShard().toString());
    chunk.getVersion().appendWithFieldForCommands(&builder, "chunkVersion");

    MigrationType migrationType = assertGet(MigrationType::fromBSON(builder.obj()));
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    MigrationType::ConfigNS,
                                                    migrationType.toBSON(),
                                                    kMajorityWriteConcern));
}

void MigrationManagerTest::checkMigrationsCollectionIsEmptyAndLocksAreUnlocked() {
    auto statusWithMigrationsQueryResponse =
        shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kMajorityReadConcern,
            MigrationType::ConfigNS,
            BSONObj(),
            BSONObj(),
            boost::none);
    Shard::QueryResponse migrationsQueryResponse =
        uassertStatusOK(statusWithMigrationsQueryResponse);
    ASSERT_EQUALS(0U, migrationsQueryResponse.docs.size());

    auto statusWithLocksQueryResponse = shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
        operationContext(),
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kMajorityReadConcern,
        LocksType::ConfigNS,
        BSON(LocksType::state(LocksType::LOCKED) << LocksType::name("{ '$ne' : 'balancer'}")),
        BSONObj(),
        boost::none);
    Shard::QueryResponse locksQueryResponse = uassertStatusOK(statusWithLocksQueryResponse);
    ASSERT_EQUALS(0U, locksQueryResponse.docs.size());
}

void MigrationManagerTest::expectMoveChunkCommand(const ChunkType& chunk,
                                                  const ShardId& toShardId,
                                                  const BSONObj& response) {
    onCommand([&chunk, &toShardId, &response](const RemoteCommandRequest& request) {
        NamespaceString nss(request.cmdObj.firstElement().valueStringData());
        ASSERT_EQ(chunk.getNS(), nss);

        const StatusWith<MoveChunkRequest> moveChunkRequestWithStatus =
            MoveChunkRequest::createFromCommand(nss, request.cmdObj);
        ASSERT_OK(moveChunkRequestWithStatus.getStatus());

        ASSERT_EQ(chunk.getNS(), moveChunkRequestWithStatus.getValue().getNss());
        ASSERT_BSONOBJ_EQ(chunk.getMin(), moveChunkRequestWithStatus.getValue().getMinKey());
        ASSERT_BSONOBJ_EQ(chunk.getMax(), moveChunkRequestWithStatus.getValue().getMaxKey());
        ASSERT_EQ(chunk.getShard(), moveChunkRequestWithStatus.getValue().getFromShardId());

        ASSERT_EQ(toShardId, moveChunkRequestWithStatus.getValue().getToShardId());

        return response;
    });
}

void MigrationManagerTest::expectMoveChunkCommand(const ChunkType& chunk,
                                                  const ShardId& toShardId,
                                                  const Status& returnStatus) {
    BSONObjBuilder resultBuilder;
    CommandHelpers::appendCommandStatusNoThrow(resultBuilder, returnStatus);
    expectMoveChunkCommand(chunk, toShardId, resultBuilder.obj());
}

TEST_F(MigrationManagerTest, OneCollectionTwoMigrations) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    ChunkVersion version(2, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up two chunks in the metadata.
    ChunkType chunk1 =
        setUpChunk(collName, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version);
    version.incMinor();
    ChunkType chunk2 =
        setUpChunk(collName, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version);

    // Going to request that these two chunks get migrated.
    const std::vector<MigrateInfo> migrationRequests{{kShardId1, chunk1}, {kShardId3, chunk2}};

    auto future = launchAsync([this, migrationRequests] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
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
    expectMoveChunkCommand(chunk1, kShardId1, Status::OK());
    expectMoveChunkCommand(chunk2, kShardId3, Status::OK());

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
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
    const NamespaceString collName2(dbName, "baz");
    ChunkVersion version1(2, 0, OID::gen());
    ChunkVersion version2(2, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName1, version1);
    setUpCollection(collName2, version2);

    // Set up two chunks in the metadata for each collection.
    ChunkType chunk1coll1 =
        setUpChunk(collName1, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version1);
    version1.incMinor();
    ChunkType chunk2coll1 =
        setUpChunk(collName1, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version1);

    ChunkType chunk1coll2 =
        setUpChunk(collName2, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version2);
    version2.incMinor();
    ChunkType chunk2coll2 =
        setUpChunk(collName2, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version2);

    // Going to request that these four chunks get migrated.
    const std::vector<MigrateInfo> migrationRequests{{kShardId1, chunk1coll1},
                                                     {kShardId3, chunk2coll1},
                                                     {kShardId1, chunk1coll2},
                                                     {kShardId3, chunk2coll2}};

    auto future = launchAsync([this, migrationRequests] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
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
    expectMoveChunkCommand(chunk1coll1, kShardId1, Status::OK());
    expectMoveChunkCommand(chunk2coll1, kShardId3, Status::OK());
    expectMoveChunkCommand(chunk1coll2, kShardId1, Status::OK());
    expectMoveChunkCommand(chunk2coll2, kShardId3, Status::OK());

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
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
    ChunkVersion version(2, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up two chunks in the metadata.
    ChunkType chunk1 =
        setUpChunk(collName, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version);
    version.incMinor();
    ChunkType chunk2 =
        setUpChunk(collName, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version);

    // Going to request that these two chunks get migrated.
    const std::vector<MigrateInfo> migrationRequests{{kShardId1, chunk1}, {kShardId3, chunk2}};

    auto future = launchAsync([this, chunk1, chunk2, migrationRequests] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();

        // Scheduling a moveChunk command requires finding a host to which to send the command. Set
        // up a dummy host for kShardHost0, and return an error for kShardHost3.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(opCtx.get(), kShardId2)
            ->setFindHostReturnValue(
                Status(ErrorCodes::ReplicaSetNotFound, "SourceShardNotFound generated error."));

        MigrationStatuses migrationStatuses = _migrationManager->executeMigrationsForAutoBalance(
            opCtx.get(), migrationRequests, 0, kDefaultSecondaryThrottle, false);

        ASSERT_OK(migrationStatuses.at(chunk1.getName()));
        ASSERT_EQ(ErrorCodes::ReplicaSetNotFound, migrationStatuses.at(chunk2.getName()));
    });

    // Expect only one moveChunk command to be called.
    expectMoveChunkCommand(chunk1, kShardId1, Status::OK());

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
}

// TODO: Delete in 3.8
TEST_F(MigrationManagerTest, JumboChunkResponseBackwardsCompatibility) {
    // Set up one shard in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    ChunkVersion version(2, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up a single chunk in the metadata.
    ChunkType chunk1 =
        setUpChunk(collName, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    // Going to request that this chunk gets migrated.
    const std::vector<MigrateInfo> migrationRequests{{kShardId1, chunk1}};

    auto future = launchAsync([this, chunk1, migrationRequests] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();

        // Scheduling a moveChunk command requires finding a host to which to send the command. Set
        // up a dummy host for kShardHost0.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);

        MigrationStatuses migrationStatuses = _migrationManager->executeMigrationsForAutoBalance(
            opCtx.get(), migrationRequests, 0, kDefaultSecondaryThrottle, false);

        ASSERT_EQ(ErrorCodes::ChunkTooBig, migrationStatuses.at(chunk1.getName()));
    });

    // Expect only one moveChunk command to be called.
    expectMoveChunkCommand(chunk1, kShardId1, BSON("ok" << 0 << "chunkTooBig" << true));

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
}

TEST_F(MigrationManagerTest, InterruptMigration) {
    // Set up one shard in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    const std::string dbName = "foo";
    const NamespaceString collName(dbName, "bar");
    ChunkVersion version(2, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up a single chunk in the metadata.
    ChunkType chunk =
        setUpChunk(collName, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();

        // Scheduling a moveChunk command requires finding a host to which to send the command. Set
        // up a dummy host for kShardHost0.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);

        ASSERT_EQ(ErrorCodes::BalancerInterrupted,
                  _migrationManager->executeManualMigration(
                      opCtx.get(), {kShardId1, chunk}, 0, kDefaultSecondaryThrottle, false));
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
    future.timed_get(kFutureTimeout);

    // Ensure that no new migrations can be scheduled
    ASSERT_EQ(ErrorCodes::BalancerInterrupted,
              _migrationManager->executeManualMigration(
                  operationContext(), {kShardId1, chunk}, 0, kDefaultSecondaryThrottle, false));

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
            BSON(MigrationType::name(chunk.getName())),
            BSONObj(),
            boost::none);
    Shard::QueryResponse migrationsQueryResponse =
        uassertStatusOK(statusWithMigrationsQueryResponse);
    ASSERT_EQUALS(1U, migrationsQueryResponse.docs.size());

    ASSERT_OK(catalogClient()->removeConfigDocuments(operationContext(),
                                                     MigrationType::ConfigNS,
                                                     BSON(MigrationType::name(chunk.getName())),
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
    ChunkVersion version(2, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up a single chunk in the metadata.
    ChunkType chunk1 =
        setUpChunk(collName, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    // Go through the lifecycle of the migration manager
    _migrationManager->interruptAndDisableMigrations();
    _migrationManager->drainActiveMigrations();
    _migrationManager->startRecoveryAndAcquireDistLocks(operationContext());
    _migrationManager->finishRecovery(operationContext(), 0, kDefaultSecondaryThrottle);

    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();

        // Scheduling a moveChunk command requires finding a host to which to send the command. Set
        // up a dummy host for kShardHost0.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);

        ASSERT_OK(_migrationManager->executeManualMigration(
            opCtx.get(), {kShardId1, chunk1}, 0, kDefaultSecondaryThrottle, false));
    });

    // Expect only one moveChunk command to be called.
    expectMoveChunkCommand(chunk1, kShardId1, Status::OK());

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
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
    ChunkVersion version(1, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up two chunks in the metadata and set up two fake active migrations by writing documents
    // to the config.migrations collection.
    ChunkType chunk1 =
        setUpChunk(collName, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version);
    version.incMinor();
    ChunkType chunk2 =
        setUpChunk(collName, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version);

    _migrationManager->interruptAndDisableMigrations();
    _migrationManager->drainActiveMigrations();

    setUpMigration(chunk1, kShardId1.toString());
    setUpMigration(chunk2, kShardId3.toString());

    // Mimic all config distlocks being released on config server stepup to primary.
    auto distLockManager = catalogClient()->getDistLockManager();
    distLockManager->unlockAll(operationContext(), distLockManager->getProcessID());

    _migrationManager->startRecoveryAndAcquireDistLocks(operationContext());

    auto future = launchAsync([this] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding hosts to which to send the commands.
        // Set up dummy hosts for the source shards.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(opCtx.get(), kShardId2)->setFindHostReturnValue(kShardHost2);

        _migrationManager->finishRecovery(opCtx.get(), 0, kDefaultSecondaryThrottle);
    });

    // Expect two moveChunk commands.
    expectMoveChunkCommand(chunk1, kShardId1, Status::OK());
    expectMoveChunkCommand(chunk2, kShardId3, Status::OK());

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
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
    ChunkVersion version(1, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up two chunks in the metadata.
    ChunkType chunk1 =
        setUpChunk(collName, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version);
    version.incMinor();
    ChunkType chunk2 =
        setUpChunk(collName, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version);

    _migrationManager->interruptAndDisableMigrations();
    _migrationManager->drainActiveMigrations();

    setUpMigration(chunk1, kShardId1.toString());

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
    ASSERT_OK(catalogClient()->getDistLockManager()->lockWithSessionID(
        operationContext(),
        collName.ns(),
        "MigrationManagerTest",
        OID::gen(),
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
    ChunkVersion version(1, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up two chunks in the metadata.
    ChunkType chunk1 =
        setUpChunk(collName, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, version);
    version.incMinor();
    ChunkType chunk2 =
        setUpChunk(collName, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, version);

    auto future = launchAsync([&] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding a host to which to send the command.
        // Set up dummy hosts for the source shards.
        shardTargeterMock(opCtx.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(opCtx.get(), kShardId2)->setFindHostReturnValue(kShardHost2);

        MigrationStatuses migrationStatuses = _migrationManager->executeMigrationsForAutoBalance(
            opCtx.get(),
            {{kShardId1, chunk1}, {kShardId3, chunk2}},
            0,
            kDefaultSecondaryThrottle,
            false);

        ASSERT_EQ(ErrorCodes::OperationFailed, migrationStatuses.at(chunk1.getName()));
        ASSERT_EQ(ErrorCodes::OperationFailed, migrationStatuses.at(chunk2.getName()));
    });

    // Expect a moveChunk command that will fail with a retriable error.
    expectMoveChunkCommand(
        chunk1,
        kShardId1,
        Status(ErrorCodes::NotMasterOrSecondary,
               "RemoteCallErrorConversionToOperationFailedCheck generated error."));

    // Expect a moveChunk command that will fail with a replset monitor updating error.
    expectMoveChunkCommand(
        chunk2,
        kShardId3,
        Status(ErrorCodes::NetworkInterfaceExceededTimeLimit,
               "RemoteCallErrorConversionToOperationFailedCheck generated error."));

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
