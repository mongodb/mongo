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

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_writes_tracker.h"
#include "mongo/s/shard_server_test_fixture.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo {

namespace {

const ShardId kThisShard("thisShard");
const NamespaceString kNss("TestDB", "TestColl");

/**
 * Creates a new routing table from the input routing table by inserting the chunks specified by
 * newChunkBoundaryPoints.  newChunkBoundaryPoints specifies a contiguous array of keys indicating
 * chunk boundaries to be inserted. As an example, if you want to split the range [0, 2] into chunks
 * [0, 1] and [1, 2], newChunkBoundaryPoints should be [0, 1, 2].
 */
std::shared_ptr<RoutingTableHistory> splitChunk(
    const std::shared_ptr<RoutingTableHistory>& rt,
    const std::vector<BSONObj>& newChunkBoundaryPoints) {

    // Convert the boundary points into chunk range objects, e.g. {0, 1, 2} ->
    // {{ChunkRange{0, 1}, ChunkRange{1, 2}}
    std::vector<ChunkRange> newChunkRanges;
    invariant(newChunkBoundaryPoints.size() > 1);
    for (size_t i = 0; i < newChunkBoundaryPoints.size() - 1; ++i) {
        newChunkRanges.emplace_back(newChunkBoundaryPoints[i], newChunkBoundaryPoints[i + 1]);
    }

    std::vector<ChunkType> newChunks;
    auto curVersion = rt->getVersion();

    for (const auto& range : newChunkRanges) {
        // Chunks must be inserted ordered by version
        curVersion.incMajor();
        newChunks.emplace_back(kNss, range, curVersion, kThisShard);
    }
    return rt->makeUpdated(newChunks);
}

/**
 * Gets a set of raw pointers to ChunkInfo objects in the specified range,
 */
std::set<ChunkInfo*> getChunksInRange(std::shared_ptr<RoutingTableHistory> rt,
                                      const BSONObj& min,
                                      const BSONObj& max) {
    auto chunksFromSplitIter = rt->overlappingRanges(min, max, false);

    std::set<ChunkInfo*> chunksFromSplit;
    std::transform(chunksFromSplitIter.first,
                   chunksFromSplitIter.second,
                   std::inserter(chunksFromSplit, chunksFromSplit.begin()),
                   [](const std::pair<std::string, std::shared_ptr<ChunkInfo>>& pair) {
                       return pair.second.get();
                   });
    return chunksFromSplit;
}

// Looks up a chunk that corresponds to or contains the range [min, max). There
// should only be one such chunk in the input RoutingTableHistory object.
std::shared_ptr<ChunkInfo> getChunkToSplit(std::shared_ptr<RoutingTableHistory> rt,
                                           const BSONObj& min,
                                           const BSONObj& max) {
    auto chunkToSplitIter = rt->overlappingRanges(min, max, false);
    invariant(std::distance(chunkToSplitIter.first, chunkToSplitIter.second) <= 1);
    invariant(chunkToSplitIter.first != rt->getChunkMap().end());

    return (*chunkToSplitIter.first).second;
}

/**
 * Helper function for testing the results of a chunk split.
 *
 * Finds the chunks in a routing table resulting from a split on the range [minSplitBoundary,
 * maxSplitBoundary). Checks that the correct number of chunks are in the routing table for the
 * corresponding range. To check that the bytes written have been correctly propagated from the
 * chunk being split to the chunks resulting from the split, we check:
 *
 *      For each chunk:
 *          If the chunk was a result of the split:
 *              Make sure it has expectedBytesInChunksFromSplit bytes in its writes tracker
 *          Else:
 *              Make sure its bytes written have not been changed due to the split (e.g. it has
 *              expectedBytesInChunksNotSplit in its writes tracker)
 *
 */
void assertCorrectBytesWritten(std::shared_ptr<RoutingTableHistory> rt,
                               const BSONObj& minSplitBoundary,
                               const BSONObj& maxSplitBoundary,
                               size_t expectedNumChunksFromSplit,
                               uint64_t expectedBytesInChunksFromSplit,
                               uint64_t expectedBytesInChunksNotSplit) {
    auto chunksFromSplit = getChunksInRange(rt, minSplitBoundary, maxSplitBoundary);
    ASSERT_EQ(chunksFromSplit.size(), expectedNumChunksFromSplit);

    for (auto kv : rt->getChunkMap()) {
        auto chunkInfo = kv.second;
        auto writesTracker = chunkInfo->getWritesTracker();
        auto bytesWritten = writesTracker->getBytesWritten();
        if (chunksFromSplit.count(chunkInfo.get()) > 0) {
            ASSERT_EQ(bytesWritten, expectedBytesInChunksFromSplit);
        } else {
            ASSERT_EQ(bytesWritten, expectedBytesInChunksNotSplit);
        }
    }
}

/**
 * Test fixture for tests that need to start with a fresh routing table with
 * only a single chunk in it, with bytes already written to that chunk object.
 */
class RoutingTableHistoryTest : public unittest::Test {
public:
    void setUp() override {
        const OID epoch = OID::gen();
        const Timestamp kRouting(100, 0);
        ChunkVersion version{1, 0, epoch};

        auto initChunk =
            ChunkType{kNss,
                      ChunkRange{_shardKeyPattern.globalMin(), _shardKeyPattern.globalMax()},
                      version,
                      kThisShard};

        _rt = RoutingTableHistory::makeNew(
            kNss, UUID::gen(), _shardKeyPattern, nullptr, false, epoch, {initChunk});

        ASSERT_EQ(_rt->getChunkMap().size(), 1ull);
        // Should only be one
        for (auto kv : _rt->getChunkMap()) {
            auto chunkInfo = kv.second;
            auto writesTracker = chunkInfo->getWritesTracker();
            writesTracker->addBytesWritten(_bytesInOriginalChunk);
        }
    }

