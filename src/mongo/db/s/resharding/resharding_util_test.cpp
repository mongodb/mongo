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
#include "mongo/db/s/resharding_txn_cloner.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/shard_id.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * Mock interface to allow specifiying mock results for the lookup pipeline.
 */
class MockMongoInterface final : public StubMongoProcessInterface {
public:
    MockMongoInterface(std::deque<DocumentSource::GetNextResult> mockResults)
        : _mockResults(std::move(mockResults)) {}

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        Pipeline* ownedPipeline, bool allowTargetingShards = true) final {
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline(
            ownedPipeline, PipelineDeleter(ownedPipeline->getContext()->opCtx));

        pipeline->addInitialSource(
            DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
        return pipeline;
    }

private:
    std::deque<DocumentSource::GetNextResult> _mockResults;
};

repl::MutableOplogEntry makeOplog(const NamespaceString& nss,
                                  const Timestamp& timestamp,
                                  const UUID& uuid,
                                  const ShardId& shardId,
                                  const repl::OpTypeEnum& opType,
                                  const BSONObj& oField,
                                  const BSONObj& o2Field,
                                  const boost::optional<ReshardingDonorOplogId>& _id) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setNss(nss);
    oplogEntry.setTimestamp(timestamp);
    oplogEntry.setWallClockTime(Date_t::now());
    oplogEntry.setTerm(1);
    oplogEntry.setUuid(uuid);
    oplogEntry.setOpType(opType);
    oplogEntry.setObject(oField);

    if (!o2Field.isEmpty()) {
        oplogEntry.setObject2(o2Field);
    }

    if (shardId.isValid()) {
        oplogEntry.setDestinedRecipient(shardId);
    }

    oplogEntry.set_id(Value(_id->toBSON()));

    return oplogEntry;
}

repl::MutableOplogEntry makePrePostImageOplog(const NamespaceString& nss,
                                              const Timestamp& timestamp,
                                              const UUID& uuid,
                                              const ShardId& shardId,
                                              const ReshardingDonorOplogId& _id,
                                              const BSONObj& prePostImage) {
    return makeOplog(nss, timestamp, uuid, shardId, repl::OpTypeEnum::kNoop, prePostImage, {}, _id);
}

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

class ReshardingAggTest : public AggregationContextFixture {
protected:
    const NamespaceString& localOplogBufferNss() {
        return _localOplogBufferNss;
    }

    boost::intrusive_ptr<ExpressionContextForTest> createExpressionContext() {
        NamespaceString slimNss =
            NamespaceString("local.system.resharding.slimOplogForGraphLookup");

        boost::intrusive_ptr<ExpressionContextForTest> expCtx(
            new ExpressionContextForTest(getOpCtx(), _localOplogBufferNss));
        expCtx->setResolvedNamespace(_localOplogBufferNss, {_localOplogBufferNss, {}});
        expCtx->setResolvedNamespace(_remoteOplogNss, {_remoteOplogNss, {}});
        expCtx->setResolvedNamespace(slimNss,
                                     {slimNss, std::vector<BSONObj>{getSlimOplogPipeline()}});
        return expCtx;
    }

    /************************************************************************************
     * These set of helper function generate pre-made oplogs with the following timestamps:
     *
     * deletePreImage: ts(7, 35)
     * updatePostImage: ts(10, 15)
     * insert: ts(25, 345)
     * update: ts(30, 16)
     * delete: ts(66, 86)
     */

    repl::MutableOplogEntry makeInsertOplog() {
        const Timestamp insertTs(25, 345);
        const ReshardingDonorOplogId insertId(insertTs, insertTs);
        return makeOplog(_crudNss,
                         insertTs,
                         _reshardingCollUUID,
                         _destinedRecipient,
                         repl::OpTypeEnum::kInsert,
                         BSON("x" << 1),
                         {},
                         insertId);
    }

    repl::MutableOplogEntry makeUpdateOplog() {
        const Timestamp updateWithPostOplogTs(30, 16);
        const ReshardingDonorOplogId updateWithPostOplogId(updateWithPostOplogTs,
                                                           updateWithPostOplogTs);
        return makeOplog(_crudNss,
                         updateWithPostOplogTs,
                         _reshardingCollUUID,
                         _destinedRecipient,
                         repl::OpTypeEnum::kUpdate,
                         BSON("$set" << BSON("y" << 1)),
                         BSON("post" << 1),
                         updateWithPostOplogId);
    }

