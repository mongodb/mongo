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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/chunks_test_util.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

using chunks_test_util::assertEqualChunkInfo;
using chunks_test_util::calculateCollVersion;
using chunks_test_util::calculateIntermediateShardKey;
using chunks_test_util::calculateShardsMaxValidAfter;
using chunks_test_util::calculateShardVersions;
using chunks_test_util::genChunkVector;
using chunks_test_util::performRandomChunkOperations;

namespace {

PseudoRandom _random{SecureRandom().nextInt64()};

const ShardId kThisShard("thisShard");
const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

/**
 * Creates a new routing table from the input routing table by inserting the chunks specified by
 * newChunkBoundaryPoints.  newChunkBoundaryPoints specifies a contiguous array of keys indicating
 * chunk boundaries to be inserted. As an example, if you want to split the range [0, 2] into chunks
 * [0, 1] and [1, 2], newChunkBoundaryPoints should be [0, 1, 2].
 */

RoutingTableHistory makeUpdatedRoutingTable(const RoutingTableHistory& rt,
                                            std::vector<ChunkType> chunks) {
    return rt.makeUpdated(boost::none /* timeseriesFields */,
                          boost::none /* reshardingFields */,
                          true,
                          false, /* unsplittable */
                          {std::move(chunks)});
}


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
        newChunks.emplace_back(rt.getUUID(), range, curVersion, kThisShard);
    }

    return makeUpdatedRoutingTable(rt, std::move(newChunks));
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

class RoutingTableHistoryTest : public unittest::Test {
public:
    const KeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const OID& collEpoch() const {
        return _epoch;
    }

    const Timestamp& collTimestamp() const {
        return _collTimestamp;
    }

    const UUID& collUUID() const {
        return _collUUID;
    }

    std::vector<ChunkType> genRandomChunkVector(size_t minNumChunks = 1,
                                                size_t maxNumChunks = 30) const {
        return chunks_test_util::genRandomChunkVector(
            _collUUID, _epoch, _collTimestamp, maxNumChunks, minNumChunks);
    }

    RoutingTableHistory makeNewRt(const std::vector<ChunkType>& chunks) const {
        const auto chunkBucketSize = llround(_random.nextInt64(chunks.size() * 1.2)) + 1;
        LOGV2(7162710,
              "Creating new RoutingTable",
              "chunkBucketSize"_attr = chunkBucketSize,
              "numChunks"_attr = chunks.size());
        RAIIServerParameterControllerForTest chunkBucketSizeParameter(
            "routingTableCacheChunkBucketSize", chunkBucketSize);
        return RoutingTableHistory::makeNew(kNss,
                                            _collUUID,
                                            _shardKeyPattern,
                                            false, /* unsplittable */
                                            nullptr,
                                            false,
                                            _epoch,
                                            _collTimestamp,
                                            boost::none /* timeseriesFields */,
                                            boost::none /* reshardingFields */,
                                            true,
                                            chunks);
    }

protected:
    KeyPattern _shardKeyPattern{chunks_test_util::kShardKeyPattern};
    const OID _epoch{OID::gen()};
    const Timestamp _collTimestamp{1, 1};
    const UUID _collUUID{UUID::gen()};
};

/**
 * Test fixture for tests that need to start with three chunks in it
 */
