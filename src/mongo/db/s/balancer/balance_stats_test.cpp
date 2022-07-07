/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/oid.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/balance_stats.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/unittest/unittest.h"

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
                                                                {},     // collator
                                                                false,  // unique
                                                                _epoch,
                                                                _timestamp,   // timestamp
                                                                boost::none,  // time series fields
                                                                boost::none,  // resharding fields
                                                                boost::none,  // chunk size bytes
                                                                true,         // allowMigration
                                                                chunks);

        return ChunkManager(_shardPrimary,
                            _dbVersion,
                            RoutingTableHistoryValueHandle(std::make_shared<RoutingTableHistory>(
                                std::move(routingTableHistory))),
                            boost::none);
    }

private:
    const NamespaceString _nss{"foo.bar"};
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