    virtual const KeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    virtual uint64_t getBytesInOriginalChunk() const {
        return _bytesInOriginalChunk;
    }

    const std::shared_ptr<RoutingTableHistory>& getInitialRoutingTable() const {
        return _rt;
    }

private:
    uint64_t _bytesInOriginalChunk{4ull};
    std::shared_ptr<RoutingTableHistory> _rt;
    KeyPattern _shardKeyPattern{BSON("a" << 1)};
};

/**
 * Test fixture for tests that need to start with three chunks in it, with the
 * same number of bytes written to every chunk object.
 */
class RoutingTableHistoryTestThreeInitialChunks : public RoutingTableHistoryTest {
public:
    void setUp() override {
        RoutingTableHistoryTest::setUp();
        _initialChunkBoundaryPoints = {getShardKeyPattern().globalMin(),
                                       BSON("a" << 10),
                                       BSON("a" << 20),
                                       getShardKeyPattern().globalMax()};
        _rt = splitChunk(RoutingTableHistoryTest::getInitialRoutingTable(),
                         _initialChunkBoundaryPoints);
        ASSERT_EQ(_rt->getChunkMap().size(), 3ull);
    }

    const std::shared_ptr<RoutingTableHistory>& getInitialRoutingTable() const {
        return _rt;
    }

