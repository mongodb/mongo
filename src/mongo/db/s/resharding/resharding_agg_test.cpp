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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding_util.h"
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

bool validateOplogId(const Timestamp& clusterTime,
                     const mongo::Document& sourceDoc,
                     const repl::OplogEntry& oplogEntry) {
    auto oplogIdExpected = ReshardingDonorOplogId{clusterTime, sourceDoc["ts"].getTimestamp()};
    auto oplogId = ReshardingDonorOplogId::parse(IDLParserErrorContext("ReshardingAggTest"),
                                                 oplogEntry.get_id()->getDocument().toBson());
    return oplogIdExpected == oplogId;
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

    auto makePipelineForReshardingDonorOplogIterator(
        std::deque<DocumentSource::GetNextResult> mockResults,
        ReshardingDonorOplogId resumeToken = {Timestamp::min(), Timestamp::min()}) {
        ReshardingDonorOplogIterator iterator(
            localOplogBufferNss(), std::move(resumeToken), nullptr /* insertNotifier */);

        // Mock lookup collection document source.
        auto pipeline =
            iterator.makePipeline(getOpCtx(), std::make_shared<MockMongoInterface>(mockResults));

        // Mock non-lookup collection document source.
        auto mockSource =
            DocumentSourceMock::createForTest(std::move(mockResults), pipeline->getContext());
        pipeline->addInitialSource(mockSource);

        return pipeline;
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

    BSONObj addExpectedFields(const repl::MutableOplogEntry& op,
                              const boost::optional<repl::MutableOplogEntry>& preImageOp,
                              const boost::optional<repl::MutableOplogEntry>& postImageOp) {
        BSONObjBuilder builder;

        builder.append(ReshardingDonorOplogIterator::kActualOpFieldName, op.toBSON());

        if (preImageOp) {
            builder.append(ReshardingDonorOplogIterator::kPreImageOpFieldName,
                           preImageOp->toBSON());
        }

        if (postImageOp) {
            builder.append(ReshardingDonorOplogIterator::kPostImageOpFieldName,
                           postImageOp->toBSON());
        }

        return builder.obj();
    }

    std::unique_ptr<Pipeline, PipelineDeleter> createPipeline(
        std::deque<DocumentSource::GetNextResult> pipelineSource) {
        // Set up the oplog collection state for $lookup and $graphLookup calls.
        auto expCtx = createExpressionContext();
        expCtx->ns = _remoteOplogNss;
        expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(pipelineSource);

        const bool doesDonorOwnMinKeyChunk = false;
        auto pipeline = createOplogFetchingPipelineForResharding(
            expCtx,
            ReshardingDonorOplogId(Timestamp::min(), Timestamp::min()),
            _reshardingCollUUID,
            {_destinedRecipient},
            doesDonorOwnMinKeyChunk);

        pipeline->addInitialSource(DocumentSourceMock::createForTest(pipelineSource, expCtx));

        return pipeline;
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

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(insertOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(deleteOplog, boost::none, boost::none),
                             next->toBson());

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

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults),
                                                                getOplogId(insertOplog));

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(deleteOplog, boost::none, boost::none),
                             next->toBson());

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

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults),
                                                                getOplogId(insertOplog));

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(deleteOplog, boost::none, boost::none),
                             next->toBson());

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

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(postImageOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(insertOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateWithPostOplog, boost::none, postImageOplog),
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

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));

    // Check only _id because attempting to call toBson will trigger BSON too large assertion.
    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(
        postImageOplog.get_id()->getDocument().toBson(),
        next->getNestedField(ReshardingDonorOplogIterator::kActualOpFieldName + "._id")
            .getDocument()
            .toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(
        insertOplog.get_id()->getDocument().toBson(),
        next->getNestedField(ReshardingDonorOplogIterator::kActualOpFieldName + "._id")
            .getDocument()
            .toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(
        updateWithPostOplog.get_id()->getDocument().toBson(),
        next->getNestedField(ReshardingDonorOplogIterator::kActualOpFieldName + "._id")
            .getDocument()
            .toBson());

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

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults),
                                                                getOplogId(postImageOplog));

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(insertOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateWithPostOplog, boost::none, postImageOplog),
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

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(preImageOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(insertOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(deleteWithPreOplog, preImageOplog, boost::none),
                             next->toBson());

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

    auto pipeline = makePipelineForReshardingDonorOplogIterator(std::move(mockResults));

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(preImageOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(postImageOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(insertOplog, boost::none, boost::none),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(updateWithPostOplog, boost::none, postImageOplog),
                             next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(addExpectedFields(deleteWithPreOplog, preImageOplog, boost::none),
                             next->toBson());

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

TEST_F(ReshardingAggTest, VerifyPipelineAtomicApplyOps) {
    const auto oplogBSON = fromjson(R"({
        "op": "c",
        "ns": "test.$cmd",
        "o": {
          "applyOps": [ {
              "op": "i",
              "ns": "test.foo",
              "o": { "_id": 0, "x": 2, "y": 2 },
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" }
            },
            {
              "op": "i",
              "ns": "test.foo",
              "o": { "_id": 1, "x": 3, "y": 5 },
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" }
            }
          ],
          "lsid": {
            "id": { "$binary": "36TPHJY3RJ6fYYBI1a5Eww==", "$type": "04" }
          },
          "$clusterTime": {
            "clusterTime": { "$timestamp": { "t": 1607639616, "i": 2 } },
            "signature": {
              "hash": { "$binary": "AAAAAAAAAAAAAAAAAAAAAAAAAAA=", "$type": "00" },
              "keyId": { "$numberLong": "0" }
            }
          },
          "$db": "test"
        },
        "ts": { "$timestamp": { "t": 1607639616, "i": 3 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2020-12-10T17:33:36.701-05:00" },
        "v": { "$numberLong": "2" }
    })");

    auto pipeline = createPipeline({Document(oplogBSON)});

    // This test currently fails since the createOplogFetchingPipelineForResharding() function is
    // looking for prevOpTime and destinedRecipient in each operation being applied.
    auto doc = pipeline->getNext();
    // TODO(SERVER-53542): Uncomment the following code once this ticket is addressed.
    // ASSERT(doc);

    // auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));

    // ASSERT(oplogEntry.isCommand());
    // ASSERT(repl::OplogEntry::CommandType::kApplyOps == oplogEntry.getCommandType());
    // ASSERT_EQ(oplogBSON["ts"].timestamp(), oplogEntry.getTimestamp());
    // ASSERT(validateOplogId(oplogBSON["ts"].timestamp(), Document(oplogBSON), oplogEntry));

    // doc = pipeline->getNext();
    ASSERT(!doc);
}

TEST_F(ReshardingAggTest, VerifyPipelineSmallTxn) {
    const auto oplogBSON = fromjson(R"({
        "lsid": {
          "id": { "$binary": "6Y5qL3pbTaGppzplSSvd8Q==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o" : {
          "applyOps": [ {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 2, "x": -2, "y": 4 },
              "destinedRecipient": "shard0"
            },
            {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 3, "x": -3, "y": 11 },
              "destinedRecipient": "shard1"
            }
          ]
        },
        "ts": { "$timestamp": { "t": 1609800490, "i": 8 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T17:48:10.907-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
            "ts": { "$timestamp": { "t": 0, "i": 0 } },
            "t": { "$numberLong": "-1" }
        }
    })");

    auto pipeline = createPipeline({Document(oplogBSON)});

    auto doc = pipeline->getNext();
    ASSERT(doc);

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));

    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kApplyOps == oplogEntry.getCommandType());
    ASSERT_FALSE(oplogEntry.shouldPrepare());
    ASSERT_EQ(1, oplogEntry.getObject()["applyOps"].Obj().nFields());
    ASSERT_EQ(oplogBSON["ts"].timestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(oplogBSON["ts"].timestamp(), Document(oplogBSON), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(!doc);
}

TEST_F(ReshardingAggTest, VerifyPipelineSmallPreparedTxn) {
    std::deque<DocumentSource::GetNextResult> pipelineSource = {Document(fromjson(R"({
            "lsid": {
              "id": { "$binary": "yakDu+s3S/qzds90F/CNsA==", "$type": "04" },
              "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
            },
            "txnNumber": { "$numberLong": "0" },
            "op": "c",
            "ns": "admin.$cmd",
            "o": {
              "applyOps": [ {
                  "op": "i",
                  "ns": "test.foo",
                  "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
                  "o": { "_id": 10, "x": -4, "y": 4 },
                  "destinedRecipient": "shard1"
                }
              ],
              "prepare": true
            },
            "ts": { "$timestamp": { "t": 1609800491, "i": 6 } },
            "t": { "$numberLong": "1" },
            "wall": { "$date": "2021-01-04T17:48:11.977-05:00" },
            "v": { "$numberLong": "2" },
            "prevOpTime": {
              "ts": { "$timestamp": { "t": 0, "i": 0 } },
              "t": { "$numberLong": "-1" }
            }
        })")),
                                                                Document(fromjson(R"({
            "lsid": {
              "id": { "$binary": "yakDu+s3S/qzds90F/CNsA==", "$type": "04" },
              "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
            },
            "txnNumber": { "$numberLong": "0" },
            "op": "c",
            "ns": "admin.$cmd",
            "o": {
              "commitTransaction": 1,
              "commitTimestamp": {
                "$timestamp": { "t": 1609800491, "i": 6 }
              }
            },
            "ts": { "$timestamp": { "t": 1609800492, "i": 2 } },
            "t": { "$numberLong": "1" },
            "wall": { "$date": "2021-01-04T17:48:12.077-05:00" },
            "v": { "$numberLong": "2" },
            "prevOpTime": {
              "ts": { "$timestamp": { "t": 1609800491, "i": 6 } },
              "t": { "$numberLong": "1" }
            }
        })"))};

    auto clusterTime = pipelineSource.back().getDocument()["ts"].getTimestamp();
    auto pipeline = createPipeline(pipelineSource);

    auto doc = pipeline->getNext();
    ASSERT(doc);

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));

    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kApplyOps == oplogEntry.getCommandType());
    ASSERT(oplogEntry.shouldPrepare());
    ASSERT_FALSE(oplogEntry.isPartialTransaction());
    ASSERT_EQ(1, oplogEntry.getObject()["applyOps"].Obj().nFields());
    ASSERT_EQ(pipelineSource[0].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[0].getDocument(), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(doc);

    oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));
    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kCommitTransaction == oplogEntry.getCommandType());
    ASSERT_EQ(pipelineSource[1].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[1].getDocument(), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(!doc);
}

