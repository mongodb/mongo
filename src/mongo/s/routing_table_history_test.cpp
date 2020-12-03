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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_writes_tracker.h"
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
RoutingTableHistory splitChunk(const RoutingTableHistory& rt,
                               const std::vector<BSONObj>& newChunkBoundaryPoints) {

    invariant(newChunkBoundaryPoints.size() > 1);

    // Convert the boundary points into chunk range objects, e.g. {0, 1, 2} ->
    // {{ChunkRange{0, 1}, ChunkRange{1, 2}}
    std::vector<ChunkRange> newChunkRanges;
    for (size_t i = 0; i < newChunkBoundaryPoints.size() - 1; ++i) {
        newChunkRanges.emplace_back(newChunkBoundaryPoints[i], newChunkBoundaryPoints[i + 1]);
    }

    std::vector<ChunkType> newChunks;
    auto curVersion = rt.getVersion();

    for (const auto& range : newChunkRanges) {
        // Chunks must be inserted ordered by version
        curVersion.incMajor();
        newChunks.emplace_back(kNss, range, curVersion, kThisShard);
    }

    return rt.makeUpdated(boost::none, true, newChunks);
}

/**
 * Gets a set of raw pointers to ChunkInfo objects in the specified range,
 */
std::set<ChunkInfo*> getChunksInRange(const RoutingTableHistory& rt,
                                      const BSONObj& min,
                                      const BSONObj& max) {
    std::set<ChunkInfo*> chunksFromSplit;

    rt.forEachOverlappingChunk(min, max, false, [&](auto& chunk) {
        chunksFromSplit.insert(chunk.get());
        return true;
    });

    return chunksFromSplit;
}

/**
 * Looks up a chunk that corresponds to or contains the range [min, max). There should only be one
 * such chunk in the input RoutingTableHistory object.
 */
ChunkInfo* getChunkToSplit(const RoutingTableHistory& rt, const BSONObj& min, const BSONObj& max) {
    std::shared_ptr<ChunkInfo> firstOverlappingChunk;

    rt.forEachOverlappingChunk(min, max, false, [&](auto& chunkInfo) {
        firstOverlappingChunk = chunkInfo;
        return false;  // only need first chunk
    });

    invariant(firstOverlappingChunk);
    return firstOverlappingChunk.get();
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
 */
void assertCorrectBytesWritten(const RoutingTableHistory& rt,
                               const BSONObj& minSplitBoundary,
                               const BSONObj& maxSplitBoundary,
                               size_t expectedNumChunksFromSplit,
                               uint64_t expectedBytesInChunksFromSplit,
                               uint64_t expectedBytesInChunksNotSplit) {
    auto chunksFromSplit = getChunksInRange(rt, minSplitBoundary, maxSplitBoundary);
    ASSERT_EQ(chunksFromSplit.size(), expectedNumChunksFromSplit);

    rt.forEachChunk([&](const auto& chunkInfo) {
        auto writesTracker = chunkInfo->getWritesTracker();
        auto bytesWritten = writesTracker->getBytesWritten();
        if (chunksFromSplit.count(chunkInfo.get()) > 0) {
            ASSERT_EQ(bytesWritten, expectedBytesInChunksFromSplit);
        } else {
            ASSERT_EQ(bytesWritten, expectedBytesInChunksNotSplit);
        }

        return true;
    });
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
        ChunkVersion version{1, 0, epoch, boost::none /* timestamp */};

        auto initChunk =
            ChunkType{kNss,
                      ChunkRange{_shardKeyPattern.globalMin(), _shardKeyPattern.globalMax()},
                      version,
                      kThisShard};

        _rt.emplace(RoutingTableHistory::makeNew(kNss,
                                                 UUID::gen(),
                                                 _shardKeyPattern,
                                                 nullptr,
                                                 false,
                                                 epoch,
                                                 boost::none /* timestamp */,
                                                 boost::none,
                                                 true,
                                                 {initChunk}));
        ASSERT_EQ(_rt->numChunks(), 1ull);

        // Should only be one
        _rt->forEachChunk([&](const auto& chunkInfo) {
            auto writesTracker = chunkInfo->getWritesTracker();
            writesTracker->addBytesWritten(_bytesInOriginalChunk);
            return true;
        });
    }

    const KeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    uint64_t getBytesInOriginalChunk() const {
        return _bytesInOriginalChunk;
    }

    const RoutingTableHistory& getInitialRoutingTable() const {
        return *_rt;
    }

private:
    uint64_t _bytesInOriginalChunk{4ull};

    boost::optional<RoutingTableHistory> _rt;

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
        _rt.emplace(splitChunk(RoutingTableHistoryTest::getInitialRoutingTable(),
                               _initialChunkBoundaryPoints));
        ASSERT_EQ(_rt->numChunks(), 3ull);
    }

    const RoutingTableHistory& getInitialRoutingTable() const {
        return *_rt;
    }

    std::vector<BSONObj> getInitialChunkBoundaryPoints() {
        return _initialChunkBoundaryPoints;
    }

