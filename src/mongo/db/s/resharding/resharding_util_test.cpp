/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/hasher.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/resharding/resharding_txn_cloner.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/shard_id.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ReshardingUtilTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();
        ShardType shard1;
        shard1.setName("a");
        shard1.setHost("a:1234");
        ShardType shard2;
        shard2.setName("b");
        shard2.setHost("b:1234");
        setupShards({shard1, shard2});
    }
    void tearDown() override {
        ConfigServerTestFixture::tearDown();
    }

    const std::string shardKey() {
        return _shardKey;
    }

    const KeyPattern& keyPattern() {
        return _shardKeyPattern.getKeyPattern();
    }

    const NamespaceString nss() {
        return _nss;
    }

    BSONObj makeReshardedChunk(const ChunkRange range, std::string shardId) {
        BSONObjBuilder reshardedchunkBuilder;
        reshardedchunkBuilder.append(ReshardedChunk::kRecipientShardIdFieldName, shardId);
        reshardedchunkBuilder.append(ReshardedChunk::kMinFieldName, range.getMin());
        reshardedchunkBuilder.append(ReshardedChunk::kMaxFieldName, range.getMax());
        return reshardedchunkBuilder.obj();
    }

    BSONObj makeZone(const ChunkRange range, std::string zoneName) {
        BSONObjBuilder tagDocBuilder;
        tagDocBuilder.append("_id",
                             BSON(TagsType::ns(nss().ns()) << TagsType::min(range.getMin())));
        tagDocBuilder.append(TagsType::ns(), nss().ns());
        tagDocBuilder.append(TagsType::min(), range.getMin());
        tagDocBuilder.append(TagsType::max(), range.getMax());
        tagDocBuilder.append(TagsType::tag(), zoneName);
        return tagDocBuilder.obj();
    }

    TagsType makeTagType(const ChunkRange range, std::string zoneName) {
        return unittest::assertGet(TagsType::fromBSON(makeZone(range, zoneName)));
    }

    const std::string zoneName(std::string zoneNum) {
        return "_zoneName" + zoneNum;
    }

private:
    const NamespaceString _nss{"test.foo"};
    const std::string _shardKey = "x";
    const ShardKeyPattern _shardKeyPattern = ShardKeyPattern(BSON("x"
                                                                  << "hashed"));
};

// Confirm the highest minFetchTimestamp is properly computed.
TEST(ReshardingUtilTest, HighestMinFetchTimestampSucceeds) {
    std::vector<DonorShardEntry> donorShards{
        makeDonorShard(ShardId("s0"), DonorStateEnum::kDonatingInitialData, Timestamp(10, 2)),
        makeDonorShard(ShardId("s1"), DonorStateEnum::kDonatingInitialData, Timestamp(10, 3)),
        makeDonorShard(ShardId("s2"), DonorStateEnum::kDonatingInitialData, Timestamp(10, 1))};
    auto highestMinFetchTimestamp = getHighestMinFetchTimestamp(donorShards);
    ASSERT_EQ(Timestamp(10, 3), highestMinFetchTimestamp);
}

TEST(ReshardingUtilTest, HighestMinFetchTimestampThrowsWhenDonorMissingTimestamp) {
    std::vector<DonorShardEntry> donorShards{
        makeDonorShard(ShardId("s0"), DonorStateEnum::kDonatingInitialData, Timestamp(10, 3)),
        makeDonorShard(ShardId("s1"), DonorStateEnum::kDonatingInitialData),
        makeDonorShard(ShardId("s2"), DonorStateEnum::kDonatingInitialData, Timestamp(10, 2))};
    ASSERT_THROWS_CODE(getHighestMinFetchTimestamp(donorShards), DBException, 4957300);
}

TEST(ReshardingUtilTest, HighestMinFetchTimestampSucceedsWithDonorStateGTkDonatingOplogEntries) {
    std::vector<DonorShardEntry> donorShards{
        makeDonorShard(ShardId("s0"), DonorStateEnum::kPreparingToMirror, Timestamp(10, 2)),
        makeDonorShard(ShardId("s1"), DonorStateEnum::kDonatingOplogEntries, Timestamp(10, 3)),
        makeDonorShard(ShardId("s2"), DonorStateEnum::kDonatingOplogEntries, Timestamp(10, 1))};
    auto highestMinFetchTimestamp = getHighestMinFetchTimestamp(donorShards);
    ASSERT_EQ(Timestamp(10, 3), highestMinFetchTimestamp);
}

// Validate resharded chunks tests.

TEST_F(ReshardingUtilTest, SuccessfulValidateReshardedChunkCase) {
    std::vector<mongo::BSONObj> chunks;
    const std::vector<ChunkRange> chunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), keyPattern().globalMax()),
    };
    chunks.push_back(makeReshardedChunk(chunkRanges[0], "a"));
    chunks.push_back(makeReshardedChunk(chunkRanges[1], "b"));

    validateReshardedChunks(chunks, operationContext(), keyPattern());
}

