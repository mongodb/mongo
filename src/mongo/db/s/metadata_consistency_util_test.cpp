/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/db/s/metadata_consistency_util.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"


namespace mongo {
namespace {

ChunkType generateChunk(const NamespaceString& nss,
                        const UUID& collUuid,
                        const ShardId& shardId,
                        const BSONObj& minKey,
                        const BSONObj& maxKey,
                        const std::vector<ChunkHistory>& history) {
    const OID epoch = OID::gen();
    ChunkType chunkType;
    chunkType.setName(OID::gen());
    chunkType.setCollectionUUID(collUuid);
    chunkType.setVersion(ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}));
    chunkType.setShard(shardId);
    chunkType.setMin(minKey);
    chunkType.setMax(maxKey);
    chunkType.setOnCurrentShardSince(Timestamp(1, 0));
    chunkType.setHistory(history);
    return chunkType;
}

class MetadataConsistencyTest : public ShardServerTestFixture {
protected:
    std::string _shardName = "shard0000";
    std::string _config = "config";
    const ShardId _shardId{_shardName};
    const NamespaceString _nss{"TestDB", "TestColl"};
    const UUID _collUuid = UUID::gen();
    const KeyPattern _keyPattern{BSON("x" << 1)};
    const CollectionType _coll{
        _nss, OID::gen(), Timestamp(1), Date_t::now(), _collUuid, _keyPattern};

    void assertOneInconsistencyFound(
        const MetadataInconsistencyTypeEnum& type,
        const NamespaceString& nss,
        const ShardId& shard,
        const std::vector<MetadataInconsistencyItem>& inconsistencies) {
        ASSERT_EQ(1, inconsistencies.size());
        ASSERT_EQ(type, inconsistencies[0].getType());
    }
};