private:
    boost::optional<RoutingTableHistory> _rt;

    std::vector<BSONObj> _initialChunkBoundaryPoints;
};

TEST_F(RoutingTableHistoryTest, SplittingOnlyChunkCopiesBytesWrittenToAllSubchunks) {
    auto minKey = BSON("a" << 10);
    auto maxKey = BSON("a" << 20);
    auto newChunkBoundaryPoints = {
        getShardKeyPattern().globalMin(), minKey, maxKey, getShardKeyPattern().globalMax()};

    auto rt = splitChunk(getInitialRoutingTable(), newChunkBoundaryPoints);
    ASSERT_EQ(rt.numChunks(), 3ull);

    rt.forEachChunk([&](const auto& chunkInfo) {
        auto writesTracker = chunkInfo->getWritesTracker();
        auto bytesWritten = writesTracker->getBytesWritten();
        ASSERT_EQ(bytesWritten, getBytesInOriginalChunk());
        return true;
    });
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
    ASSERT_EQ(rt.numChunks(), 4ull);
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
    ASSERT_EQ(rt.numChunks(), 5ull);
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
    ASSERT_EQ(rt.numChunks(), 4ull);
    assertCorrectBytesWritten(rt,
                              minKey,
                              maxKey,
                              expectedNumChunksFromSplit,
                              expectedBytesInChunksFromSplit,
                              expectedBytesInChunksNotSplit);
}

