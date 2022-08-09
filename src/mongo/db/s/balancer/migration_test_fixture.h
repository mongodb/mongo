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

#include <memory>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/type_collection_common_types_gen.h"

namespace mongo {

class MigrationTestFixture : public ConfigServerTestFixture {
protected:
    explicit MigrationTestFixture(Options options = {})
        : ConfigServerTestFixture(std::move(options)) {}

    void setUp() override;

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
    void setUpDatabase(const std::string& dbName, ShardId primaryShard);

    /**
     * Inserts a document into the config.collections collection to indicate that "collName" is
     * sharded with version "version". The shard key pattern defaults to "_id".
     */
    void setUpCollection(
        const NamespaceString& collName,
        const UUID& collUUID,
        const ChunkVersion& version,
        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields = boost::none);

    /**
     * Inserts a document into the config.chunks collection so that the chunk defined by the
     * parameters exists. Returns a ChunkType defined by the parameters.
     */
    ChunkType setUpChunk(const UUID& collUUID,
                         const BSONObj& chunkMin,
                         const BSONObj& chunkMax,
                         const ShardId& shardId,
                         const ChunkVersion& version);

    /**
     * Inserts a document into the config.tags collection so that the zone defined by the
     * parameters exists.
     */
    void setUpZones(const NamespaceString& collName, const StringMap<ChunkRange>& zoneChunkRanges);

    /**
     * Removes all document in the config.tags for the collection.
     */
    void removeAllZones(const NamespaceString& collName);

    /**
     * Removes all document in the config.chunks for the collection.
     */
    void removeAllChunks(const NamespaceString& collName, const UUID& uuid);

    /**
     * Inserts a document into the config.migrations collection as an active migration.
     */
    void setUpMigration(const NamespaceString& ns, const ChunkType& chunk, const ShardId& toShard);

    /**
     * Asserts that config.migrations is empty, that should be true if the MigrationManager is
     * inactive and behaving properly.
     */
    void checkMigrationsCollectionIsEmpty();

    // Random static initialization order can result in X constructor running before Y constructor
    // if X and Y are defined in different source files. Defining variables here to enforce order.
    const ShardId kShardId0 = ShardId("shard0");
    const ShardId kShardId1 = ShardId("shard1");
    const ShardId kShardId2 = ShardId("shard2");
    const ShardId kShardId3 = ShardId("shard3");

    const HostAndPort kShardHost0 = HostAndPort("TestHost0", 12345);
    const HostAndPort kShardHost1 = HostAndPort("TestHost1", 12346);
    const HostAndPort kShardHost2 = HostAndPort("TestHost2", 12347);
    const HostAndPort kShardHost3 = HostAndPort("TestHost3", 12348);

    const long long kMaxSizeMB = 100;

    const BSONObj kShard0 =
        BSON(ShardType::name(kShardId0.toString())
             << ShardType::host(kShardHost0.toString()) << ShardType::maxSizeMB(kMaxSizeMB));
    const BSONObj kShard1 =
        BSON(ShardType::name(kShardId1.toString())
             << ShardType::host(kShardHost1.toString()) << ShardType::maxSizeMB(kMaxSizeMB));
    const BSONObj kShard2 =
        BSON(ShardType::name(kShardId2.toString())
             << ShardType::host(kShardHost2.toString()) << ShardType::maxSizeMB(kMaxSizeMB));
    const BSONObj kShard3 =
        BSON(ShardType::name(kShardId3.toString())
             << ShardType::host(kShardHost3.toString()) << ShardType::maxSizeMB(kMaxSizeMB));

    const std::string kPattern = "_id";
    const KeyPattern kKeyPattern = KeyPattern(BSON(kPattern << 1));

    const WriteConcernOptions kMajorityWriteConcern = WriteConcernOptions(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(15));
};

}  // namespace mongo