// This test verifies that we don't return oplog entries that are not destined for the specified
// recipient shard. The test has an oplog that only has entries that stay on the source shard
// causing the pipeline to exclude the entire transaction.
// https://github.com/mongodb/mongo/blob/0615cd112f6cbe12ad6aab52319903a954158da5/src/mongo/db/s/resharding_util.cpp#L376-L379
TEST_F(ReshardingAggTest, VerifyPipelinePreparedTxnNoReshardedDocs) {
    std::deque<DocumentSource::GetNextResult> pipelineSource = {Document(fromjson(R"({
            "lsid": {
              "id": { "$binary": "yakDu+s3S/qzds90F/CNsA==", "$type": "04" },
              "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
            },
            "txnNumber": { "$numberLong": "0" },
            "op": "c",
            "ns": "admin.$cmd",
            "o": {
              "applyOps": [ {
                  "op": "i",
                  "ns": "test.foo",
                  "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
                  "o": { "_id": 10, "x": -4, "y": 4 },
                  "destinedRecipient": "shard0"
                }
              ],
              "prepare": true
            },
            "ts": { "$timestamp": { "t": 1609800491, "i": 6 } },
            "t": { "$numberLong": "1" },
            "wall": { "$date": "2021-01-04T17:48:11.977-05:00" },
            "v": { "$numberLong": "2" },
            "prevOpTime": {
              "ts": { "$timestamp": { "t": 0, "i": 0 } },
              "t": { "$numberLong": "-1" }
            }
        })")),
                                                                Document(fromjson(R"({
            "lsid": {
              "id": { "$binary": "yakDu+s3S/qzds90F/CNsA==", "$type": "04" },
              "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
            },
            "txnNumber": { "$numberLong": "0" },
            "op": "c",
            "ns": "admin.$cmd",
            "o": {
              "commitTransaction": 1,
              "commitTimestamp": {
                "$timestamp": { "t": 1609800491, "i": 6 }
              }
            },
            "ts": { "$timestamp": { "t": 1609800492, "i": 2 } },
            "t": { "$numberLong": "1" },
            "wall": { "$date": "2021-01-04T17:48:12.077-05:00" },
            "v": { "$numberLong": "2" },
            "prevOpTime": {
              "ts": { "$timestamp": { "t": 1609800491, "i": 6 } },
              "t": { "$numberLong": "1" }
            }
        })"))};

    auto pipeline = createPipeline(pipelineSource);

    auto doc = pipeline->getNext();
    ASSERT(!doc);
}

TEST_F(ReshardingAggTest, VerifyPipelinePreparedTxnAbort) {
    std::deque<DocumentSource::GetNextResult> pipelineSource = {Document(fromjson(R"({
        "lsid" : {
            "id" : {"$binary" : "qvCUY+yQRaW6mfQtQx+kWw==", "$type" : "04"},
            "uid" : {"$binary" : "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type" : "00"}
        },
        "txnNumber" : {"$numberLong" : "0"},
        "op" : "c",
        "ns" : "admin.$cmd",
        "o" : {
            "applyOps" : [ {
                "op" : "i",
                "ns" : "test.foo",
                "ui" : {"$binary" : "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type" : "04"},
                "o" : {"_id" : 12, "x" : -4, "y" : 10},
                "destinedRecipient" : "shard1"
            } ],
            "prepare" : true
        },
        "ts" : {"$timestamp" : {"t" : 1609800492, "i" : 4}},
        "t" : {"$numberLong" : "1"},
        "wall" : {"$date" : "2021-01-04T17:48:12.470-05:00"},
        "v" : {"$numberLong" : "2"},
        "prevOpTime" : {"ts" : {"$timestamp" : {"t" : 0, "i" : 0}}, "t" : {"$numberLong" : "-1"}}
    })")),
                                                                Document(fromjson(R"({
        "lsid" : {
            "id" : {"$binary" : "qvCUY+yQRaW6mfQtQx+kWw==", "$type" : "04"},
            "uid" : {"$binary" : "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type" : "00"}
        },
        "txnNumber" : {"$numberLong" : "0"},
        "op" : "c",
        "ns" : "admin.$cmd",
        "o" : {"abortTransaction" : 1},
        "ts" : {"$timestamp" : {"t" : 1609800492, "i" : 5}},
        "t" : {"$numberLong" : "1"},
        "wall" : {"$date" : "2021-01-04T17:48:12.640-05:00"},
        "v" : {"$numberLong" : "2"},
        "prevOpTime" :
            {"ts" : {"$timestamp" : {"t" : 1609800492, "i" : 4}}, "t" : {"$numberLong" : "1"}}
    })"))};

    auto clusterTime = pipelineSource.back().getDocument()["ts"].getTimestamp();
    auto pipeline = createPipeline(pipelineSource);

    auto doc = pipeline->getNext();
    ASSERT(doc);

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));

    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kAbortTransaction == oplogEntry.getCommandType());
    ASSERT_FALSE(oplogEntry.shouldPrepare());
    ASSERT_FALSE(oplogEntry.isPartialTransaction());
    ASSERT_EQ(pipelineSource[1].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[1].getDocument(), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(!doc);
}