TEST_F(MetadataConsistencyTest, FindRoutingTableRangeGapInconsistency) {
    const auto chunk1 = generateChunk(_nss,
                                      _collUuid,
                                      _shardId,
                                      _keyPattern.globalMin(),
                                      BSON("x" << 0),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto chunk2 = generateChunk(_nss,
                                      _collUuid,
                                      _shardId,
                                      BSON("x" << 1),
                                      _keyPattern.globalMax(),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto inconsistencies = metadata_consistency_util::checkChunksInconsistencies(
        operationContext(), _coll, {chunk1, chunk2});

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kRoutingTableRangeGap, _nss, _config, inconsistencies);
}

TEST_F(MetadataConsistencyTest, FindMissingChunkWithMaxKeyInconsistency) {
    const auto chunk = generateChunk(_nss,
                                     _collUuid,
                                     _shardId,
                                     _keyPattern.globalMin(),
                                     BSON("x" << 0),
                                     {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksInconsistencies(operationContext(), _coll, {chunk});

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kRoutingTableMissingMaxKey, _nss, _config, inconsistencies);
}

TEST_F(MetadataConsistencyTest, FindMissingChunkWithMinKeyInconsistency) {
    const auto chunk = generateChunk(_nss,
                                     _collUuid,
                                     _shardId,
                                     BSON("x" << 0),
                                     _keyPattern.globalMax(),
                                     {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksInconsistencies(operationContext(), _coll, {chunk});

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kRoutingTableMissingMinKey, _nss, _config, inconsistencies);
}

TEST_F(MetadataConsistencyTest, FindRoutingTableRangeOverlapInconsistency) {
    const auto chunk1 = generateChunk(_nss,
                                      _collUuid,
                                      _shardId,
                                      _keyPattern.globalMin(),
                                      BSON("x" << 0),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto chunk2 = generateChunk(_nss,
                                      _collUuid,
                                      _shardId,
                                      BSON("x" << -10),
                                      _keyPattern.globalMax(),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto inconsistencies = metadata_consistency_util::checkChunksInconsistencies(
        operationContext(), _coll, {chunk1, chunk2});

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kRoutingTableRangeOverlap, _nss, _config, inconsistencies);
}

TEST_F(MetadataConsistencyTest, FindCorruptedChunkShardKeyInconsistency) {
    const auto chunk1 = generateChunk(_nss,
                                      _collUuid,
                                      _shardId,
                                      _keyPattern.globalMin(),
                                      BSON("x" << 0),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto chunk2 = generateChunk(_nss,
                                      _collUuid,
                                      _shardId,
                                      BSON("y" << 0),
                                      _keyPattern.globalMax(),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto inconsistencies = metadata_consistency_util::checkChunksInconsistencies(
        operationContext(), _coll, {chunk1, chunk2});

    ASSERT_EQ(2, inconsistencies.size());
    ASSERT_EQ(MetadataInconsistencyTypeEnum::kCorruptedChunkShardKey, inconsistencies[0].getType());
    ASSERT_EQ(MetadataInconsistencyTypeEnum::kRoutingTableRangeGap, inconsistencies[1].getType());
}

class MetadataConsistencyRandomRoutingTableTest : public ShardServerTestFixture {
protected:
    const NamespaceString _nss{"TestDB", "TestColl"};
    const UUID _collUuid = UUID::gen();
    const KeyPattern _keyPattern{BSON("x" << 1)};
    const CollectionType _coll{
        _nss, OID::gen(), Timestamp(1), Date_t::now(), _collUuid, _keyPattern};
    inline const static auto _shards = std::vector<ShardId>{ShardId{"shard0"}, ShardId{"shard1"}};
    PseudoRandom _random{SecureRandom().nextInt64()};

    std::vector<ChunkType> getRandomRoutingTable() {
        int numChunks = _random.nextInt32(9) + 1;  // minimum 1 chunks, maximum 10 chunks
        std::vector<ChunkType> chunks;

        // Loop generating random routing table: [MinKey, 0), [1, 2), [2, 3), ... [x, MaxKey]
        int nextMin;
        for (int nextMax = 0; nextMax < numChunks; nextMax++) {
            auto randomShard = _shards.at(_random.nextInt64(_shards.size()));
            // set min as `MinKey` during first iteration, otherwise next min
            auto min = nextMax == 0 ? _keyPattern.globalMin() : BSON("x" << nextMin);
            // set max as `MaxKey` during last iteration, otherwise next max
            auto max = nextMax == numChunks - 1 ? _keyPattern.globalMax() : BSON("x" << nextMax);
            auto chunk = generateChunk(_nss,
                                       _collUuid,
                                       randomShard,
                                       min,
                                       max,
                                       {ChunkHistory(Timestamp(1, 0), randomShard)});
            nextMin = nextMax;
            chunks.push_back(chunk);
        }

        return chunks;
    };
};

/*
 * Test function to check the correct behaviour of finding range gaps inconsistencies with random
 * ranges. In order to introduce inconsistencies, a random number of chunks are removed from the the
 * routing table to create range gaps.
 */
TEST_F(MetadataConsistencyRandomRoutingTableTest, FindRoutingTableRangeGapInconsistency) {
    auto chunks = getRandomRoutingTable();

    // Check that there are no inconsistencies in the routing table
    auto inconsistencies =
        metadata_consistency_util::checkChunksInconsistencies(operationContext(), _coll, chunks);
    ASSERT_EQ(0, inconsistencies.size());

    // Remove randoms chunk from the routing table
    const auto kNumberOfChunksToRemove = _random.nextInt64(chunks.size()) + 1;
    for (int i = 0; i < kNumberOfChunksToRemove; i++) {
        const auto itChunkToRemove = _random.nextInt64(chunks.size());
        chunks.erase(chunks.begin() + itChunkToRemove);
    }

    inconsistencies =
        metadata_consistency_util::checkChunksInconsistencies(operationContext(), _coll, chunks);

    // Assert that there is at least one gap inconsistency
    try {
        ASSERT_LTE(1, inconsistencies.size());
        for (const auto& inconsistency : inconsistencies) {
            const auto type = inconsistency.getType();
            ASSERT_TRUE(type == MetadataInconsistencyTypeEnum::kRoutingTableRangeGap ||
                        type == MetadataInconsistencyTypeEnum::kRoutingTableMissingMinKey ||
                        type == MetadataInconsistencyTypeEnum::kRoutingTableMissingMaxKey ||
                        type == MetadataInconsistencyTypeEnum::kMissingRoutingTable);
        }
    } catch (...) {
        LOGV2_INFO(7424600,
                   "Expecting gap inconsistencies",
                   "numberOfInconsistencies"_attr = inconsistencies.size(),
                   "inconsistencies"_attr = inconsistencies);
        throw;
    }
}

/*
 * Test function to check the correct behaviour of finding range overlaps inconsistencies with
 * random ranges. In order to introduce inconsistencies, one chunk is randomly selected and its max
 * or min bound is set to a random bigger or lower value, respectively.
 */
TEST_F(MetadataConsistencyRandomRoutingTableTest, FindRoutingTableRangeOverlapInconsistency) {
    auto chunks = getRandomRoutingTable();

    // Check that there are no inconsistencies in the routing table
    auto inconsistencies =
        metadata_consistency_util::checkChunksInconsistencies(operationContext(), _coll, chunks);
    ASSERT_EQ(0, inconsistencies.size());

    // If there is only one chunk, we can't introduce an overlap
    if (chunks.size() == 1) {
        return;
    }

    const auto chunkIdx = static_cast<size_t>(_random.nextInt64(chunks.size()));
    auto& chunk = chunks.at(chunkIdx);

    auto overlapMax = [&]() {
        if (_random.nextInt64(10) == 0) {
            // With 1/10 probability, set min to MinKey
            chunk.setMax(_keyPattern.globalMax());
        } else {
            // Otherwise, set max to a random value bigger than actual
            auto max = chunk.getMax()["x"].numberInt();
            chunk.setMax(BSON("x" << max + _random.nextInt64(10) + 1));
        }
    };

    auto overlapMin = [&]() {
        if (_random.nextInt64(10) == 0) {
            // With 1/10 probability, set min to MinKey
            chunk.setMin(_keyPattern.globalMin());
        } else {
            // Otherwise, set min to a random value smaller than actual
            auto min = chunk.getMin()["x"].numberInt();
            chunk.setMin(BSON("x" << min - _random.nextInt64(10) - 1));
        }
    };

    if (chunkIdx == 0) {
        overlapMax();
    } else if (chunkIdx == (chunks.size() - 1)) {
        overlapMin();
    } else {
        // With 1/2 probability, overlap min or max
        if (_random.nextInt64(2) == 0) {
            overlapMin();
        } else {
            overlapMax();
        }
    }

    inconsistencies =
        metadata_consistency_util::checkChunksInconsistencies(operationContext(), _coll, chunks);

    // Assert that there is at least one overlap inconsistency
    try {
        ASSERT_LTE(1, inconsistencies.size());
        for (const auto& inconsistency : inconsistencies) {
            ASSERT_EQ(MetadataInconsistencyTypeEnum::kRoutingTableRangeOverlap,
                      inconsistency.getType());
        }
    } catch (...) {
        LOGV2_INFO(7424601,
                   "Expecting overlap inconsistencies",
                   "numberOfInconsistencies"_attr = inconsistencies.size(),
                   "inconsistencies"_attr = inconsistencies);
        throw;
    }
}

}  // namespace
}  // namespace mongo
