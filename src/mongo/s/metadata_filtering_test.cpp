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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/query_request.h"
#include "mongo/db/s/metadata_manager.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/database_version_helpers.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using unittest::assertGet;

const NamespaceString kNss("TestDB", "TestColl");

class MetadataFilteringTest : public CatalogCacheTestFixture {
protected:
    void setUp() override {
        CatalogCacheTestFixture::setUp();

        setupNShards(2);

        _manager = std::make_shared<MetadataManager>(serviceContext(), kNss, executor());
    }

    void expectGetDatabase() {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            DatabaseType db(kNss.db().toString(), {"0"}, true, databaseVersion::makeNew());
            return std::vector<BSONObj>{db.toBSON()};
        }());
    }

    void expectGetCollection(OID epoch, const ShardKeyPattern& shardKeyPattern) {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            CollectionType collType;
            collType.setNs(kNss);
            collType.setEpoch(epoch);
            collType.setKeyPattern(shardKeyPattern.toBSON());
            collType.setUnique(false);

            return std::vector<BSONObj>{collType.toBSON()};
        }());
    }

    // Prepares data with a history array populated:
    //      chunk1 - [min, -100)
    //      chunk2 - [100, 0)
    //      chunk3 - [0, 100)
    //      chunk4 - [100, max)
    // and the history:
    //      time (now,75) shard0(chunk1, chunk3) shard1(chunk2, chunk4)
    //      time (75,25) shard0(chunk2, chunk4) shard1(chunk1, chunk3)
    //      time (25,0) - no history
    void prepareTestData() {
        const OID epoch = OID::gen();
        const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

        auto future = scheduleRoutingInfoRefresh(kNss);

        expectGetDatabase();
        expectGetCollection(epoch, shardKeyPattern);

        expectGetCollection(epoch, shardKeyPattern);
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            ChunkVersion version(1, 0, epoch);

            ChunkType chunk1(kNss,
                             {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << -100)},
                             version,
                             {"0"});
            chunk1.setHistory({ChunkHistory(Timestamp(75, 0), ShardId("0")),
                               ChunkHistory(Timestamp(25, 0), ShardId("1"))});
            version.incMinor();

            ChunkType chunk2(kNss, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
            chunk2.setHistory({ChunkHistory(Timestamp(75, 0), ShardId("1")),
                               ChunkHistory(Timestamp(25, 0), ShardId("0"))});
            version.incMinor();

            ChunkType chunk3(kNss, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
            chunk3.setHistory({ChunkHistory(Timestamp(75, 0), ShardId("0")),
                               ChunkHistory(Timestamp(25, 0), ShardId("1"))});
            version.incMinor();

            ChunkType chunk4(kNss,
                             {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                             version,
                             {"1"});
            chunk4.setHistory({ChunkHistory(Timestamp(75, 0), ShardId("1")),
                               ChunkHistory(Timestamp(25, 0), ShardId("0"))});
            version.incMinor();

            return std::vector<BSONObj>{chunk1.toConfigBSON(),
                                        chunk2.toConfigBSON(),
                                        chunk3.toConfigBSON(),
                                        chunk4.toConfigBSON()};
        }());

        auto routingInfo = future.timed_get(kFutureTimeout);
        ASSERT(routingInfo->cm());
        auto cm = routingInfo->cm();

        ASSERT_EQ(4, cm->numChunks());
    }

    std::shared_ptr<MetadataManager> _manager;
};

// Verifies that right set of documents is visible.
TEST_F(MetadataFilteringTest, FilterDocumentsPresent) {
    prepareTestData();

    ShardingState::get(operationContext())->setEnabledForTest(ShardId("0").toString());

    auto metadata = _manager->createMetadataAt(operationContext(), LogicalTime(Timestamp(100, 0)));

    ASSERT_TRUE(metadata->keyBelongsToMe(BSON("_id" << -500)));
    ASSERT_TRUE(metadata->keyBelongsToMe(BSON("_id" << 50)));
    ASSERT_FALSE(metadata->keyBelongsToMe(BSON("_id" << -50)));
    ASSERT_FALSE(metadata->keyBelongsToMe(BSON("_id" << 500)));
}

// Verifies that a different set of documents is visible for a timestamp in the past.
TEST_F(MetadataFilteringTest, FilterDocumentsPast) {
    prepareTestData();

    ShardingState::get(operationContext())->setEnabledForTest(ShardId("0").toString());

    auto metadata = _manager->createMetadataAt(operationContext(), LogicalTime(Timestamp(50, 0)));

    ASSERT_FALSE(metadata->keyBelongsToMe(BSON("_id" << -500)));
    ASSERT_FALSE(metadata->keyBelongsToMe(BSON("_id" << 50)));
    ASSERT_TRUE(metadata->keyBelongsToMe(BSON("_id" << -50)));
    ASSERT_TRUE(metadata->keyBelongsToMe(BSON("_id" << 500)));
}