TEST_F(ReshardingAggTest, VerifyPipelineLargePreparedTxn) {
    std::deque<DocumentSource::GetNextResult> pipelineSource = {Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "rSg0RzXCTkmM+WGwkZz2GQ==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": {
          "applyOps": [ {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 14, "x": -4, "y": 11 },
              "destinedRecipient": "shard1"
            },
            {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 16, "x": -3, "y": 12 },
              "destinedRecipient": "shard1"
            }
          ],
          "partialTxn": true
        },
        "ts": { "$timestamp": { "t": 1609818496, "i": 2 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T22:48:16.364-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 0, "i": 0 } },
          "t": { "$numberLong": "-1" }
        }
    })")),
                                                                Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "rSg0RzXCTkmM+WGwkZz2GQ==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": {
          "applyOps": [ {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 18, "x": 2, "y": 3 },
              "destinedRecipient": "shard0"
            }
          ],
          "prepare": true,
          "count": { "$numberLong": "3" }
        },
        "ts": { "$timestamp": { "t": 1609818496, "i": 4 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T22:48:16.365-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 1609818496, "i": 2 } },
          "t": { "$numberLong": "1" }
        }
    })")),
                                                                Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "rSg0RzXCTkmM+WGwkZz2GQ==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": {
          "commitTransaction": 1,
          "commitTimestamp": {
            "$timestamp": { "t": 1609818496, "i": 4 } } },
        "ts": { "$timestamp": { "t": 1609818496, "i": 6 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T22:48:16.475-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 1609818496, "i": 4 } },
          "t": { "$numberLong": "1" }
        }
    })"))};

    auto clusterTime = pipelineSource.back().getDocument()["ts"].getTimestamp();
    auto pipeline = createPipeline(pipelineSource);

    auto doc = pipeline->getNext();
    ASSERT(doc);

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));

    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kApplyOps == oplogEntry.getCommandType());
    ASSERT_FALSE(oplogEntry.shouldPrepare());
    ASSERT(oplogEntry.isPartialTransaction());
    ASSERT_EQ(2, oplogEntry.getObject()["applyOps"].Obj().nFields());
    ASSERT_EQ(pipelineSource[0].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[0].getDocument(), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(doc);

    oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));
    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kApplyOps == oplogEntry.getCommandType());
    ASSERT(oplogEntry.shouldPrepare());
    ASSERT_FALSE(oplogEntry.isPartialTransaction());
    ASSERT_EQ(0, oplogEntry.getObject()["applyOps"].Obj().nFields());
    ASSERT_EQ(pipelineSource[1].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[1].getDocument(), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(doc);

    oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));
    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kCommitTransaction == oplogEntry.getCommandType());
    ASSERT_FALSE(oplogEntry.shouldPrepare());
    ASSERT_FALSE(oplogEntry.isPartialTransaction());
    ASSERT_EQ(pipelineSource[2].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[2].getDocument(), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(!doc);
}

