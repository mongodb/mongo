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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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
     * Setup the config.shards collection to contain the given shards.
     * Additionally set up dummy hosts for the targeted shards
     */
    void setupShards(const std::vector<ShardType>& shards) override {
        ConfigServerTestFixture::setupShards(shards);

        // Requests chunks to be relocated requires running commands on each shard to
        // get shard statistics. Set up dummy hosts for the source shards.
        for (const auto& shard : shards) {
            shardTargeterMock(operationContext(), shard.getName())
                ->setFindHostReturnValue(HostAndPort(shard.getHost()));
        }
    }

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

    CollectionType setUpUnsplittableCollection(const NamespaceString& collName,
                                               const ShardId& shardId,
                                               boost::optional<const UUID> collUUID = boost::none);

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
     * Returns the ShardId by its HostAndPort
     */
    ShardId getShardIdByHost(HostAndPort host);

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

    const ShardType kShard0{kShardId0.toString(), kShardHost0.toString()};
    const ShardType kShard1{kShardId1.toString(), kShardHost1.toString()};
    const ShardType kShard2{kShardId2.toString(), kShardHost2.toString()};
    const ShardType kShard3{kShardId3.toString(), kShardHost3.toString()};

    const std::string kPattern = "_id";
    const KeyPattern kKeyPattern = KeyPattern(BSON(kPattern << 1));

    const WriteConcernOptions kMajorityWriteConcern = WriteConcernOptions(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(15));
};

}  // namespace mongo
