// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/balancer/balance_stats.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class BalanceStatsTest : public mongo::unittest::Test {
public:
    ChunkType makeChunk(const BSONObj& minKey, const BSONObj& maxKey, const ShardId& shard) {
        _nextVersion.incMinor();
        return ChunkType(_uuid, ChunkRange(minKey, maxKey), _nextVersion, shard);
    }

    ShardType makeShard(const std::string& name, std::vector<std::string> zones = {}) {
        return ShardType(name, name, zones);
    }

    ChunkManager makeRoutingInfo(const KeyPattern& shardKeyPattern,
                                 const std::vector<ChunkType>& chunks) {
        auto routingTableHistory = RoutingTableHistory::makeNew(_nss,
                                                                _uuid,  // UUID
                                                                shardKeyPattern,
                                                                false,  /* unsplittable */
                                                                {},     // collator
                                                                false,  // unique
                                                                _epoch,
                                                                _timestamp,   // timestamp
                                                                boost::none,  // time series fields
                                                                boost::none,  // resharding fields
                                                                true,         // allowMigration
                                                                chunks);

        return CurrentChunkManager(RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(routingTableHistory))));
    }

private:
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest("foo.bar");
    const UUID _uuid = UUID::gen();
    const OID _epoch{OID::gen()};
    const Timestamp _timestamp{Timestamp(1, 1)};
    const ShardId _shardPrimary{"dummyShardPrimary"};
    const DatabaseVersion _dbVersion{UUID::gen(), _timestamp};
    ChunkVersion _nextVersion{{_epoch, _timestamp}, {1, 0}};
};

TEST_F(BalanceStatsTest, SingleChunkNoZones) {
    std::vector<ShardType> shards;
    shards.push_back(makeShard("a"));

    std::vector<ChunkType> chunks;
    chunks.push_back(makeChunk(BSON("x" << MINKEY), BSON("x" << MAXKEY), ShardId("a")));

    auto routingInfo = makeRoutingInfo(KeyPattern(BSON("x" << 1)), chunks);
    auto imbalanceCount = getMaxChunkImbalanceCount(routingInfo, shards, {});
    ASSERT_EQ(0, imbalanceCount);
}

TEST_F(BalanceStatsTest, SingleShardHasMoreChunksNoZones) {
    std::vector<ShardType> shards;
    shards.push_back(makeShard("a"));
    shards.push_back(makeShard("b"));

    std::vector<ChunkType> chunks;
    chunks.push_back(makeChunk(BSON("x" << MINKEY), BSON("x" << 10), ShardId("a")));
    chunks.push_back(makeChunk(BSON("x" << 10), BSON("x" << 20), ShardId("b")));
    chunks.push_back(makeChunk(BSON("x" << 20), BSON("x" << 30), ShardId("b")));
    chunks.push_back(makeChunk(BSON("x" << 30), BSON("x" << MAXKEY), ShardId("b")));

    auto routingInfo = makeRoutingInfo(KeyPattern(BSON("x" << 1)), chunks);
    auto imbalanceCount = getMaxChunkImbalanceCount(routingInfo, shards, {});
    ASSERT_EQ(2, imbalanceCount);
}

TEST_F(BalanceStatsTest, BalancedChunkInShardButNotZones) {
    std::vector<ShardType> shards;
    shards.push_back(makeShard("a", {"zone1"}));
    shards.push_back(makeShard("b", {"zone1"}));

    std::vector<ChunkType> chunks;
    chunks.push_back(makeChunk(BSON("x" << MINKEY), BSON("x" << 10), ShardId("a")));
    chunks.push_back(makeChunk(BSON("x" << 10), BSON("x" << 20), ShardId("a")));
    chunks.push_back(makeChunk(BSON("x" << 20), BSON("x" << 30), ShardId("b")));
    chunks.push_back(makeChunk(BSON("x" << 30), BSON("x" << 40), ShardId("b")));
    chunks.push_back(makeChunk(BSON("x" << 40), BSON("x" << MAXKEY), ShardId("b")));

    ZoneInfo zoneInfo;
    uassertStatusOK(zoneInfo.addRangeToZone({BSON("x" << 10), BSON("x" << MAXKEY), "zone1"}));

    auto routingInfo = makeRoutingInfo(KeyPattern(BSON("x" << 1)), chunks);
    auto imbalanceCount = getMaxChunkImbalanceCount(routingInfo, shards, zoneInfo);

    // Chunk counts:
    // Default Zone: a: 1, b: 0
    // zone1: a: 1, b: 3

    ASSERT_EQ(2, imbalanceCount);
}

TEST_F(BalanceStatsTest, BalancedChunkInZonesButNotShards) {
    std::vector<ShardType> shards;
    shards.push_back(makeShard("a", {"zone1"}));
    shards.push_back(makeShard("b", {"zone2"}));
    shards.push_back(makeShard("c", {"zone2"}));

    std::vector<ChunkType> chunks;
    chunks.push_back(makeChunk(BSON("x" << MINKEY), BSON("x" << 10), ShardId("a")));
    chunks.push_back(makeChunk(BSON("x" << 10), BSON("x" << 20), ShardId("a")));
    chunks.push_back(makeChunk(BSON("x" << 20), BSON("x" << 30), ShardId("a")));
    chunks.push_back(makeChunk(BSON("x" << 30), BSON("x" << 40), ShardId("a")));
    chunks.push_back(makeChunk(BSON("x" << 40), BSON("x" << 50), ShardId("b")));
    chunks.push_back(makeChunk(BSON("x" << 50), BSON("x" << MAXKEY), ShardId("c")));

    ZoneInfo zoneInfo;
    uassertStatusOK(zoneInfo.addRangeToZone({BSON("x" << MINKEY), BSON("x" << 40), "zone1"}));
    uassertStatusOK(zoneInfo.addRangeToZone({BSON("x" << 40), BSON("x" << MAXKEY), "zone2"}));

    auto routingInfo = makeRoutingInfo(KeyPattern(BSON("x" << 1)), chunks);
    auto imbalanceCount = getMaxChunkImbalanceCount(routingInfo, shards, zoneInfo);

    // Chunk counts:
    // zone1: a: 4
    // zone2: b:1, c: 1

    ASSERT_EQ(0, imbalanceCount);
}

TEST_F(BalanceStatsTest, NoChunkInAShard) {
    std::vector<ShardType> shards;
    shards.push_back(makeShard("a", {"zone1"}));
    shards.push_back(makeShard("b", {"zone1"}));
    shards.push_back(makeShard("c"));

    std::vector<ChunkType> chunks;
    chunks.push_back(makeChunk(BSON("x" << MINKEY), BSON("x" << 10), ShardId("a")));
    chunks.push_back(makeChunk(BSON("x" << 10), BSON("x" << 20), ShardId("b")));
    chunks.push_back(makeChunk(BSON("x" << 20), BSON("x" << 30), ShardId("a")));
    chunks.push_back(makeChunk(BSON("x" << 30), BSON("x" << MAXKEY), ShardId("b")));

    ZoneInfo zoneInfo;
    uassertStatusOK(zoneInfo.addRangeToZone({BSON("x" << MINKEY), BSON("x" << 20), "zone1"}));

    auto routingInfo = makeRoutingInfo(KeyPattern(BSON("x" << 1)), chunks);
    auto imbalanceCount = getMaxChunkImbalanceCount(routingInfo, shards, zoneInfo);

    // Chunk counts:
    // default zone: a: 1, b: 1, c: 0
    // zone1: a: 1, b: 1

    ASSERT_EQ(1, imbalanceCount);
}

}  // namespace
}  // namespace mongo