class RoutingTableHistoryTestThreeInitialChunks : public RoutingTableHistoryTest {
public:
    void setUp() override {
        RoutingTableHistoryTest::setUp();

        _initialChunkBoundaryPoints = {getShardKeyPattern().globalMin(),
                                       BSON("a" << 10),
                                       BSON("a" << 20),
                                       getShardKeyPattern().globalMax()};
        ChunkVersion version{{collEpoch(), collTimestamp()}, {1, 0}};
        auto chunks =
            genChunkVector(collUUID(), _initialChunkBoundaryPoints, version, 1 /* numShards */);

        _rt.emplace(makeNewRt(chunks));

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

/*
 * Test creation of a Routing Table with randomly generated chunks
 */
TEST_F(RoutingTableHistoryTest, RandomCreateBasic) {
    const auto chunks = genRandomChunkVector();
    const auto expectedShardVersions = calculateShardVersions(chunks);
    const auto expectedCollVersion = calculateCollVersion(expectedShardVersions);

    // Create a new routing table from the randomly generated chunks
    auto rt = makeNewRt(chunks);

    // Checks basic getter of routing table return correct values
    ASSERT_EQ(kNss, rt.nss());
    ASSERT_EQ(ShardKeyPattern(getShardKeyPattern()).toString(), rt.getShardKeyPattern().toString());
    ASSERT_EQ(chunks.size(), rt.numChunks());

    // Check that chunks have correct info
    size_t i = 0;
    rt.forEachChunk([&](const auto& chunkInfo) {
        assertEqualChunkInfo(ChunkInfo{chunks[i++]}, *chunkInfo);
        return true;
    });
    ASSERT_EQ(i, chunks.size());

    // Checks collection version is correct
    ASSERT_EQ(expectedCollVersion, rt.getVersion());

    // Checks version for each chunk
    for (const auto& [shardId, shardVersion] : expectedShardVersions) {
        ASSERT_EQ(shardVersion, rt.getVersion(shardId));
    }

    ASSERT_EQ(expectedShardVersions.size(), rt.getNShardsOwningChunks());

    std::set<ShardId> expectedShardIds;
    for (const auto& [shardId, shardVersion] : expectedShardVersions) {
        expectedShardIds.insert(shardId);
    }
    std::set<ShardId> shardIds;
    rt.getAllShardIds(&shardIds);
    ASSERT(expectedShardIds == shardIds);

    // Validate all shard maxValidAfter
    const auto expectedShardsMaxValidAfter = calculateShardsMaxValidAfter(chunks);
    const auto expectedMaxValidAfter = [&](const ShardId& shard) {
        auto it = expectedShardsMaxValidAfter.find(shard);
        return it != expectedShardsMaxValidAfter.end() ? it->second : Timestamp{0, 0};
    };
    for (const auto& [shardId, _] : expectedShardVersions) {
        ASSERT_GTE(rt.getMaxValidAfter(shardId), expectedMaxValidAfter(shardId));
    }
    ASSERT_EQ(rt.getMaxValidAfter(ShardId{"shard-without-chunks"}), (Timestamp{0, 0}));
}

/*
 * Test that creation of Routing Table with chunks that do not cover the entire shard key space
 * fails.
 *
 * The gap is produced by removing a random chunks from the randomly generated chunk list. Thus it
 * also cover the case for which min/max key is missing.
 */
TEST_F(RoutingTableHistoryTest, RandomCreateWithMissingChunkFail) {
    auto chunks = genRandomChunkVector(2 /*minNumChunks*/);

    // Remove one random chunk to simulate a gap in the shardkey
    chunks.erase(chunks.begin() + _random.nextInt64(chunks.size()));

    // Create a new routing table from the randomly generated chunks
    ASSERT_THROWS_CODE(makeNewRt(chunks), DBException, ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Test that creation of Routing Table with chunks that do not cover the entire shard key space
 * fails.
 *
 * The gap is produced by shrinking the range of a random chunk.
 */
TEST_F(RoutingTableHistoryTest, RandomCreateWithChunkGapFail) {
    auto chunks = genRandomChunkVector(2 /*minNumChunks*/);

    auto& shrinkedChunk = chunks.at(_random.nextInt64(chunks.size()));
    auto intermediateKey =
        calculateIntermediateShardKey(shrinkedChunk.getMin(), shrinkedChunk.getMax());
    if (_random.nextInt64(2)) {
        // Shrink right bound
        shrinkedChunk.setRange({shrinkedChunk.getMin(), intermediateKey});
    } else {
        // Shrink left bound
        shrinkedChunk.setRange({intermediateKey, shrinkedChunk.getMax()});
    }

    // Create a new routing table from the randomly generated chunks
    ASSERT_THROWS_CODE(makeNewRt(chunks), DBException, ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Updating ChunkMap with gaps must fail
 */
TEST_F(RoutingTableHistoryTest, RandomUpdateWithChunkGapFail) {
    auto chunks = genRandomChunkVector();

    // Create a new routing table from the randomly generated chunks
    auto rt = makeNewRt(chunks);
    auto collVersion = rt.getVersion();

    auto shrinkedChunk = chunks.at(_random.nextInt64(chunks.size()));
    auto intermediateKey =
        calculateIntermediateShardKey(shrinkedChunk.getMin(), shrinkedChunk.getMax());
    if (_random.nextInt64(2)) {
        // Shrink right bound
        shrinkedChunk.setRange({shrinkedChunk.getMin(), intermediateKey});
    } else {
        // Shrink left bound
        shrinkedChunk.setRange({intermediateKey, shrinkedChunk.getMax()});
    }

    // Bump chunk version
    collVersion.incMajor();
    shrinkedChunk.setVersion(collVersion);

    ASSERT_THROWS_CODE(makeUpdatedRoutingTable(rt, {shrinkedChunk}),
                       AssertionException,
                       ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Creating a Routing Table with overlapping chunks must fail.
 */
TEST_F(RoutingTableHistoryTest, RandomCreateWithChunkOverlapFail) {
    auto chunks = genRandomChunkVector(2 /* minNumChunks */);

    auto chunkToExtendIt = chunks.begin() + _random.nextInt64(chunks.size());

    const auto canExtendLeft = chunkToExtendIt > chunks.begin();
    const auto extendRight =
        !canExtendLeft || ((chunkToExtendIt < std::prev(chunks.end())) && _random.nextInt64(2));
    const auto extendLeft = !extendRight;
    if (extendRight) {
        auto newMax = calculateIntermediateShardKey(chunkToExtendIt->getMax(),
                                                    std::next(chunkToExtendIt)->getMax(),
                                                    0.0 /* minKeyProb */,
                                                    0.1 /* maxKeyProb */);
        // extend right bound
        chunkToExtendIt->setRange({chunkToExtendIt->getMin(), newMax});
        auto newVersion = chunkToExtendIt->getVersion();
        newVersion.incMajor();
        std::next(chunkToExtendIt)->setVersion(newVersion);
    }

    if (extendLeft) {
        invariant(canExtendLeft);
        auto newMin = calculateIntermediateShardKey(std::prev(chunkToExtendIt)->getMin(),
                                                    chunkToExtendIt->getMin(),
                                                    0.1 /* minKeyProb */,
                                                    0.0 /* maxKeyProb */);

        // extend left bound
        chunkToExtendIt->setRange({newMin, chunkToExtendIt->getMax()});
        auto newVersion = chunkToExtendIt->getVersion();
        newVersion.incMajor();
        std::prev(chunkToExtendIt)->setVersion(newVersion);
    }

    // Create a new routing table from the randomly generated chunks
    ASSERT_THROWS_CODE(makeNewRt(chunks), DBException, ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Updating a ChunkMap with overlapping chunks must fail.
 */
TEST_F(RoutingTableHistoryTest, RandomUpdateWithChunkOverlapFail) {
    auto chunks = genRandomChunkVector(2 /* minNumChunks */);

    // Create a new routing table from the randomly generated chunks
    auto rt = makeNewRt(chunks);
    auto collVersion = rt.getVersion();

    auto chunkToExtendIt = chunks.begin() + _random.nextInt64(chunks.size());

    const auto canExtendLeft = chunkToExtendIt > chunks.begin();
    const auto extendRight =
        !canExtendLeft || (chunkToExtendIt < std::prev(chunks.end()) && _random.nextInt64(2));
    const auto extendLeft = !extendRight;
    if (extendRight) {
        auto newMax = calculateIntermediateShardKey(chunkToExtendIt->getMax(),
                                                    std::next(chunkToExtendIt)->getMax());
        // extend right bound
        chunkToExtendIt->setRange({chunkToExtendIt->getMin(), newMax});
    }

    if (extendLeft) {
        invariant(canExtendLeft);
        auto newMin = calculateIntermediateShardKey(std::prev(chunkToExtendIt)->getMin(),
                                                    chunkToExtendIt->getMin());
        // extend left bound
        chunkToExtendIt->setRange({newMin, chunkToExtendIt->getMax()});
    }

    // Bump chunk version
    collVersion.incMajor();
    chunkToExtendIt->setVersion(collVersion);

    ASSERT_THROWS_CODE(makeUpdatedRoutingTable(rt, {*chunkToExtendIt}),
                       AssertionException,
                       ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Creating a Routing Table with wrong min key must fail.
 */
TEST_F(RoutingTableHistoryTest, RandomCreateWrongMinFail) {
    auto chunks = genRandomChunkVector();

    chunks.begin()->setRange(
        {BSON("a" << std::numeric_limits<int64_t>::min()), chunks.begin()->getMax()});

    // Create a new routing table from the randomly generated chunks
    ASSERT_THROWS_CODE(makeNewRt(chunks), DBException, ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Creating a Routing Table with wrong max key must fail.
 */
TEST_F(RoutingTableHistoryTest, RandomCreateWrongMaxFail) {
    auto chunks = genRandomChunkVector();

    chunks.begin()->setRange(
        {chunks.begin()->getMin(), BSON("a" << std::numeric_limits<int64_t>::max())});

    // Create a new routing table from the randomly generated chunks
    ASSERT_THROWS_CODE(makeNewRt(chunks), DBException, ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Creating a Routing Table with mismatching epoch must fail.
 */
TEST_F(RoutingTableHistoryTest, RandomCreateMismatchingTimestampFail) {
    auto chunks = genRandomChunkVector();

    // Change epoch on a random chunk
    auto chunkIt = chunks.begin() + _random.nextInt64(chunks.size());
    const auto& oldVersion = chunkIt->getVersion();

    const Timestamp wrongTimestamp{Date_t::now()};
    ChunkVersion newVersion{{collEpoch(), wrongTimestamp},
                            {oldVersion.majorVersion(), oldVersion.minorVersion()}};
    chunkIt->setVersion(newVersion);

    // Create a new routing table from the randomly generated chunks
    ASSERT_THROWS_CODE(makeNewRt(chunks), DBException, ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Updating a Routing Table with mismatching Timestamp must fail.
 */
TEST_F(RoutingTableHistoryTest, RandomUpdateMismatchingTimestampFail) {
    auto chunks = genRandomChunkVector();

    // Create a new routing table from the randomly generated chunks
    auto rt = makeNewRt(chunks);

    // Change epoch on a random chunk
    auto chunkIt = chunks.begin() + _random.nextInt64(chunks.size());
    const auto& oldVersion = chunkIt->getVersion();
    const Timestamp wrongTimestamp{Date_t::now()};
    ChunkVersion newVersion{{collEpoch(), wrongTimestamp},
                            {oldVersion.majorVersion(), oldVersion.minorVersion()}};
    chunkIt->setVersion(newVersion);

    ASSERT_THROWS_CODE(makeUpdatedRoutingTable(rt, {*chunkIt}),
                       AssertionException,
                       ErrorCodes::ChunkMetadataInconsistency);
}


/*
 * Test update of the Routing Table with randomly generated changed chunks.
 */
TEST_F(RoutingTableHistoryTest, RandomUpdate) {
    auto initialChunks = genRandomChunkVector();

    const auto initialShardVersions = calculateShardVersions(initialChunks);
    const auto initialCollVersion = calculateCollVersion(initialShardVersions);

    // Create a new routing table from the randomly generated initialChunks
    auto initialRt = makeNewRt(initialChunks);

    auto chunks = initialChunks;
    const auto maxNumChunkOps = 2 * initialChunks.size();
    const auto numChunkOps = _random.nextInt32(maxNumChunkOps);

    performRandomChunkOperations(&chunks, numChunkOps);

    std::vector<ChunkType> updatedChunks;
    for (const auto& chunk : chunks) {
        if ((chunk.getVersion() <=> initialCollVersion) == std::partial_ordering::greater) {
            updatedChunks.push_back(chunk);
        }
    }

    const auto expectedShardVersions = calculateShardVersions(chunks);
    const auto expectedCollVersion = calculateCollVersion(expectedShardVersions);

    auto rt = makeUpdatedRoutingTable(initialRt, updatedChunks);

    // Checks basic getter of routing table return correct values
    ASSERT_EQ(kNss, rt.nss());
    ASSERT_EQ(ShardKeyPattern(getShardKeyPattern()).toString(), rt.getShardKeyPattern().toString());
    ASSERT_EQ(chunks.size(), rt.numChunks());

    // Check that chunks have correct info
    size_t i = 0;
    rt.forEachChunk([&](const auto& chunkInfo) {
        assertEqualChunkInfo(ChunkInfo{chunks[i++]}, *chunkInfo);
        return true;
    });
    ASSERT_EQ(i, chunks.size());

    // Checks collection version is correct
    ASSERT_EQ(expectedCollVersion, rt.getVersion());

    // Checks version for each shard
    for (const auto& [shardId, shardVersion] : expectedShardVersions) {
        ASSERT_EQ(shardVersion, rt.getVersion(shardId));
    }

    ASSERT_EQ(expectedShardVersions.size(), rt.getNShardsOwningChunks());

    std::set<ShardId> expectedShardIds;
    for (const auto& [shardId, shardVersion] : expectedShardVersions) {
        expectedShardIds.insert(shardId);
    }
    std::set<ShardId> shardIds;
    rt.getAllShardIds(&shardIds);
    ASSERT(expectedShardIds == shardIds);

    // Validate all shard maxValidAfter
    const auto expectedShardsMaxValidAfter = calculateShardsMaxValidAfter(chunks);
    const auto expectedMaxValidAfter = [&](const ShardId& shard) {
        auto it = expectedShardsMaxValidAfter.find(shard);
        return it != expectedShardsMaxValidAfter.end() ? it->second : Timestamp{0, 0};
    };
    for (const auto& [shardId, _] : expectedShardVersions) {
        ASSERT_GTE(rt.getMaxValidAfter(shardId), expectedMaxValidAfter(shardId))
            << "For shardid " << shardId;
    }
    ASSERT_EQ(rt.getMaxValidAfter(ShardId{"shard-without-chunks"}), (Timestamp{0, 0}));
}

TEST_F(RoutingTableHistoryTest, AllowMigrationFlag) {
    auto chunks = genRandomChunkVector();

    auto makeUpdatedChunk = [&](const ChunkVersion& oldVersion) {
        auto updatedChunk = chunks[_random.nextInt64(chunks.size())];
        updatedChunk.setVersion({{collEpoch(), collTimestamp()},
                                 {oldVersion.majorVersion() + 1, oldVersion.minorVersion()}});
        return updatedChunk;
    };

    for (auto initialAllowMigrationsValue : std::vector<bool>{false, true}) {

        auto allowMigrationsValue = initialAllowMigrationsValue;
        auto rt = RoutingTableHistory::makeNew(kNss,
                                               collUUID(),
                                               getShardKeyPattern(),
                                               false, /* unsplittable */
                                               nullptr,
                                               false,
                                               collEpoch(),
                                               collTimestamp(),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               allowMigrationsValue,
                                               chunks);
        ASSERT_EQ(allowMigrationsValue, rt.allowMigrations());

        // Create an updated routing table with flipped allowMigration flag
        allowMigrationsValue = !allowMigrationsValue;
        rt = rt.makeUpdated(boost::none /* timeseriesFields */,
                            boost::none /* reshardingFields */,
                            allowMigrationsValue,
                            false, /* unsplittable */
                            {makeUpdatedChunk(rt.getVersion())});
        ASSERT_EQ(allowMigrationsValue, rt.allowMigrations());

        // Change back the allow migration flag to the original value
        allowMigrationsValue = !allowMigrationsValue;
        rt = rt.makeUpdated(boost::none /* timeseriesFields */,
                            boost::none /* reshardingFields */,
                            allowMigrationsValue,
                            false, /* unsplittable */
                            {makeUpdatedChunk(rt.getVersion())});
        ASSERT_EQ(allowMigrationsValue, rt.allowMigrations());
    }
}

TEST_F(RoutingTableHistoryTest, TestSplits) {
    ChunkVersion version{{collEpoch(), collTimestamp()}, {1, 0}};

    auto chunkAll =
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  version,
                  kThisShard};

    auto rt = makeNewRt({chunkAll});

    std::vector<ChunkType> chunks1 = {
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 1}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion{{collEpoch(), collTimestamp()}, {2, 2}},
                  kThisShard}};

    auto rt1 = makeUpdatedRoutingTable(rt, std::move(chunks1));
    auto v1 = ChunkVersion{{collEpoch(), collTimestamp()}, {2, 2}};
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));

    std::vector<ChunkType> chunks2 = {
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 2}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << -1)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {3, 1}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << -1), BSON("a" << 0)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {3, 2}),
                  kThisShard}};

    auto rt2 = makeUpdatedRoutingTable(rt, std::move(chunks2));

    auto v2 = ChunkVersion({collEpoch(), collTimestamp()}, {3, 2});
    ASSERT_EQ(v2, rt2.getVersion(kThisShard));
}

TEST_F(RoutingTableHistoryTest, TestReplaceEmptyChunk) {
    std::vector<ChunkType> initialChunks = {
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {1, 0}),
                  kThisShard}};

    auto rt = makeNewRt(initialChunks);
    ASSERT_EQ(rt.numChunks(), 1);

    std::vector<ChunkType> changedChunks = {
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 1}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 2}),
                  kThisShard}};

    auto rt1 = makeUpdatedRoutingTable(rt, std::move(changedChunks));

    auto v1 = ChunkVersion({collEpoch(), collTimestamp()}, {2, 2});
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
    std::vector<ChunkType> initialChunks = {
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {1, 0}),
                  kThisShard}};

