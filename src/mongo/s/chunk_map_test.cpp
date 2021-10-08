/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/s/chunk_manager.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

const NamespaceString kNss("TestDB", "TestColl");
const ShardId kThisShard("testShard");

class ChunkMapTest : public unittest::Test {
public:
    const KeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const UUID& uuid() const {
        return _uuid;
    }

private:
    KeyPattern _shardKeyPattern{BSON("a" << 1)};
    const UUID _uuid = UUID::gen();
};

}  // namespace

TEST_F(ChunkMapTest, TestAddChunk) {
    const OID epoch = OID::gen();
    ChunkVersion version{1, 0, epoch, Timestamp(1, 1)};

    auto chunk = std::make_shared<ChunkInfo>(
        ChunkType{uuid(),
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  version,
                  kThisShard});

    ChunkMap chunkMap{epoch, Timestamp(1, 1)};
    auto newChunkMap = chunkMap.createMerged({chunk});

    ASSERT_EQ(newChunkMap.size(), 1);
}

TEST_F(ChunkMapTest, TestEnumerateAllChunks) {
    const OID epoch = OID::gen();
    ChunkMap chunkMap{epoch, Timestamp(1, 1)};
    ChunkVersion version{1, 0, epoch, Timestamp(1, 1)};

    auto newChunkMap = chunkMap.createMerged(
        {std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(), ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                       version,
                       kThisShard})});

    int count = 0;
    auto lastMax = getShardKeyPattern().globalMin();

    newChunkMap.forEach([&](const auto& chunkInfo) {
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(chunkInfo->getMax() > lastMax));
        lastMax = chunkInfo->getMax();
        count++;

        return true;
    });

    ASSERT_EQ(count, newChunkMap.size());
}

TEST_F(ChunkMapTest, TestIntersectingChunk) {
    const OID epoch = OID::gen();
    ChunkMap chunkMap{epoch, Timestamp(1, 1)};
    ChunkVersion version{1, 0, epoch, Timestamp(1, 1)};

    auto newChunkMap = chunkMap.createMerged(
        {std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(), ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                       version,
                       kThisShard})});

    auto intersectingChunk = newChunkMap.findIntersectingChunk(BSON("a" << 50));

    ASSERT(intersectingChunk);
    ASSERT(
        SimpleBSONObjComparator::kInstance.evaluate(intersectingChunk->getMin() == BSON("a" << 0)));
    ASSERT(SimpleBSONObjComparator::kInstance.evaluate(intersectingChunk->getMax() ==
                                                       BSON("a" << 100)));
}

TEST_F(ChunkMapTest, TestEnumerateOverlappingChunks) {
    const OID epoch = OID::gen();
    ChunkMap chunkMap{epoch, Timestamp(1, 1)};
    ChunkVersion version{1, 0, epoch, Timestamp(1, 1)};

    auto newChunkMap = chunkMap.createMerged(
        {std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(), ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                       version,
                       kThisShard})});

    auto min = BSON("a" << -50);
    auto max = BSON("a" << 150);

    int count = 0;
    newChunkMap.forEachOverlappingChunk(min, max, true, [&](const auto& chunk) {
        count++;
        return true;
    });

    ASSERT_EQ(count, 3);
}

}  // namespace mongo