TEST_F(ReshardingAggTest, VerifyPipelineLargePreparedTxnAbort) {
    std::deque<DocumentSource::GetNextResult> pipelineSource = {Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "9blprrsdR0+oa82vX5vmWQ==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": {
          "applyOps": [ {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 19, "x": -4, "y": 4 },
              "destinedRecipient": "shard0"
            },
            {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 21, "x": -3, "y": 3 },
              "destinedRecipient": "shard0"
            }
          ],
          "partialTxn": true
        },
        "ts": { "$timestamp": { "t": 1609800493, "i": 8 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T17:48:13.937-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 0, "i": 0 } },
          "t": { "$numberLong": "-1" }
        }
    })")),
                                                                Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "9blprrsdR0+oa82vX5vmWQ==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": {
          "applyOps": [ {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 22, "x": -2, "y": 12 },
              "destinedRecipient": "shard1"
            }
          ],
          "prepare": true,
          "count": { "$numberLong": "3" }
        },
        "ts": { "$timestamp": { "t": 1609800493, "i": 10 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T17:48:13.937-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 1609800493, "i": 8 } },
          "t": { "$numberLong": "1" }
        }
    })")),
                                                                Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "9blprrsdR0+oa82vX5vmWQ==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": { "abortTransaction": 1 },
        "ts": { "$timestamp": { "t": 1609800494, "i": 1 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T17:48:14.081-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 1609800493, "i": 10 } },
          "t": { "$numberLong": "1" }
        }
    })"))};

    auto clusterTime = pipelineSource.back().getDocument()["ts"].getTimestamp();
    auto pipeline = createPipeline(pipelineSource);

    auto doc = pipeline->getNext();
    ASSERT(doc);

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));

    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kAbortTransaction == oplogEntry.getCommandType());
    ASSERT_FALSE(oplogEntry.shouldPrepare());
    ASSERT_FALSE(oplogEntry.isPartialTransaction());
    ASSERT_EQ(pipelineSource[2].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[2].getDocument(), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(!doc);
}

