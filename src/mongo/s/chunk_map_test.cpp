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

private:
    KeyPattern _shardKeyPattern{BSON("a" << 1)};
};

}  // namespace

TEST_F(ChunkMapTest, TestAddChunk) {
    const OID epoch = OID::gen();
    ChunkVersion version{1, 0, epoch, boost::none /* timestamp */};

    auto chunk = std::make_shared<ChunkInfo>(
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  version,
                  kThisShard});

    /* test full chunks to chunkMap */
    ChunkMap oldChunkMap{epoch, boost::none /* timestamp */, 5};
    ChunkMap fullChunkMap{epoch, boost::none /* timestamp */, 5};
    fullChunkMap.createMerged(oldChunkMap, {chunk}, kNss, false);
    ASSERT_EQ(fullChunkMap.size(), 1);

    /* test update chunks to chunkMap */
    ChunkVersion newVersion{2, 0, epoch, boost::none /* timestamp */};
    auto vectorPtr = std::make_shared<ChunkVector>();
    auto updateChunk1 = std::make_shared<ChunkInfo>(
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 100)},
                  newVersion,
                  kThisShard});
    auto updateChunk2 = std::make_shared<ChunkInfo>(
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                  newVersion,
                  kThisShard});

    ChunkMap updatedChunkMap(fullChunkMap.getChunkMap(), fullChunkMap.getVersion(), 
        fullChunkMap.getShardVersion(), fullChunkMap.getVersion().getTimestamp(), 5);
    vectorPtr->push_back(updateChunk1);
    vectorPtr->push_back(updateChunk2);
    updatedChunkMap.createMerged(fullChunkMap, *vectorPtr, kNss, true);
    ASSERT_EQ(updatedChunkMap.size(), 2);
}


TEST_F(ChunkMapTest, TestEnumerateAllChunks) {
    const OID epoch = OID::gen();
    ChunkMap chunkMap{epoch, boost::none /* timestamp */, 5};
    ChunkVersion version{1, 0, epoch, boost::none /* timestamp */};

    ChunkMap oldChunkMap{epoch, boost::none /* timestamp */, 5};
    ChunkMap fullChunkMap{epoch, boost::none /* timestamp */, 5};

    /* test full chunks to chunkMap */
    fullChunkMap.createMerged(oldChunkMap,
        {std::make_shared<ChunkInfo>(
             ChunkType{kNss,
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       kThisShard}), 

         std::make_shared<ChunkInfo>(
             ChunkType{kNss, ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, kThisShard}),

         std::make_shared<ChunkInfo>(
            ChunkType{
                     kNss,
                     ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                     version,
                     kThisShard})}, 
             kNss, false);

    int count = 0;
    auto lastMax = getShardKeyPattern().globalMin();

    fullChunkMap.forEach([&](const auto& chunkInfo) {
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(chunkInfo->getMax() > lastMax));
        lastMax = chunkInfo->getMax();
        count++;

        return true;
    });

    ASSERT_EQ(count, fullChunkMap.size());
}


TEST_F(ChunkMapTest, TestAddAndSplitChunkVectorAndMergeChunkVector) {
    const OID epoch = OID::gen();
    ChunkVersion version{1, 0, epoch, boost::none /* timestamp */};

    auto chunk = std::make_shared<ChunkInfo>(
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  version,
                  kThisShard});

    /* test full chunks to chunkMap */
    ChunkMap oldChunkMap{epoch, boost::none /* timestamp */, 5};
    ChunkMap fullChunkMap{epoch, boost::none /* timestamp */, 5};
    fullChunkMap.createMerged(oldChunkMap, {chunk}, kNss,false);
    ASSERT_EQ(fullChunkMap.size(), 1);

    /* test update chunks to chunkMap and vector chunk split*/
    ChunkVersion newVersion{2, 0, epoch, boost::none /* timestamp */};
    ChunkMap splitChunkMap(fullChunkMap.getChunkMap(), fullChunkMap.getVersion(), 
        fullChunkMap.getShardVersion(), fullChunkMap.getVersion().getTimestamp(), 5);
    splitChunkMap.createMerged(fullChunkMap,
        {std::make_shared<ChunkInfo>(
             ChunkType{kNss,
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       newVersion,
                       kThisShard}),

        std::make_shared<ChunkInfo>(
             ChunkType{kNss, ChunkRange{BSON("a" << 0), BSON("a" << 100)}, newVersion, kThisShard}),

        std::make_shared<ChunkInfo>(
               ChunkType{kNss, ChunkRange{BSON("a" << 100), BSON("a" << 200)}, newVersion, kThisShard}),
               
        std::make_shared<ChunkInfo>(
               ChunkType{kNss, ChunkRange{BSON("a" << 200), BSON("a" << 300)}, newVersion, kThisShard}),

        std::make_shared<ChunkInfo>(
             ChunkType{kNss, ChunkRange{BSON("a" << 300), BSON("a" << 400)}, newVersion, kThisShard}),

        std::make_shared<ChunkInfo>(
               ChunkType{kNss, ChunkRange{BSON("a" << 400), BSON("a" << 500)}, newVersion, kThisShard}),
               
        std::make_shared<ChunkInfo>(
               ChunkType{kNss, ChunkRange{BSON("a" << 500), BSON("a" << 600)}, newVersion, kThisShard}),

         std::make_shared<ChunkInfo>(
            ChunkType{
                     kNss,
                     ChunkRange{BSON("a" << 600), getShardKeyPattern().globalMax()},
                     newVersion,
                     kThisShard})}, 
        kNss,true);
        
     ASSERT_EQ(splitChunkMap.getChunkMap().size(), 2);
     ASSERT_EQ(splitChunkMap.size(), 8);

     /* test full chunks to chunkMap and vector chunk merge*/

     ChunkVersion newVersionForMerge{3, 0, epoch, boost::none /* timestamp */};