    auto rt = makeNewRt(initialChunks);
    ASSERT_EQ(rt.numChunks(), 1);

    std::vector<ChunkType> changedChunks = {
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {1, 0}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 1}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 2}),
                  kThisShard}};

    auto rt1 = makeUpdatedRoutingTable(rt, std::move(changedChunks));

    auto v1 = ChunkVersion({collEpoch(), collTimestamp()}, {2, 2});
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));
    ASSERT_EQ(rt1.numChunks(), 2);
}

TEST_F(RoutingTableHistoryTest, TestOutOfOrderVersion) {
    std::vector<ChunkType> initialChunks = {
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 1}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 2}),
                  kThisShard}};

    auto rt = makeNewRt(initialChunks);
    ASSERT_EQ(rt.numChunks(), 2);

    std::vector<ChunkType> changedChunks = {
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 0), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {3, 0}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {3, 1}),
                  kThisShard}};

    auto rt1 = makeUpdatedRoutingTable(rt, std::move(changedChunks));

    auto v1 = ChunkVersion({collEpoch(), collTimestamp()}, {3, 1});
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));
    ASSERT_EQ(rt1.numChunks(), 2);

    auto chunk1 = rt1.findIntersectingChunk(BSON("a" << 0));
    ASSERT_EQ(chunk1->getLastmod(), ChunkVersion({collEpoch(), collTimestamp()}, {3, 0}));
    ASSERT_EQ(chunk1->getMin().woCompare(BSON("a" << 0)), 0);
    ASSERT_EQ(chunk1->getMax().woCompare(getShardKeyPattern().globalMax()), 0);
}