TEST_F(RoutingTableHistoryTest, TestSplits) {
    const OID epoch = OID::gen();
    ChunkVersion version{1, 0, epoch, boost::none /* timestamp */};

    auto chunkAll =
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  version,
                  kThisShard};

    auto rt = RoutingTableHistory::makeNew(kNss,
                                           UUID::gen(),
                                           getShardKeyPattern(),
                                           nullptr,
                                           false,
                                           epoch,
                                           boost::none /* timestamp */,
                                           boost::none,
                                           true,
                                           {chunkAll});

    std::vector<ChunkType> chunks1 = {
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion{2, 1, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion{2, 2, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt1 = rt.makeUpdated(boost::none, true, chunks1);
    auto v1 = ChunkVersion{2, 2, epoch, boost::none /* timestamp */};
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));

    std::vector<ChunkType> chunks2 = {
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion{2, 2, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << -1)},
                  ChunkVersion{3, 1, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{BSON("a" << -1), BSON("a" << 0)},
                  ChunkVersion{3, 2, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt2 = rt1.makeUpdated(boost::none, true, chunks2);
    auto v2 = ChunkVersion{3, 2, epoch, boost::none /* timestamp */};
    ASSERT_EQ(v2, rt2.getVersion(kThisShard));
}

TEST_F(RoutingTableHistoryTest, TestReplaceEmptyChunk) {
    const OID epoch = OID::gen();

    std::vector<ChunkType> initialChunks = {
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  ChunkVersion{1, 0, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt = RoutingTableHistory::makeNew(kNss,
                                           UUID::gen(),
                                           getShardKeyPattern(),
                                           nullptr,
                                           false,
                                           epoch,
                                           boost::none /* timestamp */,
                                           boost::none,
                                           true,
                                           initialChunks);
    ASSERT_EQ(rt.numChunks(), 1);

    std::vector<ChunkType> changedChunks = {
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion{2, 1, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion{2, 2, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt1 = rt.makeUpdated(boost::none, true, changedChunks);
    auto v1 = ChunkVersion{2, 2, epoch, boost::none /* timestamp */};
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));
    ASSERT_EQ(rt1.numChunks(), 2);

    std::shared_ptr<ChunkInfo> found;

    rt1.forEachChunk(
        [&](auto& chunkInfo) {
            if (chunkInfo->getShardIdAt(boost::none) == kThisShard) {
                found = chunkInfo;
                return false;
            }
            return true;
        },
        BSON("a" << 0));
    ASSERT(found);
}

TEST_F(RoutingTableHistoryTest, TestUseLatestVersions) {
    const OID epoch = OID::gen();

    std::vector<ChunkType> initialChunks = {
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  ChunkVersion{1, 0, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt = RoutingTableHistory::makeNew(kNss,
                                           UUID::gen(),
                                           getShardKeyPattern(),
                                           nullptr,
                                           false,
                                           epoch,
                                           boost::none /* timestamp */,
                                           boost::none,
                                           true,
                                           initialChunks);
    ASSERT_EQ(rt.numChunks(), 1);

    std::vector<ChunkType> changedChunks = {
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  ChunkVersion{1, 0, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion{2, 1, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion{2, 2, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt1 = rt.makeUpdated(boost::none, true, changedChunks);
    auto v1 = ChunkVersion{2, 2, epoch, boost::none /* timestamp */};
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));
    ASSERT_EQ(rt1.numChunks(), 2);
}

TEST_F(RoutingTableHistoryTest, TestOutOfOrderVersion) {
    const OID epoch = OID::gen();

    std::vector<ChunkType> initialChunks = {
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion{2, 1, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion{2, 2, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt = RoutingTableHistory::makeNew(kNss,
                                           UUID::gen(),
                                           getShardKeyPattern(),
                                           nullptr,
                                           false,
                                           epoch,
                                           boost::none /* timestamp */,
                                           boost::none,
                                           true,
                                           initialChunks);
    ASSERT_EQ(rt.numChunks(), 2);

    std::vector<ChunkType> changedChunks = {
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion{3, 0, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion{3, 1, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt1 = rt.makeUpdated(boost::none, true, changedChunks);
    auto v1 = ChunkVersion{3, 1, epoch, boost::none /* timestamp */};
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));
    ASSERT_EQ(rt1.numChunks(), 2);

    auto chunk1 = rt1.findIntersectingChunk(BSON("a" << 0));
    ASSERT_EQ(chunk1->getLastmod(), ChunkVersion(3, 0, epoch, boost::none /* timestamp */));
    ASSERT_EQ(chunk1->getMin().woCompare(BSON("a" << 0)), 0);
    ASSERT_EQ(chunk1->getMax().woCompare(getShardKeyPattern().globalMax()), 0);
}

TEST_F(RoutingTableHistoryTest, TestMergeChunks) {
    const OID epoch = OID::gen();

    std::vector<ChunkType> initialChunks = {
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 0), BSON("a" << 10)},
                  ChunkVersion{2, 0, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion{2, 1, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 10), getShardKeyPattern().globalMax()},
                  ChunkVersion{2, 2, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt = RoutingTableHistory::makeNew(kNss,
                                           UUID::gen(),
                                           getShardKeyPattern(),
                                           nullptr,
                                           false,
                                           epoch,
                                           boost::none,
                                           boost::none /* timestamp */,
                                           true,
                                           initialChunks);
    ASSERT_EQ(rt.numChunks(), 3);
    ASSERT_EQ(rt.getVersion(), ChunkVersion(2, 2, epoch, boost::none /* timestamp */));

    std::vector<ChunkType> changedChunks = {
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 10), getShardKeyPattern().globalMax()},
                  ChunkVersion{3, 0, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 10)},
                  ChunkVersion{3, 1, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt1 = rt.makeUpdated(boost::none, true, changedChunks);
    auto v1 = ChunkVersion{3, 1, epoch, boost::none /* timestamp */};
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));
    ASSERT_EQ(rt1.numChunks(), 2);
}

TEST_F(RoutingTableHistoryTest, TestMergeChunksOrdering) {
    const OID epoch = OID::gen();

    std::vector<ChunkType> initialChunks = {
        ChunkType{kNss,
                  ChunkRange{BSON("a" << -10), getShardKeyPattern().globalMax()},
                  ChunkVersion{2, 0, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << -500)},
                  ChunkVersion{2, 1, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{BSON("a" << -500), BSON("a" << -10)},
                  ChunkVersion{2, 2, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt = RoutingTableHistory::makeNew(kNss,
                                           UUID::gen(),
                                           getShardKeyPattern(),
                                           nullptr,
                                           false,
                                           epoch,
                                           boost::none /* timestamp */,
                                           boost::none,
                                           true,
                                           initialChunks);
    ASSERT_EQ(rt.numChunks(), 3);
    ASSERT_EQ(rt.getVersion(), ChunkVersion(2, 2, epoch, boost::none /* timestamp */));

    std::vector<ChunkType> changedChunks = {
        ChunkType{kNss,
                  ChunkRange{BSON("a" << -500), BSON("a" << -10)},
                  ChunkVersion{2, 2, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << -10)},
                  ChunkVersion{3, 1, epoch, boost::none /* timestamp */},
                  kThisShard}};

    auto rt1 = rt.makeUpdated(boost::none, true, changedChunks);
    auto v1 = ChunkVersion{3, 1, epoch, boost::none /* timestamp */};
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));
    ASSERT_EQ(rt1.numChunks(), 2);

    auto chunk1 = rt1.findIntersectingChunk(BSON("a" << -500));
    ASSERT_EQ(chunk1->getLastmod(), ChunkVersion(3, 1, epoch, boost::none /* timestamp */));
    ASSERT_EQ(chunk1->getMin().woCompare(getShardKeyPattern().globalMin()), 0);
    ASSERT_EQ(chunk1->getMax().woCompare(BSON("a" << -10)), 0);
}

TEST_F(RoutingTableHistoryTest, TestFlatten) {
    const OID epoch = OID::gen();

    std::vector<ChunkType> initialChunks = {
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 10)},
                  ChunkVersion{2, 0, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 10), BSON("a" << 20)},
                  ChunkVersion{2, 1, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 20), getShardKeyPattern().globalMax()},
                  ChunkVersion{2, 2, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  ChunkVersion{3, 0, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 10)},
                  ChunkVersion{4, 0, epoch, boost::none /* timestamp */},
                  kThisShard},
        ChunkType{kNss,
                  ChunkRange{BSON("a" << 10), getShardKeyPattern().globalMax()},
                  ChunkVersion{4, 1, epoch, boost::none /* timestamp */},
                  kThisShard},
    };

    auto rt = RoutingTableHistory::makeNew(kNss,
                                           UUID::gen(),
                                           getShardKeyPattern(),
                                           nullptr,
                                           false,
                                           epoch,
                                           boost::none /* timestamp */,
                                           boost::none,
                                           true,
                                           initialChunks);
    ASSERT_EQ(rt.numChunks(), 2);
    ASSERT_EQ(rt.getVersion(), ChunkVersion(4, 1, epoch, boost::none /* timestamp */));

    auto chunk1 = rt.findIntersectingChunk(BSON("a" << 0));
    ASSERT_EQ(chunk1->getLastmod(), ChunkVersion(4, 0, epoch, boost::none /* timestamp */));
    ASSERT_EQ(chunk1->getMin().woCompare(getShardKeyPattern().globalMin()), 0);
    ASSERT_EQ(chunk1->getMax().woCompare(BSON("a" << 10)), 0);
}

}  // namespace
}  // namespace mongo
