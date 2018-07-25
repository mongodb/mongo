/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 * Asserts that the given vectors of BSON objects are equal
 */
void assertBSONObjVectorsAreEqual(const std::vector<BSONObj>& expected,
                                  const std::vector<BSONObj>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (auto expectedIt = expected.begin(), actualIt = actual.begin();
         expectedIt != expected.end() && actualIt != actual.end();
         ++expectedIt, ++actualIt) {
        ASSERT_BSONOBJ_EQ(*expectedIt, *actualIt);
    }
}

/**
 * Returns a test hashed shard key pattern if isHashed is true.
 * Otherwise, returns a regular shard key pattern.
 */
ShardKeyPattern makeShardKeyPattern(bool isHashed) {
    if (isHashed)
        return ShardKeyPattern(BSON("x"
                                    << "hashed"));
    return ShardKeyPattern(BSON("x" << 1));
}

/**
 * Calls calculateHashedSplitPointsForEmptyCollection according to the given arguments
 * and asserts that calculated split points match with the expected split points.
 */
void checkCalculatedHashedSplitPoints(bool isHashed,
                                      bool isEmpty,
                                      int numShards,
                                      int numInitialChunks,
                                      const std::vector<BSONObj>* expectedInitialSplitPoints,
                                      const std::vector<BSONObj>* expectedFinalSplitPoints) {
    std::vector<BSONObj> initialSplitPoints;
    std::vector<BSONObj> finalSplitPoints;
    InitialSplitPolicy::calculateHashedSplitPointsForEmptyCollection(makeShardKeyPattern(isHashed),
                                                                     isEmpty,
                                                                     numShards,
                                                                     numInitialChunks,
                                                                     &initialSplitPoints,
                                                                     &finalSplitPoints);
    assertBSONObjVectorsAreEqual(initialSplitPoints, *expectedInitialSplitPoints);
    assertBSONObjVectorsAreEqual(finalSplitPoints, *expectedFinalSplitPoints);
}

TEST(CalculateHashedSplitPointsTest, EmptyCollectionMoreChunksThanShards) {
    const std::vector<BSONObj> expectedInitialSplitPoints = {BSON("x" << 0)};
    const std::vector<BSONObj> expectedFinalSplitPoints = {
        BSON("x" << -4611686018427387902LL), BSON("x" << 0), BSON("x" << 4611686018427387902LL)};
    checkCalculatedHashedSplitPoints(
        true, true, 2, 4, &expectedInitialSplitPoints, &expectedFinalSplitPoints);
}

TEST(CalculateHashedSplitPointsTest, EmptyCollectionChunksEqualToShards) {
    const std::vector<BSONObj> expectedSplitPoints = {BSON("x" << -3074457345618258602LL),
                                                      BSON("x" << 3074457345618258602LL)};
    checkCalculatedHashedSplitPoints(true, true, 3, 3, &expectedSplitPoints, &expectedSplitPoints);
}

TEST(CalculateHashedSplitPointsTest, EmptyCollectionHashedWithInitialSplitsReturnsEmptySplits) {
    const std::vector<BSONObj> expectedSplitPoints;
    checkCalculatedHashedSplitPoints(true, true, 2, 1, &expectedSplitPoints, &expectedSplitPoints);
}

TEST(CalculateHashedSplitPointsTest, EmptyCollectionNumInitialChunksZero) {
    const std::vector<BSONObj> expectedInitialSplitPoints = {BSON("x" << 0)};
    const std::vector<BSONObj> expectedFinalSplitPoints = {
        BSON("x" << -4611686018427387902LL), BSON("x" << 0), BSON("x" << 4611686018427387902LL)};
    checkCalculatedHashedSplitPoints(
        true, true, 2, 0, &expectedInitialSplitPoints, &expectedFinalSplitPoints);
}