TEST_F(RoutingTableHistoryTest, TestMergeChunks) {
    std::vector<ChunkType> initialChunks = {
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 0), BSON("a" << 10)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 0}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 1}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 10), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 2}),
                  kThisShard}};

    auto rt = makeNewRt(initialChunks);

    ASSERT_EQ(rt.numChunks(), 3);
    ASSERT_EQ(rt.getVersion(), ChunkVersion({collEpoch(), collTimestamp()}, {2, 2}));

    std::vector<ChunkType> changedChunks = {
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 10), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {3, 0}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 10)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {3, 1}),
                  kThisShard}};

    auto rt1 = makeUpdatedRoutingTable(rt, std::move(changedChunks));

    auto v1 = ChunkVersion({collEpoch(), collTimestamp()}, {3, 1});
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));
    ASSERT_EQ(rt1.numChunks(), 2);
}

TEST_F(RoutingTableHistoryTest, TestMergeChunksOrdering) {
    std::vector<ChunkType> initialChunks = {
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << -10), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 0}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << -500)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 1}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << -500), BSON("a" << -10)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 2}),
                  kThisShard}};

    auto rt = makeNewRt(initialChunks);
    ASSERT_EQ(rt.numChunks(), 3);
    ASSERT_EQ(rt.getVersion(), ChunkVersion({collEpoch(), collTimestamp()}, {2, 2}));

    std::vector<ChunkType> changedChunks = {
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << -500), BSON("a" << -10)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 2}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << -10)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {3, 1}),
                  kThisShard}};

    auto rt1 = makeUpdatedRoutingTable(rt, std::move(changedChunks));

    auto v1 = ChunkVersion({collEpoch(), collTimestamp()}, {3, 1});
    ASSERT_EQ(v1, rt1.getVersion(kThisShard));
    ASSERT_EQ(rt1.numChunks(), 2);

    auto chunk1 = rt1.findIntersectingChunk(BSON("a" << -500));
    ASSERT_EQ(chunk1->getLastmod(), ChunkVersion({collEpoch(), collTimestamp()}, {3, 1}));
    ASSERT_EQ(chunk1->getMin().woCompare(getShardKeyPattern().globalMin()), 0);
    ASSERT_EQ(chunk1->getMax().woCompare(BSON("a" << -10)), 0);
}

