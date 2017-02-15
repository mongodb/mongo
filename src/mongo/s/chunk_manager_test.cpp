/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/s/catalog/sharding_catalog_test_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using executor::RemoteCommandResponse;
using executor::RemoteCommandRequest;

const NamespaceString kNss("TestDB", "TestColl");

class ChunkManagerTestFixture : public ShardingCatalogTestFixture {
protected:
    void setUp() override {
        ShardingCatalogTestFixture::setUp();
        setRemote(HostAndPort("FakeRemoteClient:34567"));
        configTargeter()->setFindHostReturnValue(HostAndPort{CONFIG_HOST_PORT});
    }

    /**
     * Returns a chunk manager with chunks at the specified split points. Each individual chunk is
     * placed on a separate shard with id ranging from "0" to the number of chunks.
     */
    std::unique_ptr<ChunkManager> makeChunkManager(
        const ShardKeyPattern& shardKeyPattern,
        std::unique_ptr<CollatorInterface> defaultCollator,
        bool unique,
        const std::vector<BSONObj>& splitPoints) {
        ChunkVersion version(1, 0, OID::gen());

        std::vector<BSONObj> shards;
        std::vector<BSONObj> initialChunks;

        auto splitPointsIncludingEnds(splitPoints);
        splitPointsIncludingEnds.insert(splitPointsIncludingEnds.begin(),
                                        shardKeyPattern.getKeyPattern().globalMin());
        splitPointsIncludingEnds.push_back(shardKeyPattern.getKeyPattern().globalMax());

        for (size_t i = 1; i < splitPointsIncludingEnds.size(); ++i) {
            ShardType shard;
            shard.setName(str::stream() << (i - 1));
            shard.setHost(str::stream() << "Host" << (i - 1) << ":12345");

            shards.push_back(shard.toBSON());

            ChunkType chunk;
            chunk.setNS(kNss.ns());
            chunk.setMin(shardKeyPattern.getKeyPattern().extendRangeBound(
                splitPointsIncludingEnds[i - 1], false));
            chunk.setMax(shardKeyPattern.getKeyPattern().extendRangeBound(
                splitPointsIncludingEnds[i], false));
            chunk.setShard(shard.getName());
            chunk.setVersion(version);

            initialChunks.push_back(chunk.toConfigBSON());

            version.incMajor();
        }

        // Load the initial manager
        auto manager = stdx::make_unique<ChunkManager>(
            kNss, version.epoch(), shardKeyPattern, std::move(defaultCollator), unique);

        auto future = launchAsync([&manager] {
            ON_BLOCK_EXIT([&] { Client::destroy(); });
            Client::initThread("Test");
            auto opCtx = cc().makeOperationContext();
            manager->loadExistingRanges(opCtx.get(), nullptr);
        });

        expectFindOnConfigSendBSONObjVector(initialChunks);
        expectFindOnConfigSendBSONObjVector(shards);

        future.timed_get(kFutureTimeout);

        return manager;
    }
};

using ChunkManagerLoadTest = ChunkManagerTestFixture;

TEST_F(ChunkManagerLoadTest, IncrementalLoadAfterSplit) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialManager(makeChunkManager(shardKeyPattern, nullptr, true, {}));

    ChunkVersion version = initialManager->getVersion();

    CollectionType collType;
    collType.setNs(kNss);
    collType.setEpoch(version.epoch());
    collType.setUpdatedAt(jsTime());
    collType.setKeyPattern(shardKeyPattern.toBSON());
    collType.setUnique(false);

    ChunkManager manager(kNss, version.epoch(), shardKeyPattern, nullptr, true);

    auto future =
        launchAsync([&] { manager.loadExistingRanges(operationContext(), initialManager.get()); });

    // Return set of chunks, which represent a split
    expectFindOnConfigSendBSONObjVector([&]() {
        version.incMajor();

        ChunkType chunk1;
        chunk1.setNS(kNss.ns());
        chunk1.setMin(shardKeyPattern.getKeyPattern().globalMin());
        chunk1.setMax(BSON("_id" << 0));
        chunk1.setShard({"0"});
        chunk1.setVersion(version);

        version.incMinor();

        ChunkType chunk2;
        chunk2.setNS(kNss.ns());
        chunk2.setMin(BSON("_id" << 0));
        chunk2.setMax(shardKeyPattern.getKeyPattern().globalMax());
        chunk2.setShard({"0"});
        chunk2.setVersion(version);

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk2.toConfigBSON()};
    }());

    future.timed_get(kFutureTimeout);
}

/**
 * Fixture to be used as a shortcut for tests which exercise the getShardIdsForQuery routing logic
 */
class ChunkManagerQueryTest : public ChunkManagerTestFixture {
protected:
    void runQueryTest(const BSONObj& shardKey,
                      std::unique_ptr<CollatorInterface> defaultCollator,
                      bool unique,
                      const std::vector<BSONObj>& splitPoints,
                      const BSONObj& query,
                      const BSONObj& queryCollation,
                      const std::set<ShardId> expectedShardIds) {
        const ShardKeyPattern shardKeyPattern(shardKey);
        auto chunkManager =
            makeChunkManager(shardKeyPattern, std::move(defaultCollator), false, splitPoints);

        std::set<ShardId> shardIds;
        chunkManager->getShardIdsForQuery(operationContext(), query, queryCollation, &shardIds);

        BSONArrayBuilder expectedBuilder;
        for (const auto& shardId : expectedShardIds) {
            expectedBuilder << shardId;
        }

        BSONArrayBuilder actualBuilder;
        for (const auto& shardId : shardIds) {
            actualBuilder << shardId;
        }

        ASSERT_BSONOBJ_EQ(expectedBuilder.arr(), actualBuilder.arr());
    }
};

