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
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/balancer/migration_manager.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/replset/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using MigrationRequest = MigrationManager::MigrationRequest;
using MigrationRequestVector = MigrationManager::MigrationRequestVector;

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
     *     shardTargeterMock(txn, shardId)->setFindHostReturnValue(shardHost);
     *
     * Then calls to RemoteCommandTargeterMock::findHost will return HostAndPort "shardHost" for
     * Shard "shardId".
     *
     * Scheduling a command requires a shard host target. The command will be caught by the mock
     * network, but sending the command requires finding the shard's host.
     */
    std::shared_ptr<RemoteCommandTargeterMock> shardTargeterMock(OperationContext* txn,
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
    void setUpCollection(const std::string collName, ChunkVersion version);

    /**
     * Inserts a document into the config.chunks collection so that the chunk defined by the
     * parameters exists. Returns a ChunkType defined by the parameters.
     */
    ChunkType setUpChunk(const std::string& collName,
                         const BSONObj& chunkMin,
                         const BSONObj& chunkMax,
                         const ShardId& shardId,
                         const ChunkVersion& version);

    /**
     * Sets up mock network to expect a moveChunk command and return a fixed BSON response or a
     * "returnStatus".
     */
    void expectMoveChunkCommand(const ChunkType& chunk,
                                const ShardId& toShardId,
                                const bool& takeDistLock,
                                const BSONObj& response);
    void expectMoveChunkCommand(const ChunkType& chunk,
                                const ShardId& toShardId,
                                const bool& takeDistLock,
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
    _migrationManager = stdx::make_unique<MigrationManager>();
}

void MigrationManagerTest::tearDown() {
    _migrationManager.reset();
    ConfigServerTestFixture::tearDown();
}

std::shared_ptr<RemoteCommandTargeterMock> MigrationManagerTest::shardTargeterMock(
    OperationContext* txn, ShardId shardId) {
    return RemoteCommandTargeterMock::get(shardRegistry()->getShard(txn, shardId)->getTargeter());
}

void MigrationManagerTest::setUpDatabase(const std::string& dbName, const ShardId primaryShard) {
    DatabaseType db;
    db.setName(dbName);
    db.setPrimary(primaryShard);
    db.setSharded(true);
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), DatabaseType::ConfigNS, db.toBSON(), kMajorityWriteConcern));
}

void MigrationManagerTest::setUpCollection(const std::string collName, ChunkVersion version) {
    CollectionType coll;
    coll.setNs(NamespaceString(collName));
    coll.setEpoch(version.epoch());
    coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(version.toLong()));
    coll.setKeyPattern(kKeyPattern);
    coll.setUnique(false);
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), CollectionType::ConfigNS, coll.toBSON(), kMajorityWriteConcern));
}

ChunkType MigrationManagerTest::setUpChunk(const std::string& collName,
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
        operationContext(), ChunkType::ConfigNS, chunk.toBSON(), kMajorityWriteConcern));
    return chunk;
}

void MigrationManagerTest::expectMoveChunkCommand(const ChunkType& chunk,
                                                  const ShardId& toShardId,
                                                  const bool& takeDistLock,
                                                  const BSONObj& response) {
    onCommand([&chunk, &toShardId, &takeDistLock, &response](const RemoteCommandRequest& request) {
        NamespaceString nss(request.cmdObj.firstElement().valueStringData());
        ASSERT_EQ(chunk.getNS(), nss.ns());

        const StatusWith<MoveChunkRequest> moveChunkRequestWithStatus =
            MoveChunkRequest::createFromCommand(nss, request.cmdObj);
        ASSERT_OK(moveChunkRequestWithStatus.getStatus());

        ASSERT_EQ(chunk.getNS(), moveChunkRequestWithStatus.getValue().getNss().ns());
        ASSERT_EQ(chunk.getMin(), moveChunkRequestWithStatus.getValue().getMinKey());
        ASSERT_EQ(chunk.getMax(), moveChunkRequestWithStatus.getValue().getMaxKey());
        ASSERT_EQ(chunk.getShard(), moveChunkRequestWithStatus.getValue().getFromShardId());

        ASSERT_EQ(toShardId, moveChunkRequestWithStatus.getValue().getToShardId());
        ASSERT_EQ(takeDistLock, moveChunkRequestWithStatus.getValue().getTakeDistLock());

        return response;
    });
}

