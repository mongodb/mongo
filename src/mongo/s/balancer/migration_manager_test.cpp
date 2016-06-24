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

const auto kShardId0 = ShardId("shard0");
const auto kShardId1 = ShardId("shard1");
const auto kShardId2 = ShardId("shard2");
const auto kShardId3 = ShardId("shard3");

const HostAndPort kShardHost0 = HostAndPort("TestHost0", 12345);
const HostAndPort kShardHost1 = HostAndPort("TestHost1", 12346);
const HostAndPort kShardHost2 = HostAndPort("TestHost2", 12347);
const HostAndPort kShardHost3 = HostAndPort("TestHost3", 12348);

const long long kMaxSizeMB = 100;
const std::string kPattern = "_id";

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(15));

class MigrationManagerTest : public ConfigServerTestFixture {
protected:
    std::unique_ptr<MigrationManager> _migrationManager;

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
                         const ShardId& fromShardId,
                         const ShardId& toShardId,
                         const ChunkVersion& version);

    /**
     * Sets up mock network to expect a moveChunk command and return "returnStatus".
     */
    void expectMoveChunkCommand(const ChunkType& chunk,
                                const ShardId& toShardId,
                                const bool& takeDistLock,
                                const Status& returnStatus);

private:
    void setUp() override;
    void tearDown() override;

public:
    // Random static initialization order can result in X constructor running before Y constructor
    // if X and Y are defined in different source files. Defining variables here to enforce order.
    const BSONObj shardType0 =
        BSON(ShardType::name(kShardId0.toString()) << ShardType::host(kShardHost0.toString())
                                                   << ShardType::maxSizeMB(kMaxSizeMB));
    const BSONObj shardType1 =
        BSON(ShardType::name(kShardId1.toString()) << ShardType::host(kShardHost1.toString())
                                                   << ShardType::maxSizeMB(kMaxSizeMB));
    const BSONObj shardType2 =
        BSON(ShardType::name(kShardId2.toString()) << ShardType::host(kShardHost2.toString())
                                                   << ShardType::maxSizeMB(kMaxSizeMB));
    const BSONObj shardType3 =
        BSON(ShardType::name(kShardId3.toString()) << ShardType::host(kShardHost3.toString())
                                                   << ShardType::maxSizeMB(kMaxSizeMB));
    const KeyPattern kKeyPattern = KeyPattern(BSON(kPattern << 1));
};

void MigrationManagerTest::setUp() {
    _migrationManager = stdx::make_unique<MigrationManager>();
    ConfigServerTestFixture::setUp();
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
    ASSERT(catalogClient()
               ->insertConfigDocument(
                   operationContext(), DatabaseType::ConfigNS, db.toBSON(), kMajorityWriteConcern)
               .isOK());
}

void MigrationManagerTest::setUpCollection(const std::string collName, ChunkVersion version) {
    CollectionType coll;
    coll.setNs(NamespaceString(collName));
    coll.setEpoch(version.epoch());
    coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(version.toLong()));
    coll.setKeyPattern(kKeyPattern);
    coll.setUnique(false);
    ASSERT(
        catalogClient()
            ->insertConfigDocument(
                operationContext(), CollectionType::ConfigNS, coll.toBSON(), kMajorityWriteConcern)
            .isOK());
}

ChunkType MigrationManagerTest::setUpChunk(const std::string& collName,
                                           const BSONObj& chunkMin,
                                           const BSONObj& chunkMax,
                                           const ShardId& fromShardId,
                                           const ShardId& toShardId,
                                           const ChunkVersion& version) {
    ChunkType chunk;
    chunk.setNS(collName);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);
    chunk.setShard(fromShardId);
    chunk.setVersion(version);
    ASSERT(catalogClient()
               ->insertConfigDocument(
                   operationContext(), ChunkType::ConfigNS, chunk.toBSON(), kMajorityWriteConcern)
               .isOK());
    return chunk;
}