TEST_F(ReshardingUtilTest, FailWhenHoleInChunkRange) {
    std::vector<mongo::BSONObj> chunks;
    const std::vector<ChunkRange> chunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 20), keyPattern().globalMax()),
    };
    chunks.push_back(makeReshardedChunk(chunkRanges[0], "a"));
    chunks.push_back(makeReshardedChunk(chunkRanges[1], "b"));
    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenOverlapInChunkRange) {
    const std::vector<ChunkRange> overlapChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 10)),
        ChunkRange(BSON(shardKey() << 5), keyPattern().globalMax()),
    };
    std::vector<mongo::BSONObj> chunks;
    chunks.push_back(makeReshardedChunk(overlapChunkRanges[0], "a"));
    chunks.push_back(makeReshardedChunk(overlapChunkRanges[1], "b"));
    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenChunkRangeDoesNotStartAtGlobalMin) {
    std::vector<mongo::BSONObj> chunks;
    const std::vector<ChunkRange> chunkRanges = {
        ChunkRange(BSON(shardKey() << 10), BSON(shardKey() << 20)),
        ChunkRange(BSON(shardKey() << 20), keyPattern().globalMax()),
    };
    chunks.push_back(makeReshardedChunk(chunkRanges[0], "a"));
    chunks.push_back(makeReshardedChunk(chunkRanges[1], "b"));
    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenChunkRangeDoesNotEndAtGlobalMax) {
    std::vector<mongo::BSONObj> chunks;
    const std::vector<ChunkRange> chunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
    };
    chunks.push_back(makeReshardedChunk(chunkRanges[0], "a"));
    chunks.push_back(makeReshardedChunk(chunkRanges[1], "b"));

    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

// Validate zones tests.

TEST_F(ReshardingUtilTest, SuccessfulValidateZoneCase) {
    const std::vector<ChunkRange> zoneRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
    };
    const std::vector<TagsType> authoritativeTags = {makeTagType(zoneRanges[1], zoneName("1"))};
    std::vector<mongo::BSONObj> zones;
    zones.push_back(makeZone(zoneRanges[0], zoneName("1")));
    validateZones(zones, authoritativeTags);
}

TEST_F(ReshardingUtilTest, FailWhenMissingZoneNameInUserProvidedZone) {
    const std::vector<ChunkRange> zoneRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
    };
    const std::vector<TagsType> authoritativeTags = {makeTagType(zoneRanges[1], zoneName("1"))};
    std::vector<mongo::BSONObj> zones;
    // make a zoneBSONObj and remove the zoneName field from it.
    auto zone = makeZone(zoneRanges[0], zoneName("0")).removeField(TagsType::tag());
    zones.push_back(zone);
    ASSERT_THROWS_CODE(validateZones(zones, authoritativeTags), DBException, ErrorCodes::NoSuchKey);
}

TEST_F(ReshardingUtilTest, FailWhenZoneNameDoesNotExistInConfigTagsCollection) {
    const std::vector<ChunkRange> zoneRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
    };
    const std::vector<TagsType> authoritativeTags = {makeTagType(zoneRanges[1], zoneName("1"))};
    std::vector<mongo::BSONObj> zones;
    zones.push_back(makeZone(zoneRanges[0], zoneName("0")));
    ASSERT_THROWS_CODE(validateZones(zones, authoritativeTags), DBException, ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenOverlappingZones) {
    const std::vector<ChunkRange> overlapZoneRanges = {
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
        ChunkRange(BSON(shardKey() << 8), keyPattern().globalMax()),
    };
    const std::vector<TagsType> authoritativeTags = {
        makeTagType(overlapZoneRanges[0], zoneName("0")),
        makeTagType(overlapZoneRanges[1], zoneName("1"))};
    std::vector<mongo::BSONObj> zones;
    zones.push_back(makeZone(overlapZoneRanges[0], zoneName("0")));
    zones.push_back(makeZone(overlapZoneRanges[1], zoneName("1")));
    ASSERT_THROWS_CODE(validateZones(zones, authoritativeTags), DBException, ErrorCodes::BadValue);
}

TEST(ReshardingUtilTest, AssertDonorOplogIdSerialization) {
    // It's a correctness requirement that `ReshardingDonorOplogId.toBSON` serializes as
    // `{clusterTime: <value>, ts: <value>}`, paying particular attention to the ordering of the
    // fields. The serialization order is defined as the ordering of the fields in the idl file.
    //
    // This is because a document with the same shape as a BSON serialized `ReshardingDonorOplogId`
    // is tacked on as the `_id` to documents in an aggregation pipeline. The pipeline then performs
    // a $gt on the `_id` value with an input `ReshardingDonorOplogId`. If the field ordering were
    // different, the comparison would silently evaluate to the wrong result.
    ReshardingDonorOplogId oplogId(Timestamp::min(), Timestamp::min());
    BSONObj oplogIdObj = oplogId.toBSON();
    BSONObjIterator it(oplogIdObj);
    ASSERT_EQ("clusterTime"_sd, it.next().fieldNameStringData()) << oplogIdObj;
    ASSERT_EQ("ts"_sd, it.next().fieldNameStringData()) << oplogIdObj;
    ASSERT_FALSE(it.more());
}