void MigrationManagerTest::expectMoveChunkCommand(const ChunkType& chunk,
                                                  const ShardId& toShardId,
                                                  const bool& takeDistLock,
                                                  const Status& returnStatus) {
    BSONObjBuilder resultBuilder;
    Command::appendCommandStatus(resultBuilder, returnStatus);
    expectMoveChunkCommand(chunk, toShardId, takeDistLock, resultBuilder.obj());
}

TEST_F(MigrationManagerTest, OneCollectionTwoMigrations) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    std::string dbName = "foo";
    std::string collName = "foo.bar";
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
    const MigrationRequestVector migrationRequests{
        MigrationRequest(
            MigrateInfo(chunk1.getNS(), kShardId1, chunk1), 0, kDefaultSecondaryThrottle, false),
        MigrationRequest(
            MigrateInfo(chunk2.getNS(), kShardId3, chunk2), 0, kDefaultSecondaryThrottle, false)};

    auto future = launchAsync([this, migrationRequests] {
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding a host to which to send the command.
        // Set up dummy hosts for the source shards.
        shardTargeterMock(txn.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(txn.get(), kShardId2)->setFindHostReturnValue(kShardHost2);

        MigrationStatuses migrationStatuses =
            _migrationManager->scheduleMigrations(txn.get(), migrationRequests);

        for (const auto& migrateInfo : migrationRequests) {
            ASSERT_OK(migrationStatuses.at(migrateInfo.migrateInfo.getName()));
        }
    });

    // Expect two moveChunk commands.
    expectMoveChunkCommand(chunk1, kShardId1, false, Status::OK());
    expectMoveChunkCommand(chunk2, kShardId3, false, Status::OK());

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
    std::string collName1 = "foo.bar";
    std::string collName2 = "foo.baz";
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
    const MigrationRequestVector migrationRequests{
        MigrationRequest(MigrateInfo(chunk1coll1.getNS(), kShardId1, chunk1coll1),
                         0,
                         kDefaultSecondaryThrottle,
                         false),
        MigrationRequest(MigrateInfo(chunk2coll1.getNS(), kShardId3, chunk2coll1),
                         0,
                         kDefaultSecondaryThrottle,
                         false),
        MigrationRequest(MigrateInfo(chunk1coll2.getNS(), kShardId1, chunk1coll2),
                         0,
                         kDefaultSecondaryThrottle,
                         false),
        MigrationRequest(MigrateInfo(chunk2coll2.getNS(), kShardId3, chunk2coll2),
                         0,
                         kDefaultSecondaryThrottle,
                         false)};

    auto future = launchAsync([this, migrationRequests] {
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding a host to which to send the command.
        // Set up dummy hosts for the source shards.
        shardTargeterMock(txn.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(txn.get(), kShardId2)->setFindHostReturnValue(kShardHost2);

        MigrationStatuses migrationStatuses =
            _migrationManager->scheduleMigrations(txn.get(), migrationRequests);

        for (const auto& migrateInfo : migrationRequests) {
            ASSERT_OK(migrationStatuses.at(migrateInfo.migrateInfo.getName()));
        }
    });

    // Expect four moveChunk commands.
    expectMoveChunkCommand(chunk1coll1, kShardId1, false, Status::OK());
    expectMoveChunkCommand(chunk2coll1, kShardId3, false, Status::OK());
    expectMoveChunkCommand(chunk1coll2, kShardId1, false, Status::OK());
    expectMoveChunkCommand(chunk2coll2, kShardId3, false, Status::OK());

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
}

// Old v3.2 shards expect to take the distributed lock before executing a moveChunk command. The
// MigrationManager should take the distlock and fail the first moveChunk command with an old shard,
// and then release the lock and retry the command successfully.
TEST_F(MigrationManagerTest, SameCollectionOldShardMigration) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    std::string dbName = "foo";
    std::string collName = "foo.bar";
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
    const MigrationRequestVector migrationRequests{
        MigrationRequest(
            MigrateInfo(chunk1.getNS(), kShardId1, chunk1), 0, kDefaultSecondaryThrottle, false),
        MigrationRequest(
            MigrateInfo(chunk2.getNS(), kShardId3, chunk2), 0, kDefaultSecondaryThrottle, false)};

    auto future = launchAsync([this, migrationRequests] {
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding a host to which to send the command.
        // Set up dummy hosts for the source shards.
        shardTargeterMock(txn.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(txn.get(), kShardId2)->setFindHostReturnValue(kShardHost2);

        MigrationStatuses migrationStatuses =
            _migrationManager->scheduleMigrations(txn.get(), migrationRequests);

        for (const auto& migrateInfo : migrationRequests) {
            ASSERT_OK(migrationStatuses.at(migrateInfo.migrateInfo.getName()));
        }
    });

    // Expect two moveChunk commands.
    expectMoveChunkCommand(
        chunk1,
        kShardId1,
        false,
        Status(ErrorCodes::LockBusy, "SameCollectionOldShardMigration generated error."));
    expectMoveChunkCommand(chunk2, kShardId3, false, Status::OK());
    expectMoveChunkCommand(chunk1, kShardId1, true, Status::OK());

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
}

// Fail a migration if an old v3.2 shard fails to acquire the distributed lock more than once. The
// first LockBusy error identifies the shard as an old shard to the MigrationManager, the second
// indicates the lock is held elsewhere and unavailable.
TEST_F(MigrationManagerTest, SameOldShardFailsToAcquireDistributedLockTwice) {
    // Set up a shard in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    std::string dbName = "foo";
    std::string collName = "foo.bar";
    ChunkVersion version(2, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up a chunk in the metadata.
    ChunkType chunk1 =
        setUpChunk(collName, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    // Going to request that this chunk get migrated.
    const MigrationRequestVector migrationRequests{MigrationRequest(
        MigrateInfo(chunk1.getNS(), kShardId1, chunk1), 0, kDefaultSecondaryThrottle, false)};

    auto future = launchAsync([this, migrationRequests] {
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding a host to which to send the command.
        // Set up a dummy host for the source shard.
        shardTargeterMock(txn.get(), kShardId0)->setFindHostReturnValue(kShardHost0);

        MigrationStatuses migrationStatuses =
            _migrationManager->scheduleMigrations(txn.get(), migrationRequests);

        for (const auto& migrateInfo : migrationRequests) {
            ASSERT_EQ(ErrorCodes::LockBusy,
                      migrationStatuses.at(migrateInfo.migrateInfo.getName()));
        }
    });

    // Expect two sequential moveChunk commands to the same shard, both of which fail with LockBusy.
    expectMoveChunkCommand(
        chunk1,
        kShardId1,
        false,
        Status(ErrorCodes::LockBusy, "SameCollectionOldShardMigrations generated error."));
    expectMoveChunkCommand(
        chunk1,
        kShardId1,
        true,
        Status(ErrorCodes::LockBusy, "SameCollectionOldShardMigrations generated error."));

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
}

// If in the same collection a migration is scheduled with an old v3.2 shard, a second migration in
// the collection with a different old v3.2 shard should get rescheduled.
TEST_F(MigrationManagerTest, SameCollectionTwoOldShardMigrations) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    std::string dbName = "foo";
    std::string collName = "foo.bar";
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
    const MigrationRequestVector migrationRequests{
        MigrationRequest(
            MigrateInfo(chunk1.getNS(), kShardId1, chunk1), 0, kDefaultSecondaryThrottle, false),
        MigrationRequest(
            MigrateInfo(chunk2.getNS(), kShardId3, chunk2), 0, kDefaultSecondaryThrottle, false)};

    auto future = launchAsync([this, migrationRequests] {
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding a host to which to send the command.
        // Set up dummy hosts for the source shards.
        shardTargeterMock(txn.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(txn.get(), kShardId2)->setFindHostReturnValue(kShardHost2);

        MigrationStatuses migrationStatuses =
            _migrationManager->scheduleMigrations(txn.get(), migrationRequests);

        for (const auto& migrateInfo : migrationRequests) {
            ASSERT_OK(migrationStatuses.at(migrateInfo.migrateInfo.getName()));
        }
    });

    // Expect two failed moveChunk commands, then two successful moveChunk commands after the
    // balancer releases the distributed lock.
    expectMoveChunkCommand(
        chunk1,
        kShardId1,
        false,
        Status(ErrorCodes::LockBusy, "SameCollectionOldShardMigration generated error."));
    expectMoveChunkCommand(
        chunk2,
        kShardId3,
        false,
        Status(ErrorCodes::LockBusy, "SameCollectionOldShardMigration generated error."));
    expectMoveChunkCommand(chunk1, kShardId1, true, Status::OK());
    expectMoveChunkCommand(chunk2, kShardId3, true, Status::OK());

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
}

// Takes the distributed lock for a collection so that that the MigrationManager is unable to
// schedule migrations for that collection.
TEST_F(MigrationManagerTest, FailToAcquireDistributedLock) {
    // Set up two shards in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard2, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    std::string dbName = "foo";
    std::string collName = "foo.bar";
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
    const MigrationRequestVector migrationRequests{
        MigrationRequest(
            MigrateInfo(chunk1.getNS(), kShardId1, chunk1), 0, kDefaultSecondaryThrottle, false),
        MigrationRequest(
            MigrateInfo(chunk2.getNS(), kShardId3, chunk2), 0, kDefaultSecondaryThrottle, false)};

    // Take the distributed lock for the collection before scheduling via the MigrationManager.
    const std::string whyMessage("FailToAcquireDistributedLock unit-test taking distributed lock");
    DistLockManager::ScopedDistLock distLockStatus = unittest::assertGet(
        catalogClient()->distLock(operationContext(), chunk1.getNS(), whyMessage));

    MigrationStatuses migrationStatuses =
        _migrationManager->scheduleMigrations(operationContext(), migrationRequests);

    for (const auto& migrateInfo : migrationRequests) {
        ASSERT_EQ(ErrorCodes::LockBusy, migrationStatuses.at(migrateInfo.migrateInfo.getName()));
    }
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
    std::string dbName = "foo";
    std::string collName = "foo.bar";
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
    const MigrationRequestVector migrationRequests{
        MigrationRequest(
            MigrateInfo(chunk1.getNS(), kShardId1, chunk1), 0, kDefaultSecondaryThrottle, false),
        MigrationRequest(
            MigrateInfo(chunk2.getNS(), kShardId3, chunk2), 0, kDefaultSecondaryThrottle, false)};

    auto future = launchAsync([this, chunk1, chunk2, migrationRequests] {
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        // Scheduling a moveChunk command requires finding a host to which to send the command. Set
        // up a dummy host for kShardHost0, and return an error for kShardHost3.
        shardTargeterMock(txn.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(txn.get(), kShardId2)
            ->setFindHostReturnValue(
                Status(ErrorCodes::ReplicaSetNotFound, "SourceShardNotFound generated error."));

        MigrationStatuses migrationStatuses =
            _migrationManager->scheduleMigrations(txn.get(), migrationRequests);

        ASSERT_OK(migrationStatuses.at(chunk1.getName()));
        ASSERT_EQ(ErrorCodes::ReplicaSetNotFound, migrationStatuses.at(chunk2.getName()));
    });

    // Expect only one moveChunk command to be called.
    expectMoveChunkCommand(chunk1, kShardId1, false, Status::OK());

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
}

TEST_F(MigrationManagerTest, JumboChunkResponseBackwardsCompatibility) {
    // Set up one shard in the metadata.
    ASSERT_OK(catalogClient()->insertConfigDocument(
        operationContext(), ShardType::ConfigNS, kShard0, kMajorityWriteConcern));

    // Set up the database and collection as sharded in the metadata.
    std::string dbName = "foo";
    std::string collName = "foo.bar";
    ChunkVersion version(2, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up a single chunk in the metadata.
    ChunkType chunk1 =
        setUpChunk(collName, kKeyPattern.globalMin(), kKeyPattern.globalMax(), kShardId0, version);

    // Going to request that this chunk gets migrated.
    const MigrationRequestVector migrationRequests{MigrationRequest(
        MigrateInfo(chunk1.getNS(), kShardId1, chunk1), 0, kDefaultSecondaryThrottle, false)};

    auto future = launchAsync([this, chunk1, migrationRequests] {
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        // Scheduling a moveChunk command requires finding a host to which to send the command. Set
        // up a dummy host for kShardHost0.
        shardTargeterMock(txn.get(), kShardId0)->setFindHostReturnValue(kShardHost0);

        MigrationStatuses migrationStatuses =
            _migrationManager->scheduleMigrations(txn.get(), migrationRequests);

        ASSERT_EQ(ErrorCodes::ChunkTooBig, migrationStatuses.at(chunk1.getName()));
    });

    // Expect only one moveChunk command to be called.
    expectMoveChunkCommand(chunk1, kShardId1, false, BSON("ok" << 0 << "chunkTooBig" << true));

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