TEST_F(RoutingTableHistoryTest, TestFlatten) {
    std::vector<ChunkType> initialChunks = {
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 10)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 0}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 10), BSON("a" << 20)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 1}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 20), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {2, 2}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {3, 0}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 10)},
                  ChunkVersion({collEpoch(), collTimestamp()}, {4, 0}),
                  kThisShard},
        ChunkType{collUUID(),
                  ChunkRange{BSON("a" << 10), getShardKeyPattern().globalMax()},
                  ChunkVersion({collEpoch(), collTimestamp()}, {4, 1}),
                  kThisShard},
    };

    auto rt = makeNewRt(initialChunks);
    ASSERT_EQ(rt.numChunks(), 2);
    ASSERT_EQ(rt.getVersion(), ChunkVersion({collEpoch(), collTimestamp()}, {4, 1}));

    auto chunk1 = rt.findIntersectingChunk(BSON("a" << 0));
    ASSERT_EQ(chunk1->getLastmod(), ChunkVersion({collEpoch(), collTimestamp()}, {4, 0}));
    ASSERT_EQ(chunk1->getMin().woCompare(getShardKeyPattern().globalMin()), 0);
    ASSERT_EQ(chunk1->getMax().woCompare(BSON("a" << 10)), 0);
}

}  // namespace
}  // namespace mongo