TEST_F(ChunkManagerQueryTest, EmptyQuerySingleShard) {
    runQueryTest(BSON("a" << 1), nullptr, false, {}, BSONObj(), BSONObj(), {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, EmptyQueryMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSONObj(),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, UniversalRangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("b" << 1),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, EqualityRangeSingleShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {},
                 BSON("a"
                      << "x"),
                 BSONObj(),
                 {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, EqualityRangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a"
                      << "y"),
                 BSONObj(),
                 {ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, SetRangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 fromjson("{a:{$in:['u','y']}}"),
                 BSONObj(),
                 {ShardId("0"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, GTRangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << GT << "x"),
                 BSONObj(),
                 {ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, GTERangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << GTE << "x"),
                 BSONObj(),
                 {ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, LTRangeMultiShard) {
    // NOTE (SERVER-4791): It isn't actually necessary to return shard 2 because its lowest key is
    // "y", which is excluded from the query
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << LT << "y"),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, LTERangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << LTE << "y"),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, OrEqualities) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 fromjson("{$or:[{a:'u'},{a:'y'}]}"),
                 BSONObj(),
                 {ShardId("0"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, OrEqualityInequality) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 fromjson("{$or:[{a:'u'},{a:{$gte:'y'}}]}"),
                 BSONObj(),
                 {ShardId("0"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, OrEqualityInequalityUnhelpful) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 fromjson("{$or:[{a:'u'},{a:{$gte:'zz'}},{}]}"),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, UnsatisfiableRangeSingleShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {},
                 BSON("a" << GT << "x" << LT << "x"),
                 BSONObj(),
                 {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, UnsatisfiableRangeMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << GT << "x" << LT << "x"),
                 BSONObj(),
                 {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, EqualityThenUnsatisfiable) {
    runQueryTest(BSON("a" << 1 << "b" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << 1 << "b" << GT << 4 << LT << 4),
                 BSONObj(),
                 {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, InequalityThenUnsatisfiable) {
    runQueryTest(BSON("a" << 1 << "b" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a" << GT << 1 << "b" << GT << 4 << LT << 4),
                 BSONObj(),
                 {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, OrEqualityUnsatisfiableInequality) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 fromjson("{$or:[{a:'x'},{a:{$gt:'u',$lt:'u'}},{a:{$gte:'y'}}]}"),
                 BSONObj(),
                 {ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, InMultiShard) {
    runQueryTest(BSON("a" << 1 << "b" << 1),
                 nullptr,
                 false,
                 {BSON("a" << 5 << "b" << 10), BSON("a" << 5 << "b" << 20)},
                 BSON("a" << BSON("$in" << BSON_ARRAY(0 << 5 << 10)) << "b"
                          << BSON("$in" << BSON_ARRAY(0 << 5 << 25))),
                 BSONObj(),
                 {ShardId("0"), ShardId("1"), ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, CollationStringsMultiShard) {
    runQueryTest(BSON("a" << 1),
                 nullptr,
                 false,
                 {BSON("a"
                       << "x"),
                  BSON("a"
                       << "y"),
                  BSON("a"
                       << "z")},
                 BSON("a"
                      << "y"),
                 BSON("locale"
                      << "mock_reverse_string"),
                 {ShardId("0"), ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, DefaultCollationStringsMultiShard) {
    runQueryTest(
        BSON("a" << 1),
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString),
        false,
        {BSON("a"
              << "x"),
         BSON("a"
              << "y"),
         BSON("a"
              << "z")},
        BSON("a"
             << "y"),
        BSON("locale"
             << "mock_reverse_string"),
        {ShardId("0"), ShardId("1"), ShardId("2"), ShardId("3")});
}

TEST_F(ChunkManagerQueryTest, SimpleCollationStringsMultiShard) {
    runQueryTest(
        BSON("a" << 1),
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString),
        false,
        {BSON("a"
              << "x"),
         BSON("a"
              << "y"),
         BSON("a"
              << "z")},
        BSON("a"
             << "y"),
        BSON("locale"
             << "simple"),
        {ShardId("2")});
}

TEST_F(ChunkManagerQueryTest, CollationNumbersMultiShard) {
    runQueryTest(
        BSON("a" << 1),
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString),
        false,
        {BSON("a"
              << "x"),
         BSON("a"
              << "y"),
         BSON("a"
              << "z")},
        BSON("a" << 5),
        BSON("locale"
             << "mock_reverse_string"),
        {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, DefaultCollationNumbersMultiShard) {
    runQueryTest(
        BSON("a" << 1),
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString),
        false,
        {BSON("a"
              << "x"),
         BSON("a"
              << "y"),
         BSON("a"
              << "z")},
        BSON("a" << 5),
        BSONObj(),
        {ShardId("0")});
}

TEST_F(ChunkManagerQueryTest, SimpleCollationNumbersMultiShard) {
    runQueryTest(
        BSON("a" << 1),
        stdx::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString),
        false,
        {BSON("a"
              << "x"),
         BSON("a"
              << "y"),
         BSON("a"
              << "z")},
        BSON("a" << 5),
        BSON("locale"
             << "simple"),
        {ShardId("0")});
}

}  // namespace
}  // namespace mongo