TEST_F(ReshardingAggTest, VerifyPipelineLargeTxn) {
    std::deque<DocumentSource::GetNextResult> pipelineSource = {Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "+0TxuFyBSeqjfJzju2Xl+w==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": {
          "applyOps": [ {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 4, "x": -20, "y": 4 },
              "destinedRecipient": "shard0"
            },
            {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 5, "x": -30, "y": 11 },
              "destinedRecipient": "shard1"
            }
          ],
          "partialTxn": true
        },
        "ts": { "$timestamp": { "t": 1609800491, "i": 1 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T17:48:11.237-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 0, "i": 0 } },
          "t": { "$numberLong": "-1" }
        }
    })")),
                                                                Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "+0TxuFyBSeqjfJzju2Xl+w==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": {
          "applyOps": [ {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 6, "x": -40, "y": 11 },
              "destinedRecipient": "shard1"
            }
          ],
          "count": { "$numberLong": "3" }
        },
        "ts": { "$timestamp": { "t": 1609800491, "i": 2 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T17:48:11.240-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 1609800491, "i": 1 } },
          "t": { "$numberLong": "1" }
        }
    })"))};

    auto clusterTime = pipelineSource.back().getDocument()["ts"].getTimestamp();
    auto pipeline = createPipeline(pipelineSource);

    auto doc = pipeline->getNext();
    ASSERT(doc);

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));

    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kApplyOps == oplogEntry.getCommandType());
    ASSERT_FALSE(oplogEntry.shouldPrepare());
    ASSERT(oplogEntry.isPartialTransaction());
    ASSERT_EQ(1, oplogEntry.getObject()["applyOps"].Obj().nFields());
    ASSERT_EQ(pipelineSource[0].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[0].getDocument(), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(doc);

    oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));
    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kApplyOps == oplogEntry.getCommandType());
    ASSERT_FALSE(oplogEntry.shouldPrepare());
    ASSERT_FALSE(oplogEntry.isPartialTransaction());
    ASSERT_EQ(1, oplogEntry.getObject()["applyOps"].Obj().nFields());
    ASSERT_EQ(pipelineSource[1].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[1].getDocument(), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(!doc);
}