    repl::MutableOplogEntry makeDeleteOplog() {
        const Timestamp deleteWithPreOplogTs(66, 86);
        const ReshardingDonorOplogId deleteWithPreOplogId(deleteWithPreOplogTs,
                                                          deleteWithPreOplogTs);
        return makeOplog(_crudNss,
                         deleteWithPreOplogTs,
                         _reshardingCollUUID,
                         _destinedRecipient,
                         repl::OpTypeEnum::kDelete,
                         BSON("pre" << 1),
                         {},
                         deleteWithPreOplogId);
    }

    /**
     * Returns (postImageOplog, updateOplog) pair.
     */
    std::pair<repl::MutableOplogEntry, repl::MutableOplogEntry> makeUpdateWithPostImage() {
        const Timestamp postImageTs(10, 5);
        const ReshardingDonorOplogId postImageId(postImageTs, postImageTs);
        auto postImageOplog = makePrePostImageOplog(_crudNss,
                                                    postImageTs,
                                                    _reshardingCollUUID,
                                                    _destinedRecipient,
                                                    postImageId,
                                                    BSON("post" << 1 << "y" << 4));

        auto updateWithPostOplog = makeUpdateOplog();
        updateWithPostOplog.setPostImageOpTime(repl::OpTime(postImageTs, _term));
        return std::make_pair(postImageOplog, updateWithPostOplog);
    }

    /**
     * Returns (preImageOplog, deleteOplog) pair.
     */
    std::pair<repl::MutableOplogEntry, repl::MutableOplogEntry> makeDeleteWithPreImage() {
        const Timestamp preImageTs(7, 35);
        const ReshardingDonorOplogId preImageId(preImageTs, preImageTs);
        auto preImageOplog = makePrePostImageOplog(_crudNss,
                                                   preImageTs,
                                                   _reshardingCollUUID,
                                                   _destinedRecipient,
                                                   preImageId,
                                                   BSON("pre" << 1 << "z" << 4));

        auto deleteWithPreOplog = makeDeleteOplog();
        deleteWithPreOplog.setPreImageOpTime(repl::OpTime(preImageTs, _term));

        return std::make_pair(preImageOplog, deleteWithPreOplog);
    }

    ReshardingDonorOplogId getOplogId(const repl::MutableOplogEntry& oplog) {
        return ReshardingDonorOplogId::parse(IDLParserErrorContext("ReshardingAggTest::getOplogId"),
                                             oplog.get_id()->getDocument().toBson());
    }

    BSONObj addExpectedFields(const repl::MutableOplogEntry& oplog,
                              const boost::optional<repl::MutableOplogEntry>& chainedEntry) {
        BSONObjBuilder builder(oplog.toBSON());

        BSONArrayBuilder arrayBuilder(builder.subarrayStart(kReshardingOplogPrePostImageOps));
        if (chainedEntry) {
            arrayBuilder.append(chainedEntry->toBSON());
        }
        arrayBuilder.done();

        return builder.obj();
    }

    const NamespaceString _remoteOplogNss{"local.oplog.rs"};
    const NamespaceString _localOplogBufferNss{"config.localReshardingOplogBuffer.xxx.yyy"};
    const NamespaceString _crudNss{"test.foo"};
    // Use a constant value so unittests can store oplog entries as extended json strings in code.
    const UUID _reshardingCollUUID =
        fassert(5074001, UUID::parse("8926ba8e-611a-42c2-bb1a-3b7819f610ed"));
    // Also referenced via strings in code.
    const ShardId _destinedRecipient = {"shard1"};
    const int _term{20};
};

TEST_F(ReshardingAggTest, OplogPipelineBasicCRUDOnly) {
    auto insertOplog = makeInsertOplog();
    auto updateOplog = makeUpdateOplog();
    auto deleteOplog = makeDeleteOplog();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(updateOplog.toBSON()));
    mockResults.emplace_back(Document(deleteOplog.toBSON()));

    // Mock lookup collection document souce.
    auto expCtx = createExpressionContext();
    expCtx->ns = localOplogBufferNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, boost::none, false);

    // Mock non-lookup collection document source.
    auto mockSource = DocumentSourceMock::createForTest(mockResults, expCtx);
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(insertOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(deleteOplog, boost::none), next->toBson());

    ASSERT(!pipeline->getNext());
}

/**
 * Test with 3 oplog: insert -> update -> delete, then resume from point after insert.
 */