class ReshardingTxnCloningPipelineTest : public AggregationContextFixture {

protected:
    std::pair<std::deque<DocumentSource::GetNextResult>, std::deque<SessionTxnRecord>>
    makeTransactions(size_t numRetryableWrites,
                     size_t numMultiDocTxns,
                     std::function<Timestamp(size_t)> getTimestamp) {
        std::deque<DocumentSource::GetNextResult> mockResults;
        std::deque<SessionTxnRecord>
            expectedTransactions;  // this will hold the expected result for this test
        for (size_t i = 0; i < numRetryableWrites; i++) {
            auto transaction = SessionTxnRecord(
                makeLogicalSessionIdForTest(), 0, repl::OpTime(getTimestamp(i), 0), Date_t());
            mockResults.emplace_back(Document(transaction.toBSON()));
            expectedTransactions.emplace_back(transaction);
        }
        for (size_t i = 0; i < numMultiDocTxns; i++) {
            auto transaction = SessionTxnRecord(makeLogicalSessionIdForTest(),
                                                0,
                                                repl::OpTime(getTimestamp(numMultiDocTxns), 0),
                                                Date_t());
            transaction.setState(DurableTxnStateEnum::kInProgress);
            mockResults.emplace_back(Document(transaction.toBSON()));
            expectedTransactions.emplace_back(transaction);
        }
        std::sort(expectedTransactions.begin(),
                  expectedTransactions.end(),
                  [](SessionTxnRecord a, SessionTxnRecord b) {
                      return a.getSessionId().toBSON().woCompare(b.getSessionId().toBSON()) < 0;
                  });
        return std::pair(mockResults, expectedTransactions);
    }

    std::unique_ptr<Pipeline, PipelineDeleter> constructPipeline(
        std::deque<DocumentSource::GetNextResult> mockResults,
        Timestamp fetchTimestamp,
        boost::optional<LogicalSessionId> startAfter) {
        // create expression context
        static const NamespaceString _transactionsNss{"config.transactions"};
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(
            new ExpressionContextForTest(getOpCtx(), _transactionsNss));
        expCtx->setResolvedNamespace(_transactionsNss, {_transactionsNss, {}});

        auto pipeline =
            createConfigTxnCloningPipelineForResharding(expCtx, fetchTimestamp, startAfter);
        auto mockSource = DocumentSourceMock::createForTest(mockResults, expCtx);
        pipeline->addInitialSource(mockSource);
        return pipeline;
    }

    bool pipelineMatchesDeque(const std::unique_ptr<Pipeline, PipelineDeleter>& pipeline,
                              const std::deque<SessionTxnRecord>& transactions) {
        auto expected = transactions.begin();
        boost::optional<Document> next;
        for (size_t i = 0; i < transactions.size(); i++) {
            next = pipeline->getNext();
            if (expected == transactions.end() || !next ||
                !expected->toBSON().binaryEqual(next->toBson())) {
                return false;
            }
            expected++;
        }
        return !pipeline->getNext() && expected == transactions.end();
    }
};

TEST_F(ReshardingTxnCloningPipelineTest, TxnPipelineSorted) {
    auto [mockResults, expectedTransactions] =
        makeTransactions(10, 10, [](size_t) { return Timestamp::min(); });

    auto pipeline = constructPipeline(mockResults, Timestamp::max(), boost::none);

    ASSERT(pipelineMatchesDeque(pipeline, expectedTransactions));
}


TEST_F(ReshardingTxnCloningPipelineTest, TxnPipelineBeforeFetchTimestamp) {
    size_t numTransactions = 10;
    Timestamp fetchTimestamp(numTransactions / 2 + 1, 0);
    auto [mockResults, expectedTransactions] = makeTransactions(
        numTransactions, numTransactions, [](size_t i) { return Timestamp(i + 1, 0); });
    expectedTransactions.erase(
        std::remove_if(expectedTransactions.begin(),
                       expectedTransactions.end(),
                       [&fetchTimestamp](SessionTxnRecord transaction) {
                           return transaction.getLastWriteOpTime().getTimestamp() >= fetchTimestamp;
                       }),
        expectedTransactions.end());

    auto pipeline = constructPipeline(mockResults, fetchTimestamp, boost::none);

    ASSERT(pipelineMatchesDeque(pipeline, expectedTransactions));
}


TEST_F(ReshardingTxnCloningPipelineTest, TxnPipelineAfterID) {
    size_t numTransactions = 10;
    auto [mockResults, expectedTransactions] = makeTransactions(
        numTransactions, numTransactions, [](size_t i) { return Timestamp(i + 1, 0); });
    auto middleTransaction = expectedTransactions.begin() + (numTransactions / 2);
    auto middleTransactionSessionId = middleTransaction->getSessionId();
    expectedTransactions.erase(expectedTransactions.begin(), middleTransaction + 1);

    auto pipeline = constructPipeline(mockResults, Timestamp::max(), middleTransactionSessionId);

    ASSERT(pipelineMatchesDeque(pipeline, expectedTransactions));
}

}  // namespace
}  // namespace mongo