// Verifies that when accessing too far into the past we get the stale error.
TEST_F(MetadataFilteringTest, FilterDocumentsStale) {
    prepareTestData();

    ShardingState::get(operationContext())->setEnabledForTest(ShardId("0").toString());

    auto metadata = _manager->createMetadataAt(operationContext(), LogicalTime(Timestamp(10, 0)));

    ASSERT_THROWS_CODE(metadata->keyBelongsToMe(BSON("_id" << -500)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
    ASSERT_THROWS_CODE(metadata->keyBelongsToMe(BSON("_id" << 50)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
    ASSERT_THROWS_CODE(metadata->keyBelongsToMe(BSON("_id" << -50)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
    ASSERT_THROWS_CODE(metadata->keyBelongsToMe(BSON("_id" << 500)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
}

// The same test as FilterDocumentsPresent but using "readConcern"
TEST_F(MetadataFilteringTest, FilterDocumentsPresentShardingState) {
    prepareTestData();

    ShardingState::get(operationContext())->setEnabledForTest(ShardId("0").toString());

    BSONObj readConcern = BSON("readConcern" << BSON("level"
                                                     << "snapshot"
                                                     << "atClusterTime"
                                                     << Timestamp(100, 0)));

    auto&& readConcernArgs = repl::ReadConcernArgs::get(operationContext());
    ASSERT_OK(readConcernArgs.initialize(readConcern["readConcern"]));

    auto css = CollectionShardingState::get(operationContext(), kNss);

    auto metadata = css->getMetadata(operationContext());

    ASSERT_TRUE(metadata->keyBelongsToMe(BSON("_id" << -500)));
    ASSERT_TRUE(metadata->keyBelongsToMe(BSON("_id" << 50)));
    ASSERT_FALSE(metadata->keyBelongsToMe(BSON("_id" << -50)));
    ASSERT_FALSE(metadata->keyBelongsToMe(BSON("_id" << 500)));
}

// The same test as FilterDocumentsPast but using "readConcern"
TEST_F(MetadataFilteringTest, FilterDocumentsPastShardingState) {
    prepareTestData();

    ShardingState::get(operationContext())->setEnabledForTest(ShardId("0").toString());

    BSONObj readConcern = BSON("readConcern" << BSON("level"
                                                     << "snapshot"
                                                     << "atClusterTime"
                                                     << Timestamp(50, 0)));

    auto&& readConcernArgs = repl::ReadConcernArgs::get(operationContext());
    ASSERT_OK(readConcernArgs.initialize(readConcern["readConcern"]));

    auto css = CollectionShardingState::get(operationContext(), kNss);

    auto metadata = css->getMetadata(operationContext());

    ASSERT_FALSE(metadata->keyBelongsToMe(BSON("_id" << -500)));
    ASSERT_FALSE(metadata->keyBelongsToMe(BSON("_id" << 50)));
    ASSERT_TRUE(metadata->keyBelongsToMe(BSON("_id" << -50)));
    ASSERT_TRUE(metadata->keyBelongsToMe(BSON("_id" << 500)));
}

// The same test as FilterDocumentsStale but using "readConcern"
TEST_F(MetadataFilteringTest, FilterDocumentsStaleShardingState) {
    prepareTestData();

    ShardingState::get(operationContext())->setEnabledForTest(ShardId("0").toString());

    BSONObj readConcern = BSON("readConcern" << BSON("level"
                                                     << "snapshot"
                                                     << "atClusterTime"
                                                     << Timestamp(10, 0)));

    auto&& readConcernArgs = repl::ReadConcernArgs::get(operationContext());
    ASSERT_OK(readConcernArgs.initialize(readConcern["readConcern"]));

    auto css = CollectionShardingState::get(operationContext(), kNss);

    auto metadata = css->getMetadata(operationContext());

    ASSERT_THROWS_CODE(metadata->keyBelongsToMe(BSON("_id" << -500)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
    ASSERT_THROWS_CODE(metadata->keyBelongsToMe(BSON("_id" << 50)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
    ASSERT_THROWS_CODE(metadata->keyBelongsToMe(BSON("_id" << -50)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
    ASSERT_THROWS_CODE(metadata->keyBelongsToMe(BSON("_id" << 500)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
}


}  // namespace
}  // namespace mongo