TEST_F(ReshardingAggTest, OplogPipelineWithResumeToken) {
    auto insertOplog = makeInsertOplog();
    auto updateOplog = makeUpdateOplog();
    auto deleteOplog = makeDeleteOplog();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(updateOplog.toBSON()));
    mockResults.emplace_back(Document(deleteOplog.toBSON()));

    // Mock lookup collection document souce.
    auto expCtx = createExpressionContext();
    expCtx->ns = localOplogBufferNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, getOplogId(insertOplog), false);

    // Mock non-lookup collection document source.
    auto mockSource = DocumentSourceMock::createForTest(mockResults, expCtx);
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(deleteOplog, boost::none), next->toBson());

    ASSERT(!pipeline->getNext());
}

/**
 * Test with 3 oplog: insert -> update -> delete, then resume from point after insert.
 */
TEST_F(ReshardingAggTest, OplogPipelineWithResumeTokenClusterTimeNotEqualTs) {
    auto modifyClusterTsTo = [&](repl::MutableOplogEntry& oplog, const Timestamp& ts) {
        auto newId = getOplogId(oplog);
        newId.setClusterTime(ts);
        oplog.set_id(Value(newId.toBSON()));
    };

    auto insertOplog = makeInsertOplog();
    modifyClusterTsTo(insertOplog, Timestamp(33, 46));
    auto updateOplog = makeUpdateOplog();
    modifyClusterTsTo(updateOplog, Timestamp(44, 55));
    auto deleteOplog = makeDeleteOplog();
    modifyClusterTsTo(deleteOplog, Timestamp(79, 80));

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(updateOplog.toBSON()));
    mockResults.emplace_back(Document(deleteOplog.toBSON()));

    // Mock lookup collection document souce.
    auto expCtx = createExpressionContext();
    expCtx->ns = localOplogBufferNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, getOplogId(insertOplog), false);

    // Mock non-lookup collection document source.
    auto mockSource = DocumentSourceMock::createForTest(mockResults, expCtx);
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(deleteOplog, boost::none), next->toBson());

    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingAggTest, OplogPipelineWithPostImage) {
    auto insertOplog = makeInsertOplog();

    repl::MutableOplogEntry postImageOplog, updateWithPostOplog;
    std::tie(postImageOplog, updateWithPostOplog) = makeUpdateWithPostImage();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(postImageOplog.toBSON()));
    mockResults.emplace_back(Document(updateWithPostOplog.toBSON()));

    // Mock lookup collection document souce.
    auto expCtx = createExpressionContext();
    expCtx->ns = localOplogBufferNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, boost::none, false);

    // Mock non-lookup collection document source.
    auto mockSource = DocumentSourceMock::createForTest(mockResults, expCtx);
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(postImageOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(insertOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateWithPostOplog, postImageOplog),
                             next->toBson());

    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingAggTest, OplogPipelineWithLargeBSONPostImage) {
    auto insertOplog = makeInsertOplog();

    repl::MutableOplogEntry postImageOplog, updateWithPostOplog;
    std::tie(postImageOplog, updateWithPostOplog) = makeUpdateWithPostImage();

    // Modify default fixture docs with large BSON documents.
    const std::string::size_type bigSize = 12 * 1024 * 1024;
    std::string bigStr(bigSize, 'x');
    postImageOplog.setObject(BSON("bigVal" << bigStr));
    updateWithPostOplog.setObject2(BSON("bigVal" << bigStr));

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(postImageOplog.toBSON()));
    mockResults.emplace_back(Document(updateWithPostOplog.toBSON()));

    // Mock lookup collection document souce.
    auto expCtx = createExpressionContext();
    expCtx->ns = localOplogBufferNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, boost::none, false);

    // Mock non-lookup collection document source.
    auto mockSource = DocumentSourceMock::createForTest(mockResults, expCtx);
    pipeline->addInitialSource(mockSource);

    // Check only _id because attempting to call toBson will trigger BSON too large assertion.
    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(postImageOplog.get_id()->getDocument().toBson(),
                             next->getField("_id").getDocument().toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(insertOplog.get_id()->getDocument().toBson(),
                             next->getField("_id").getDocument().toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(updateWithPostOplog.get_id()->getDocument().toBson(),
                             next->getField("_id").getDocument().toBson());

    ASSERT(!pipeline->getNext());
}

/**
 * Test with 3 oplog: postImage -> insert -> update, then resume from point after postImage.
 */
TEST_F(ReshardingAggTest, OplogPipelineResumeAfterPostImage) {
    auto insertOplog = makeInsertOplog();

    repl::MutableOplogEntry postImageOplog, updateWithPostOplog;
    std::tie(postImageOplog, updateWithPostOplog) = makeUpdateWithPostImage();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(postImageOplog.toBSON()));
    mockResults.emplace_back(Document(updateWithPostOplog.toBSON()));

    // Mock lookup collection document souce.
    auto expCtx = createExpressionContext();
    expCtx->ns = localOplogBufferNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, getOplogId(postImageOplog), false);

    // Mock non-lookup collection document source.
    auto mockSource = DocumentSourceMock::createForTest(mockResults, expCtx);
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(insertOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateWithPostOplog, postImageOplog),
                             next->toBson());

    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingAggTest, OplogPipelineWithPreImage) {
    auto insertOplog = makeInsertOplog();

    repl::MutableOplogEntry preImageOplog, deleteWithPreOplog;
    std::tie(preImageOplog, deleteWithPreOplog) = makeDeleteWithPreImage();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(preImageOplog.toBSON()));
    mockResults.emplace_back(Document(deleteWithPreOplog.toBSON()));

    // Mock lookup collection document souce.
    auto expCtx = createExpressionContext();
    expCtx->ns = localOplogBufferNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, boost::none, false);

    // Mock non-lookup collection document source.
    auto mockSource = DocumentSourceMock::createForTest(mockResults, expCtx);
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(preImageOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(insertOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(deleteWithPreOplog, preImageOplog), next->toBson());

    ASSERT(!pipeline->getNext());
}