    std::vector<BSONObj> getInitialChunkBoundaryPoints() {
        return _initialChunkBoundaryPoints;
    }

private:
    std::shared_ptr<RoutingTableHistory> _rt;
    std::vector<BSONObj> _initialChunkBoundaryPoints;
};

TEST_F(RoutingTableHistoryTest, SplittingOnlyChunkCopiesBytesWrittenToAllSubchunks) {
    auto minKey = BSON("a" << 10);
    auto maxKey = BSON("a" << 20);
    auto newChunkBoundaryPoints = {
        getShardKeyPattern().globalMin(), minKey, maxKey, getShardKeyPattern().globalMax()};

    auto rt = splitChunk(getInitialRoutingTable(), newChunkBoundaryPoints);

    ASSERT_EQ(rt->getChunkMap().size(), 3ull);
    for (auto kv : rt->getChunkMap()) {
        auto chunkInfo = kv.second;
        auto writesTracker = chunkInfo->getWritesTracker();
        auto bytesWritten = writesTracker->getBytesWritten();
        ASSERT_EQ(bytesWritten, getBytesInOriginalChunk());
    }
}

TEST_F(RoutingTableHistoryTestThreeInitialChunks,
       SplittingFirstChunkOfSeveralCopiesBytesWrittenToAllSubchunks) {
    auto minKey = getInitialChunkBoundaryPoints()[0];
    auto maxKey = getInitialChunkBoundaryPoints()[1];
    std::vector<BSONObj> newChunkBoundaryPoints = {minKey, BSON("a" << 5), maxKey};

    auto chunkToSplit = getChunkToSplit(getInitialRoutingTable(), minKey, maxKey);
    auto bytesToWrite = 5ull;
    chunkToSplit->getWritesTracker()->addBytesWritten(bytesToWrite);

    // Split first chunk into two
    auto rt = splitChunk(getInitialRoutingTable(), newChunkBoundaryPoints);

    auto expectedNumChunksFromSplit = 2;
    auto expectedBytesInChunksFromSplit = getBytesInOriginalChunk() + bytesToWrite;
    auto expectedBytesInChunksNotSplit = getBytesInOriginalChunk();
    ASSERT_EQ(rt->getChunkMap().size(), 4ull);
    assertCorrectBytesWritten(rt,
                              minKey,
                              maxKey,
                              expectedNumChunksFromSplit,
                              expectedBytesInChunksFromSplit,
                              expectedBytesInChunksNotSplit);
}


TEST_F(RoutingTableHistoryTestThreeInitialChunks,
       SplittingMiddleChunkOfSeveralCopiesBytesWrittenToAllSubchunks) {
    auto minKey = getInitialChunkBoundaryPoints()[1];
    auto maxKey = getInitialChunkBoundaryPoints()[2];
    auto newChunkBoundaryPoints = {minKey, BSON("a" << 16), BSON("a" << 17), maxKey};

    auto chunkToSplit = getChunkToSplit(getInitialRoutingTable(), minKey, maxKey);
    auto bytesToWrite = 5ull;
    chunkToSplit->getWritesTracker()->addBytesWritten(bytesToWrite);

    // Split middle chunk into three
    auto rt = splitChunk(getInitialRoutingTable(), newChunkBoundaryPoints);

    auto expectedNumChunksFromSplit = 3;
    auto expectedBytesInChunksFromSplit = getBytesInOriginalChunk() + bytesToWrite;
    auto expectedBytesInChunksNotSplit = getBytesInOriginalChunk();
    ASSERT_EQ(rt->getChunkMap().size(), 5ull);
    assertCorrectBytesWritten(rt,
                              minKey,
                              maxKey,
                              expectedNumChunksFromSplit,
                              expectedBytesInChunksFromSplit,
                              expectedBytesInChunksNotSplit);
}

TEST_F(RoutingTableHistoryTestThreeInitialChunks,
       SplittingLastChunkOfSeveralCopiesBytesWrittenToAllSubchunks) {
    auto minKey = getInitialChunkBoundaryPoints()[2];
    auto maxKey = getInitialChunkBoundaryPoints()[3];
    auto newChunkBoundaryPoints = {minKey, BSON("a" << 25), maxKey};

    auto chunkToSplit = getChunkToSplit(getInitialRoutingTable(), minKey, maxKey);
    auto bytesToWrite = 5ull;
    chunkToSplit->getWritesTracker()->addBytesWritten(bytesToWrite);

    // Split last chunk into two
    auto rt = splitChunk(getInitialRoutingTable(), newChunkBoundaryPoints);

    auto expectedNumChunksFromSplit = 2;
    auto expectedBytesInChunksFromSplit = getBytesInOriginalChunk() + bytesToWrite;
    auto expectedBytesInChunksNotSplit = getBytesInOriginalChunk();
    ASSERT_EQ(rt->getChunkMap().size(), 4ull);
    assertCorrectBytesWritten(rt,
                              minKey,
                              maxKey,
                              expectedNumChunksFromSplit,
                              expectedBytesInChunksFromSplit,
                              expectedBytesInChunksNotSplit);
}

}  // namespace
}  // namespace mongo