TEST(CalculateHashedSplitPointsTest, NonEmptyCollectionHashedWithInitialSplitsFails) {
    std::vector<BSONObj> expectedSplitPoints;
    ASSERT_THROWS_CODE(checkCalculatedHashedSplitPoints(
                           true, false, 2, 3, &expectedSplitPoints, &expectedSplitPoints),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST(CalculateHashedSplitPointsTest, NotHashedWithInitialSplitsFails) {
    std::vector<BSONObj> expectedSplitPoints;
    ASSERT_THROWS_CODE(checkCalculatedHashedSplitPoints(
                           false, true, 2, 3, &expectedSplitPoints, &expectedSplitPoints),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

class GenerateInitialSplitChunksTest : public unittest::Test {
public:
    ChunkType makeChunk(const BSONObj min, const BSONObj max, const ShardId shardId) {
        ChunkVersion version(1, 0, OID::gen());
        ChunkType chunk(_nss, ChunkRange(min, max), version, shardId);
        chunk.setHistory({ChunkHistory(_timeStamp, shardId)});
        return chunk;
    }
    const NamespaceString nss() {
        return _nss;
    }

    const ShardKeyPattern& shardKeyPattern() {
        return _shardKeyPattern;
    }

    const std::vector<ShardId> shardIds() {
        return _shardIds;
    }

    const Timestamp timeStamp() {
        return _timeStamp;
    }

private:
    const NamespaceString _nss{"test.foo"};
    const ShardKeyPattern _shardKeyPattern = makeShardKeyPattern(true);
    const std::vector<ShardId> _shardIds = {ShardId("testShard0"), ShardId("testShard1")};
    const Timestamp _timeStamp{Date_t::now()};
};

TEST_F(GenerateInitialSplitChunksTest, NoSplitPoints) {
    const std::vector<BSONObj> splitPoints;
    const auto shardCollectionConfig = InitialSplitPolicy::generateShardCollectionInitialChunks(
        nss(), shardKeyPattern(), shardIds()[0], timeStamp(), splitPoints, shardIds());

    const auto& keyPattern = shardKeyPattern().getKeyPattern();
    const auto expectedChunk =
        makeChunk(keyPattern.globalMin(), keyPattern.globalMax(), shardIds()[0]);
    ASSERT_EQ(1U, shardCollectionConfig.chunks.size());
    ASSERT_BSONOBJ_EQ(expectedChunk.toShardBSON(), shardCollectionConfig.chunks[0].toShardBSON());
}

TEST_F(GenerateInitialSplitChunksTest, SplitPointsMoreThanAvailableShards) {
    const auto& keyPattern = shardKeyPattern().getKeyPattern();
    const std::vector<BSONObj> expectedChunkBounds = {keyPattern.globalMin(),
                                                      BSON("x" << -4611686018427387902LL),
                                                      BSON("x" << 0),
                                                      BSON("x" << 4611686018427387902LL),
                                                      keyPattern.globalMax()};
    const std::vector<BSONObj> splitPoints(expectedChunkBounds.begin() + 1,
                                           expectedChunkBounds.end() - 1);
    const auto shardCollectionConfig = InitialSplitPolicy::generateShardCollectionInitialChunks(
        nss(), shardKeyPattern(), shardIds()[0], timeStamp(), splitPoints, shardIds());

    ASSERT_EQ(splitPoints.size() + 1, shardCollectionConfig.chunks.size());
    for (unsigned long i = 0; i < expectedChunkBounds.size() - 1; ++i) {
        // chunks should be distributed in a round-robin manner
        const auto expectedChunk = makeChunk(
            expectedChunkBounds[i], expectedChunkBounds[i + 1], shardIds()[i % shardIds().size()]);
        ASSERT_BSONOBJ_EQ(expectedChunk.toShardBSON().removeField("lastmod"),
                          shardCollectionConfig.chunks[i].toShardBSON().removeField("lastmod"));
    }
}

}  // namespace
}  // namespace mongo