/**
 * Oplog _id order in this test is:
 * delPreImage -> updatePostImage -> unrelatedInsert -> update -> delete
 */
TEST_F(ReshardingAggTest, OplogPipelineWithPreAndPostImage) {
    auto insertOplog = makeInsertOplog();

    repl::MutableOplogEntry postImageOplog, updateWithPostOplog, preImageOplog, deleteWithPreOplog;
    std::tie(postImageOplog, updateWithPostOplog) = makeUpdateWithPostImage();
    std::tie(preImageOplog, deleteWithPreOplog) = makeDeleteWithPreImage();

    std::deque<DocumentSource::GetNextResult> mockResults;
    mockResults.emplace_back(Document(insertOplog.toBSON()));
    mockResults.emplace_back(Document(postImageOplog.toBSON()));
    mockResults.emplace_back(Document(updateWithPostOplog.toBSON()));
    mockResults.emplace_back(Document(preImageOplog.toBSON()));
    mockResults.emplace_back(Document(deleteWithPreOplog.toBSON()));

    // Mock lookup collection document souce.
    auto expCtx = createExpressionContext();
    expCtx->ns = localOplogBufferNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, boost::none, false);

    // Mock non-lookup collection document source.
    auto mockSource = DocumentSourceMock::createForTest(mockResults, expCtx);
    pipeline->addInitialSource(mockSource);

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(preImageOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(postImageOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(insertOplog, boost::none), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateWithPostOplog, postImageOplog),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(deleteWithPreOplog, preImageOplog), next->toBson());

    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingAggTest, VerifyPipelineOutputHasOplogSchema) {
    repl::MutableOplogEntry insertOplog = makeInsertOplog();
    auto updateOplog = makeUpdateOplog();
    auto deleteOplog = makeDeleteOplog();

    const bool debug = false;
    if (debug) {
        std::cout << "Oplog. Insert:" << std::endl
                  << insertOplog.toBSON() << std::endl
                  << "Update:" << std::endl
                  << updateOplog.toBSON() << std::endl
                  << "Delete:" << deleteOplog.toBSON();
    }

    std::deque<DocumentSource::GetNextResult> pipelineSource = {Document(insertOplog.toBSON()),
                                                                Document(updateOplog.toBSON()),
                                                                Document(deleteOplog.toBSON())};

    boost::intrusive_ptr<ExpressionContext> expCtx = createExpressionContext();
    expCtx->ns = _remoteOplogNss;
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(pipelineSource);

    const bool doesDonorOwnMinKeyChunk = false;
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline = createOplogFetchingPipelineForResharding(
        expCtx,
        // Use the test to also exercise the stages for resuming. The timestamp passed in is
        // excluded from the results.
        ReshardingDonorOplogId(insertOplog.getTimestamp(), insertOplog.getTimestamp()),
        _reshardingCollUUID,
        {_destinedRecipient},
        doesDonorOwnMinKeyChunk);
    auto bsonPipeline = pipeline->serializeToBson();
    if (debug) {
        std::cout << "Pipeline stages:" << std::endl;
        for (std::size_t idx = 0; idx < bsonPipeline.size(); ++idx) {
            auto& stage = bsonPipeline[idx];
            std::cout << stage.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
        }
    }

    pipeline->addInitialSource(DocumentSourceMock::createForTest(pipelineSource, expCtx));
    boost::optional<Document> doc = pipeline->getNext();
    ASSERT(doc);
    auto bsonDoc = doc->toBson();
    if (debug) {
        std::cout << "Doc:" << std::endl
                  << bsonDoc.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
    }
    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(bsonDoc));
    // The insert oplog entry is excluded, we first expect the update oplog entry.
    ASSERT_EQ(updateOplog.getTimestamp(), oplogEntry.getTimestamp()) << bsonDoc;

    doc = pipeline->getNext();
    ASSERT(doc);
    bsonDoc = doc->toBson();
    if (debug) {
        std::cout << "Doc:" << std::endl
                  << bsonDoc.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
    }
    oplogEntry = uassertStatusOK(repl::OplogEntry::parse(bsonDoc));
    ASSERT_EQ(deleteOplog.getTimestamp(), oplogEntry.getTimestamp()) << bsonDoc;

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(ReshardingAggTest, VerifyPipelinePreparedTxn) {
    // Create a prepared transaction with three inserts. The pipeline matches on `destinedRecipient:
    // shard1`, which targets two of the inserts.
    BSONObj prepareEntry = fromjson(
        "{ 'lsid' : { 'id' : { '$binary' : 'ZscSybogRx+iPUemRZVojA==', '$type' : '04' }, "
        "             'uid' : { '$binary' : '47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=', "
        "                       '$type' : '00' } }, "
        "  'txnNumber' : { '$numberLong' : '0' }, "
        "  'op' : 'c', 'ns' : 'admin.$cmd', 'o' : { 'applyOps' : [ "
        "    { 'op' : 'i', 'ns' : 'test.foo', 'ui' : { '$binary' : 'iSa6jmEaQsK7Gjt4GfYQ7Q==', "
        "                                              '$type' : '04' }, "
        "      'destinedRecipient' : 'shard1', "
        "      'o' : { '_id' : { '$oid' : '5f5fcf57a8da34eec240cbd6' }, 'x' : 1000 } }, "
        "    { 'op' : 'i', 'ns' : 'test.foo', 'ui' : { '$binary' : 'iSa6jmEaQsK7Gjt4GfYQ7Q==', "
        "                                              '$type' : '04' }, "
        "      'destinedRecipient' : 'shard1', "
        "      'o' : { '_id' : { '$oid' : '5f5fcf57a8da34eec240cbd7' }, 'x' : 5005 } }, "
        "    { 'op' : 'i', 'ns' : 'test.foo', 'ui' : { '$binary' : 'iSa6jmEaQsK7Gjt4GfYQ7Q==', "
        "                                              '$type' : '04' }, "
        "      'destinedRecipient' : 'shard2', "
        "      'o' : { '_id' : { '$oid' : '5f5fcf57a8da34eec240cbd8' }, 'x' : 6002 } } ], "
        "    'prepare' : true }, "
        "  'ts' : { '$timestamp' : { 't' : 1600114519, 'i' : 7 } }, "
        "  't' : { '$numberLong' : '1' }, 'wall' : { '$date' : 900 }, "
        "  'v' : { '$numberLong' : '2' }, "
        "  'prevOpTime' : { 'ts' : { '$timestamp' : { 't' : 0, 'i' : 0 } }, "
        "    't' : { '$numberLong' : '-1' } } }");
    BSONObj commitEntry = fromjson(
        "{ 'lsid' : { 'id' : { '$binary' : 'ZscSybogRx+iPUemRZVojA==', '$type' : '04' }, "
        "             'uid' : { '$binary' : '47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=', "
        "                       '$type' : '00' } }, "
        "  'txnNumber' : { '$numberLong' : '0' }, "
        "  'op' : 'c', 'ns' : 'admin.$cmd', "
        "  'o' : { 'commitTransaction' : 1, "
        "          'commitTimestamp' : { '$timestamp' : { 't' : 1600114519, 'i' : 7 } } }, "
        "  'ts' : { '$timestamp' : { 't' : 1600114519, 'i' : 9 } }, "
        "  't' : { '$numberLong' : '1' }, 'wall' : { '$date' : 1000 }, "
        "  'v' : { '$numberLong' : '2' }, "
        "  'prevOpTime' : { 'ts' : { '$timestamp' : { 't' : 1600114519, 'i' : 7 } }, "
        "    't' : { '$numberLong' : '1' } } }");

    const Timestamp clusterTime = commitEntry["ts"].timestamp();
    const Timestamp commitTime = commitEntry["o"].Obj()["commitTimestamp"].timestamp();

    const bool debug = false;
    if (debug) {
        std::cout << "Prepare:" << std::endl
                  << prepareEntry.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
        std::cout << "Commit:" << std::endl
                  << commitEntry.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
    }

    std::deque<DocumentSource::GetNextResult> pipelineSource = {Document(prepareEntry),
                                                                Document(commitEntry)};

    boost::intrusive_ptr<ExpressionContext> expCtx = createExpressionContext();
    // Set up the oplog collection state for $lookup and $graphLookup calls.
    expCtx->ns = _remoteOplogNss;
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(pipelineSource);

    const bool doesDonorOwnMinKeyChunk = false;
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline = createOplogFetchingPipelineForResharding(
        expCtx,
        ReshardingDonorOplogId(Timestamp::min(), Timestamp::min()),
        _reshardingCollUUID,
        {_destinedRecipient},
        doesDonorOwnMinKeyChunk);
    if (debug) {
        std::cout << "Pipeline stages:" << std::endl;
        // This is can be changed to process a prefix of the pipeline for debugging.
        const std::size_t numStagesToKeep = pipeline->getSources().size();
        pipeline->getSources().resize(numStagesToKeep);
        auto bsonPipeline = pipeline->serializeToBson();
        for (std::size_t idx = 0; idx < bsonPipeline.size(); ++idx) {
            auto& stage = bsonPipeline[idx];
            std::cout << stage.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
        }
    }

    // Set up the initial input into the pipeline.
    pipeline->addInitialSource(DocumentSourceMock::createForTest(pipelineSource, expCtx));

    // The first document should be `prepare: true` and contain two inserts.
    boost::optional<Document> doc = pipeline->getNext();
    ASSERT(doc);
    auto bsonDoc = doc->toBson();
    if (debug) {
        std::cout << "Prepare doc:" << std::endl
                  << bsonDoc.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
    }
    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(bsonDoc));
    ASSERT_TRUE(oplogEntry.shouldPrepare()) << bsonDoc;
    ASSERT_FALSE(oplogEntry.isPartialTransaction()) << bsonDoc;
    ASSERT_EQ(clusterTime, oplogEntry.get_id()->getDocument()["clusterTime"].getTimestamp())
        << bsonDoc;
    ASSERT_EQ(2, oplogEntry.getObject()["applyOps"].Obj().nFields()) << bsonDoc;

    // The second document should be `o.commitTransaction: 1`.
    doc = pipeline->getNext();
    ASSERT(doc);
    bsonDoc = doc->toBson();
    if (debug) {
        std::cout << "Commit doc:" << std::endl
                  << bsonDoc.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
    }
    oplogEntry = uassertStatusOK(repl::OplogEntry::parse(bsonDoc));
    ASSERT_TRUE(oplogEntry.isPreparedCommit()) << bsonDoc;
    ASSERT_EQ(clusterTime, oplogEntry.get_id()->getDocument()["clusterTime"].getTimestamp())
        << bsonDoc;
    ASSERT_EQ(commitTime, oplogEntry.getObject()["commitTimestamp"].timestamp()) << bsonDoc;
}