void MigrationManagerTest::expectMoveChunkCommand(const ChunkType& chunk,
                                                  const ShardId& toShardId,
                                                  const bool& takeDistLock,
                                                  const Status& returnStatus) {
    onCommand(
        [&chunk, &toShardId, &takeDistLock, &returnStatus](const RemoteCommandRequest& request) {
            NamespaceString nss(request.cmdObj.firstElement().valueStringData());
            ASSERT_EQ(chunk.getNS(), nss.ns());

            const StatusWith<MoveChunkRequest> moveChunkRequestWithStatus =
                MoveChunkRequest::createFromCommand(nss, request.cmdObj);
            ASSERT(moveChunkRequestWithStatus.isOK());

            ASSERT_EQ(chunk.getNS(), moveChunkRequestWithStatus.getValue().getNss().ns());
            ASSERT_EQ(chunk.getMin(), moveChunkRequestWithStatus.getValue().getMinKey());
            ASSERT_EQ(chunk.getMax(), moveChunkRequestWithStatus.getValue().getMaxKey());
            ASSERT_EQ(chunk.getShard(), moveChunkRequestWithStatus.getValue().getFromShardId());

            ASSERT_EQ(toShardId, moveChunkRequestWithStatus.getValue().getToShardId());
            ASSERT_EQ(takeDistLock, moveChunkRequestWithStatus.getValue().getTakeDistLock());

            if (returnStatus.isOK()) {
                return BSON("ok" << 1);
            } else {
                BSONObjBuilder builder;
                builder.append("ok", 0);
                builder.append("code", returnStatus.code());
                builder.append("errmsg", returnStatus.reason());
                return builder.obj();
            }
        });
}

TEST_F(MigrationManagerTest, TwoSameCollectionMigrations) {
    // Set up two shards in the metadata.
    ASSERT(catalogClient()
               ->insertConfigDocument(
                   operationContext(), ShardType::ConfigNS, shardType0, kMajorityWriteConcern)
               .isOK());
    ASSERT(catalogClient()
               ->insertConfigDocument(
                   operationContext(), ShardType::ConfigNS, shardType2, kMajorityWriteConcern)
               .isOK());

    // Set up the database and collection as sharded in the metadata.
    std::string dbName = "foo";
    std::string collName = "foo.bar";
    ChunkVersion version(2, 0, OID::gen());

    setUpDatabase(dbName, kShardId0);
    setUpCollection(collName, version);

    // Set up two chunks in the metadata.
    BalancerChunkSelectionPolicy::MigrateInfoVector candidateChunks;
    ChunkType chunk1 = setUpChunk(
        collName, kKeyPattern.globalMin(), BSON(kPattern << 49), kShardId0, kShardId1, version);
    version.incMinor();
    ChunkType chunk2 = setUpChunk(
        collName, BSON(kPattern << 49), kKeyPattern.globalMax(), kShardId2, kShardId3, version);

    // Going to request that these two chunks get migrated.
    candidateChunks.push_back(MigrateInfo(chunk1.getNS(), kShardId1, chunk1));
    candidateChunks.push_back(MigrateInfo(chunk2.getNS(), kShardId3, chunk2));

    auto future = launchAsync([this, candidateChunks] {
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        // Scheduling the moveChunk commands requires finding a host to which to send the command.
        // Set up some dummy hosts -- moveChunk commands are going to hit the mock network anyway.
        shardTargeterMock(txn.get(), kShardId0)->setFindHostReturnValue(kShardHost0);
        shardTargeterMock(txn.get(), kShardId2)->setFindHostReturnValue(kShardHost3);

        _migrationManager->scheduleMigrations(txn.get(), candidateChunks);
    });

    // Expect two moveChunk commands.
    expectMoveChunkCommand(chunk1, kShardId1, false, Status::OK());
    expectMoveChunkCommand(chunk2, kShardId3, false, Status::OK());

    // Run the MigrationManager code.
    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