// This case can only happen if a primary locally commits the transaction, but fails before the
// commit txn is replicated. A new node will step up, and then it will see the in-progress entry +
// txn state, and then abort it. This means the new primary will have a a partialTxn applyOps entry
// + and abortTransaction oplog entry.
TEST_F(ReshardingAggTest, VerifyPipelineLargeTxnAbort) {
    std::deque<DocumentSource::GetNextResult> pipelineSource = {Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "+0TxuFyBSeqjfJzju2Xl+w==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": {
          "applyOps": [ {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 4, "x": -20, "y": 4 },
              "destinedRecipient": "shard0"
            },
            {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 5, "x": -30, "y": 11 },
              "destinedRecipient": "shard1"
            }
          ],
          "partialTxn": true
        },
        "ts": { "$timestamp": { "t": 1609800491, "i": 1 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T17:48:11.237-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 0, "i": 0 } },
          "t": { "$numberLong": "-1" }
        }
    })")),
                                                                Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "+0TxuFyBSeqjfJzju2Xl+w==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": { "abortTransaction": 1 },
        "ts": { "$timestamp": { "t": 1609800491, "i": 2 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T17:48:11.240-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 1609800491, "i": 1 } },
          "t": { "$numberLong": "1" }
        }
    })"))};

    auto clusterTime = pipelineSource.back().getDocument()["ts"].getTimestamp();
    auto pipeline = createPipeline(pipelineSource);

    auto doc = pipeline->getNext();
    ASSERT(doc);

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));

    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kAbortTransaction == oplogEntry.getCommandType());
    ASSERT_FALSE(oplogEntry.shouldPrepare());
    ASSERT_FALSE(oplogEntry.isPartialTransaction());
    ASSERT_EQ(pipelineSource[1].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[1].getDocument(), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(!doc);
}

TEST_F(ReshardingAggTest, VerifyPipelineLargeTxnIncomplete) {
    std::deque<DocumentSource::GetNextResult> pipelineSource = {Document(fromjson(R"({
        "lsid": {
          "id": { "$binary": "+0TxuFyBSeqjfJzju2Xl+w==", "$type": "04" },
          "uid": { "$binary": "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=", "$type": "00" }
        },
        "txnNumber": { "$numberLong": "0" },
        "op": "c",
        "ns": "admin.$cmd",
        "o": {
          "applyOps": [ {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 4, "x": -20, "y": 4 },
              "destinedRecipient": "shard0"
            },
            {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 5, "x": -30, "y": 11 },
              "destinedRecipient": "shard1"
            }
          ],
          "partialTxn": true
        },
        "ts": { "$timestamp": { "t": 1609800491, "i": 1 } },
        "t": { "$numberLong": "1" },
        "wall": { "$date": "2021-01-04T17:48:11.237-05:00" },
        "v": { "$numberLong": "2" },
        "prevOpTime": {
          "ts": { "$timestamp": { "t": 0, "i": 0 } },
          "t": { "$numberLong": "-1" }
        }
    })"))};

    auto pipeline = createPipeline(pipelineSource);

    auto doc = pipeline->getNext();
    ASSERT(!doc);
}

}  // namespace
}  // namespace mongo