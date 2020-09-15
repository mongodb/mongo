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
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/s/catalog/type_shard.h"
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
                                  const UUID& uuid,
                                  const repl::OpTypeEnum& opType,
                                  const BSONObj& oField,
                                  const BSONObj& o2Field,
                                  const boost::optional<ReshardingDonorOplogId>& _id) {
    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setNss(nss);
    oplogEntry.setUuid(uuid);
    oplogEntry.setOpType(opType);
    oplogEntry.setObject(oField);

    if (!o2Field.isEmpty()) {
        oplogEntry.setObject2(o2Field);
    }

    oplogEntry.setOpTimeAndWallTimeBase(repl::OpTimeAndWallTimeBase({}, {}));
    oplogEntry.set_id(Value(_id->toBSON()));

    return oplogEntry;
}

repl::MutableOplogEntry makePrePostImageOplog(const NamespaceString& nss,
                                              const UUID& uuid,
                                              const ReshardingDonorOplogId& _id,
                                              const BSONObj& prePostImage) {
    return makeOplog(nss, uuid, repl::OpTypeEnum::kNoop, prePostImage, {}, _id);
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

class ReshardingAggTest : public AggregationContextFixture {
protected:
    const NamespaceString& reshardingOplogNss() {
        return _oplogNss;
    }

    boost::intrusive_ptr<ExpressionContextForTest> createExpressionContext() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(
            new ExpressionContextForTest(getOpCtx(), _oplogNss));
        expCtx->setResolvedNamespace(_oplogNss, {_oplogNss, {}});
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
        return makeOplog(_crudNss, _uuid, repl::OpTypeEnum::kInsert, BSON("x" << 1), {}, insertId);
    }

    repl::MutableOplogEntry makeUpdateOplog() {
        const Timestamp updateWithPostOplogTs(30, 16);
        const ReshardingDonorOplogId updateWithPostOplogId(updateWithPostOplogTs,
                                                           updateWithPostOplogTs);
        return makeOplog(_crudNss,
                         _uuid,
                         repl::OpTypeEnum::kUpdate,
                         BSON("$set" << BSON("y" << 1)),
                         BSON("post" << 1),
                         updateWithPostOplogId);
    }

    repl::MutableOplogEntry makeDeleteOplog() {
        const Timestamp deleteWithPreOplogTs(66, 86);
        const ReshardingDonorOplogId deleteWithPreOplogId(deleteWithPreOplogTs,
                                                          deleteWithPreOplogTs);
        return makeOplog(
            _crudNss, _uuid, repl::OpTypeEnum::kDelete, BSON("pre" << 1), {}, deleteWithPreOplogId);
    }

    /**
     * Returns (postImageOplog, updateOplog) pair.
     */
    std::pair<repl::MutableOplogEntry, repl::MutableOplogEntry> makeUpdateWithPostImage() {
        const Timestamp postImageTs(10, 5);
        const ReshardingDonorOplogId postImageId(postImageTs, postImageTs);
        auto postImageOplog =
            makePrePostImageOplog(_crudNss, _uuid, postImageId, BSON("post" << 1 << "y" << 4));

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
        auto preImageOplog =
            makePrePostImageOplog(_crudNss, _uuid, preImageId, BSON("pre" << 1 << "z" << 4));

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

private:
    const NamespaceString _oplogNss{"config.localReshardingOplogBuffer.xxx.yyy"};
    const NamespaceString _crudNss{"test.foo"};
    const UUID _uuid{UUID::gen()};
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
    expCtx->ns = reshardingOplogNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, boost::none);

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
    expCtx->ns = reshardingOplogNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, getOplogId(insertOplog));

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
    expCtx->ns = reshardingOplogNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, getOplogId(insertOplog));

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
    expCtx->ns = reshardingOplogNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, boost::none);

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
    expCtx->ns = reshardingOplogNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, boost::none);

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
    expCtx->ns = reshardingOplogNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, getOplogId(postImageOplog));

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
    expCtx->ns = reshardingOplogNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, boost::none);

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
    expCtx->ns = reshardingOplogNss();
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockResults);

    auto pipeline = createAggForReshardingOplogBuffer(expCtx, boost::none);

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