TEST_F(ReshardingAggTest, VerifyPipelineLargeTxn) {
    std::vector<BSONObj> oplogEntries = {
        fromjson(
            "{ 'lsid' : { 'id' : { '$binary' : 'IFVYZej6QVmC/JiXojJUIQ==', '$type' : '04' }, "
            "             'uid' : { '$binary' : '47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=',"
            "                       '$type' : '00' } }, "
            "  'txnNumber' : { '$numberLong' : '0' }, "
            "  'op' : 'c', 'ns' : 'admin.$cmd', 'o' : { 'applyOps' : [ "
            "    { 'op' : 'i', 'destinedRecipient' : 'shard1', 'ns' : 'test.foo', "
            "      'ui' : { '$binary' : 'iSa6jmEaQsK7Gjt4GfYQ7Q==', '$type' : '04' }, "
            "      'o' : { '_id' : { '$oid' : '5f60260d257c51cea52d22fe' }, 'x' : 1 } } ], "
            "    'partialTxn' : true }, "
            "  'ts' : { '$timestamp' : { 't' : 1600136717, 'i' : 1 } }, "
            "  't' : { '$numberLong' : '1' }, 'wall' : { '$date' : 1000 }, "
            "  'v' : { '$numberLong' : '2' }, "
            "  'prevOpTime' : { 'ts' : { '$timestamp' : { 't' : 0, 'i' : 0 } }, "
            "                   't' : { '$numberLong' : '-1' } } }"),
        fromjson(
            "{ 'lsid' : { 'id' : { '$binary' : 'IFVYZej6QVmC/JiXojJUIQ==', '$type' : '04' }, "
            "             'uid' : { '$binary' : '47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=',"
            "                       '$type' : '00' } }, "
            "  'txnNumber' : { '$numberLong' : '0' }, "
            "  'op' : 'c', 'ns' : 'admin.$cmd', 'o' : { 'applyOps' : [ "
            "    { 'op' : 'i', 'destinedRecipient' : 'shard2', 'ns' : 'test.foo', "
            "      'ui' : { '$binary' : 'iSa6jmEaQsK7Gjt4GfYQ7Q==', '$type' : '04' }, "
            "      'o' : { '_id' : { '$oid' : '5f60260d257c51cea52d22ff' }, 'x' : -1 } }, "
            "    { 'op' : 'i', 'destinedRecipient' : 'shard2', 'ns' : 'test.foo', "
            "      'ui' : { '$binary' : 'iSa6jmEaQsK7Gjt4GfYQ7Q==', '$type' : '04' }, "
            "      'o' : { '_id' : { '$oid' : '5f60260d257c51cea52d2300' }, 'x' : 1000 } } ], "
            "    'partialTxn' : true }, "
            "  'ts' : { '$timestamp' : { 't' : 1600136717, 'i' : 2 } },"
            "  't' : { '$numberLong' : '1' }, 'wall' : { '$date' : 900 }, "
            "  'v' : { '$numberLong' : '2' }, "
            "  'prevOpTime' : { 'ts' : { '$timestamp' : { 't' : 1600136717, 'i' : 1 } }, "
            "                   't' : { '$numberLong' : '1' } } }"),
        fromjson(
            "{ 'lsid' : { 'id' : { '$binary' : 'IFVYZej6QVmC/JiXojJUIQ==', '$type' : '04' }, "
            "             'uid' : { '$binary' : '47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=', "
            "                       '$type' : '00' } }, "
            "  'txnNumber' : { '$numberLong' : '0' }, "
            "  'op' : 'c', 'ns' : 'admin.$cmd', 'o' : { 'applyOps' : [ "
            "    { 'op' : 'i', 'destinedRecipient' : 'shard2', 'ns' : 'test.foo', "
            "      'ui' : { '$binary' : 'iSa6jmEaQsK7Gjt4GfYQ7Q==', '$type' : '04' }, "
            "      'o' : { '_id' : { '$oid' : '5f60260d257c51cea52d2301' }, 'x' : 5005 } }, "
            "    { 'op' : 'i', 'destinedRecipient' : 'shard1', 'ns' : 'test.foo', "
            "      'ui' : { '$binary' : 'iSa6jmEaQsK7Gjt4GfYQ7Q==', '$type' : '04' }, "
            "      'o' : { '_id' : { '$oid' : '5f60260d257c51cea52d2302' }, 'x' : 6002 } } ], "
            "    'count' : { '$numberLong' : '6' } }, "
            "  'ts' : { '$timestamp' : { 't' : 1600136717, 'i' : 3 } }, "
            "  't' : { '$numberLong' : '1' }, 'wall' : { '$date' : 800 }, "
            "  'v' : { '$numberLong' : '2' }, "
            "  'prevOpTime' : { 'ts' : { '$timestamp' : { 't' : 1600136717, 'i' : 2 } }, "
            "                   't' : { '$numberLong' : '1' } } }")};

    const Timestamp clusterTime = oplogEntries[2]["ts"].timestamp();

    const bool debug = true;
    if (debug) {
        std::cout << "Parsed oplog entries:" << std::endl;
        for (std::size_t idx = 0; idx < oplogEntries.size(); ++idx) {
            std::cout << "Idx:" << idx << std::endl
                      << oplogEntries[idx].jsonString(ExtendedRelaxedV2_0_0, true, false)
                      << std::endl;
        }
    }

    std::deque<DocumentSource::GetNextResult> pipelineSource = {
        Document(oplogEntries[0]), Document(oplogEntries[1]), Document(oplogEntries[2])};

    boost::intrusive_ptr<ExpressionContext> expCtx = createExpressionContext();
    // Set up the oplog collection state for $lookup and $graphLookup calls.
    expCtx->ns = _remoteOplogNss;
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(pipelineSource);

    const bool doesDonorOwnMinKeyChunk = false;
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline = createOplogFetchingPipelineForResharding(
        expCtx,
        ReshardingDonorOplogId(Timestamp::min(), Timestamp::min()),
        _reshardingCollUUID,
        {_destinedRecipient},
        doesDonorOwnMinKeyChunk);
    if (debug) {
        std::cout << "Pipeline stages:" << std::endl;
        // This is can be changed to process a prefix of the pipeline for debugging.
        const std::size_t numStagesToKeep = pipeline->getSources().size();
        pipeline->getSources().resize(numStagesToKeep);
        auto bsonPipeline = pipeline->serializeToBson();
        for (std::size_t idx = 0; idx < bsonPipeline.size(); ++idx) {
            auto& stage = bsonPipeline[idx];
            std::cout << stage.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
        }
    }

    // Set up the initial input into the pipeline.
    pipeline->addInitialSource(DocumentSourceMock::createForTest(pipelineSource, expCtx));

    // The first document contains one insert.
    boost::optional<Document> doc = pipeline->getNext();
    ASSERT(doc);
    auto bsonDoc = doc->toBson();
    if (debug) {
        std::cout << bsonDoc.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
    }
    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(bsonDoc));
    ASSERT_TRUE(oplogEntry.isPartialTransaction()) << bsonDoc;
    ASSERT_EQ(1, oplogEntry.getObject()["applyOps"].Obj().nFields()) << bsonDoc;
    ASSERT_EQ(clusterTime, oplogEntry.get_id()->getDocument()["clusterTime"].getTimestamp())
        << bsonDoc;
    ASSERT_EQ(oplogEntries[0]["ts"].timestamp(), oplogEntry.getTimestamp()) << bsonDoc;

    // The second piece of the large transaction only head documents for a different recipient. The
    // oplog entry still passes through, but with an empty `applyOps`.
    doc = pipeline->getNext();
    ASSERT(doc);
    bsonDoc = doc->toBson();
    if (debug) {
        std::cout << bsonDoc.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
    }
    oplogEntry = uassertStatusOK(repl::OplogEntry::parse(bsonDoc));
    ASSERT_TRUE(oplogEntry.isPartialTransaction()) << bsonDoc;
    ASSERT_EQ(0, oplogEntry.getObject()["applyOps"].Obj().nFields()) << bsonDoc;
    ASSERT_EQ(clusterTime, oplogEntry.get_id()->getDocument()["clusterTime"].getTimestamp())
        << bsonDoc;
    ASSERT_EQ(oplogEntries[1]["ts"].timestamp(), oplogEntry.getTimestamp()) << bsonDoc;

    // The last oplog entry for the large transaction. This contains one more insert for this
    // recipient.
    doc = pipeline->getNext();
    ASSERT(doc);
    bsonDoc = doc->toBson();
    if (debug) {
        std::cout << bsonDoc.jsonString(ExtendedRelaxedV2_0_0, true, false) << std::endl;
    }
    oplogEntry = uassertStatusOK(repl::OplogEntry::parse(bsonDoc));
    ASSERT_FALSE(oplogEntry.isPartialTransaction()) << bsonDoc;
    ASSERT_EQ(1, oplogEntry.getObject()["applyOps"].Obj().nFields()) << bsonDoc;
    ASSERT_EQ(clusterTime, oplogEntry.get_id()->getDocument()["clusterTime"].getTimestamp())
        << bsonDoc;
    ASSERT_EQ(oplogEntries[2]["ts"].timestamp(), oplogEntry.getTimestamp()) << bsonDoc;
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