//   ChunkMap mergeChunkMap{epoch, boost::none /* timestamp */, 5};
     ChunkMap mergeChunkMap(splitChunkMap.getChunkMap(), splitChunkMap.getVersion(), 
         splitChunkMap.getShardVersion(), splitChunkMap.getVersion().getTimestamp(), 5);
     mergeChunkMap.createMerged(splitChunkMap,
        {std::make_shared<ChunkInfo>(
             ChunkType{kNss,
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       newVersionForMerge,
                       kThisShard}),
        
         std::make_shared<ChunkInfo>(
              ChunkType{kNss, ChunkRange{BSON("a" << 0), BSON("a" << 600)}, newVersion, kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{
                     kNss,
                     ChunkRange{BSON("a" << 600), getShardKeyPattern().globalMax()},
                     newVersionForMerge,
                     kThisShard})}, 
        kNss, true);
             
     ASSERT_EQ(mergeChunkMap.getChunkMap().size(), 1);
     ASSERT_EQ(mergeChunkMap.size(), 3); 
}


TEST_F(ChunkMapTest, TestIntersectingChunk) {
    const OID epoch = OID::gen();
    ChunkVersion version{1, 0, epoch, boost::none /* timestamp */};

    ChunkMap oldChunkMap{epoch, boost::none /* timestamp */, 5};
    ChunkMap updatedChunkMap{epoch, boost::none /* timestamp */, 5};

    updatedChunkMap.createMerged(oldChunkMap, 
        {std::make_shared<ChunkInfo>(
             ChunkType{kNss,
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{kNss, ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, kThisShard}),

         std::make_shared<ChunkInfo>(
            ChunkType{
                     kNss,
                     ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                     version,
                     kThisShard})}, 
         kNss, false);

    auto intersectingChunk = updatedChunkMap.findIntersectingChunk(BSON("a" << 50));

    ASSERT(intersectingChunk);
    ASSERT(
        SimpleBSONObjComparator::kInstance.evaluate(intersectingChunk->getMin() == BSON("a" << 0)));
    ASSERT(SimpleBSONObjComparator::kInstance.evaluate(intersectingChunk->getMax() ==
                                                       BSON("a" << 100)));
}

TEST_F(ChunkMapTest, TestEnumerateOverlappingChunks) {
    const OID epoch = OID::gen();
    ChunkVersion version{1, 0, epoch, boost::none /* timestamp */};

    ChunkMap oldChunkMap{epoch, boost::none /* timestamp */, 5};
    ChunkMap updatedChunkMap{epoch, boost::none /* timestamp */, 5};
    updatedChunkMap.createMerged(oldChunkMap, 
        {std::make_shared<ChunkInfo>(
             ChunkType{kNss,
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{kNss, ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{
                 kNss,
                 ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                 version,
                 kThisShard})}, 
         kNss, false);

    auto min = BSON("a" << -50);
    auto max = BSON("a" << 150);

    int count = 0;
    updatedChunkMap.forEachOverlappingChunk(min, max, true, [&](const auto& chunk) {
        count++;
        return true;
    });

    ASSERT_EQ(count, 3);
}

}  // namespace mongo