class ReshardingTxnCloningPipelineTest : public AggregationContextFixture {

protected:
    std::pair<std::deque<DocumentSource::GetNextResult>, std::deque<SessionTxnRecord>>
    makeTransactions(size_t numTransactions, std::function<Timestamp(size_t)> getTimestamp) {
        std::deque<DocumentSource::GetNextResult> mockResults;
        std::deque<SessionTxnRecord>
            expectedTransactions;  // this will hold the expected result for this test
        for (size_t i = 0; i < numTransactions; i++) {
            auto transaction = SessionTxnRecord(
                makeLogicalSessionIdForTest(), 0, repl::OpTime(getTimestamp(i), 0), Date_t());
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
        makeTransactions(10, [](size_t) { return Timestamp::min(); });

    auto pipeline = constructPipeline(mockResults, Timestamp::max(), boost::none);

    ASSERT(pipelineMatchesDeque(pipeline, expectedTransactions));
}


TEST_F(ReshardingTxnCloningPipelineTest, TxnPipelineBeforeFetchTimestamp) {
    size_t numTransactions = 10;
    Timestamp fetchTimestamp(numTransactions / 2 + 1, 0);
    auto [mockResults, expectedTransactions] =
        makeTransactions(numTransactions, [](size_t i) { return Timestamp(i + 1, 0); });
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
    auto [mockResults, expectedTransactions] =
        makeTransactions(numTransactions, [](size_t i) { return Timestamp(i + 1, 0); });
    auto middleTransaction = expectedTransactions.begin() + (numTransactions / 2);
    expectedTransactions.erase(expectedTransactions.begin(), middleTransaction + 1);

    auto pipeline =
        constructPipeline(mockResults, Timestamp::max(), middleTransaction->getSessionId());

    ASSERT(pipelineMatchesDeque(pipeline, expectedTransactions));
}

class ReshardingCollectionCloneTest : public AggregationContextFixture {
protected:
    const NamespaceString& sourceNss() {
        return _sourceNss;
    }

    boost::intrusive_ptr<ExpressionContextForTest> createExpressionContext(
        NamespaceString sourceNss) {
        _sourceNss = sourceNss;
        NamespaceString foreignNss("config.cache.chunks." + sourceNss.toString());
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(
            new ExpressionContextForTest(getOpCtx(), sourceNss));
        expCtx->setResolvedNamespace(sourceNss, {sourceNss, {}});
        expCtx->setResolvedNamespace(foreignNss, {foreignNss, {}});
        return expCtx;
    }


    std::deque<DocumentSource::GetNextResult> makeForeignData(const ShardKeyPattern& pattern) {
        const std::initializer_list<const char*> data{
            "{_id: { x : { $minKey : 1 } }, max: { x : 0.0 }, shard: 'shard1' }",
            "{_id: { x : 0.0 }, max: { x : { $maxKey : 1 } }, shard: 'shard2' }"};
        std::deque<DocumentSource::GetNextResult> results;
        for (auto&& json : data) {
            results.emplace_back(Document(fromjson(json)));
        }
        return results;
    }

    std::deque<DocumentSource::GetNextResult> makeSourceData(const ShardKeyPattern& pattern) {
        const std::initializer_list<const char*> data{
            "{_id: 1, x: {$minKey: 1}}",
            "{_id: 2, x: -0.001}",
            "{_id: 3, x: NumberLong(0)}",
            "{_id: 4, x: 0.0}",
            "{_id: 5, x: 0.001}",
            "{_id: 6, x: {$maxKey: 1}}",
        };
        std::deque<DocumentSource::GetNextResult> results;
        for (auto&& json : data) {
            results.emplace_back(Document(fromjson(json)));
        }
        return results;
    }

private:
    NamespaceString _sourceNss;
};

TEST_F(ReshardingCollectionCloneTest, CollectionClonePipelineBasic) {
    NamespaceString fromNs("test", "system.resharding.coll");
    ShardKeyPattern pattern(BSON("x" << 1));

    // Mock non-lookup collection document source.

    {
        auto expCtx = createExpressionContext(fromNs);

        auto foreignData = makeForeignData(pattern);
        expCtx->mongoProcessInterface =
            std::make_shared<MockMongoInterface>(std::move(foreignData));

        auto sourceData = makeSourceData(pattern);
        auto mockSource = DocumentSourceMock::createForTest(std::move(sourceData), expCtx);

        auto pipeline = createAggForCollectionCloning(expCtx, pattern, fromNs, ShardId("shard1"));
        pipeline->addInitialSource(mockSource);

        auto next = pipeline->getNext();
        BSONObj val = fromjson("{_id: 1, x: {$minKey: 1}}");
        ASSERT_BSONOBJ_BINARY_EQ(val, next->toBson());

        next = pipeline->getNext();
        ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 2 << "x" << -0.001), next->toBson());

        ASSERT(!pipeline->getNext());
    }
    {
        auto expCtx = createExpressionContext(fromNs);

        auto foreignData = makeForeignData(pattern);
        expCtx->mongoProcessInterface =
            std::make_shared<MockMongoInterface>(std::move(foreignData));

        auto sourceData = makeSourceData(pattern);
        auto mockSource = DocumentSourceMock::createForTest(std::move(sourceData), expCtx);

        auto pipeline = createAggForCollectionCloning(expCtx, pattern, fromNs, ShardId("shard2"));
        pipeline->addInitialSource(mockSource);

        auto next = pipeline->getNext();
        BSONObj val = fromjson("{_id: 3, x: NumberLong(0)}");
        ASSERT_BSONOBJ_BINARY_EQ(val, next->toBson());

        next = pipeline->getNext();
        ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 4 << "x" << 0.0), next->toBson());

        next = pipeline->getNext();
        ASSERT_BSONOBJ_BINARY_EQ(BSON("_id" << 5 << "x" << 0.001), next->toBson());

        next = pipeline->getNext();
        val = fromjson("{_id: 6, x: {$maxKey: 1}}");
        ASSERT_BSONOBJ_BINARY_EQ(val, next->toBson());

        ASSERT(!pipeline->getNext());
    }
}

}  // namespace
}  // namespace mongo
