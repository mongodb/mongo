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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

using namespace fmt::literals;

const NamespaceString kRemoteOplogNss{"local.oplog.rs"};
const NamespaceString kLocalOplogBufferNss{"{}.{}xxx.yyy"_format(
    NamespaceString::kConfigDb, NamespaceString::kReshardingLocalOplogBufferPrefix)};

// A mock TransactionHistoryIterator to support DSReshardingIterateTransaction.
class MockTransactionHistoryIterator : public TransactionHistoryIteratorBase {
public:
    MockTransactionHistoryIterator(std::deque<DocumentSource::GetNextResult> oplogContents,
                                   repl::OpTime startTime)
        : _oplogContents(std::move(oplogContents)),
          _oplogIt(_oplogContents.rbegin()),
          _nextOpTime(std::move(startTime)) {}

    virtual ~MockTransactionHistoryIterator() = default;

    bool hasNext() const {
        return !_nextOpTime.isNull();
    }

    repl::OplogEntry next(OperationContext* opCtx) {
        BSONObj oplogBSON = findOneOplogEntry(_nextOpTime);

        auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogBSON));
        const auto& oplogPrevTsOption = oplogEntry.getPrevWriteOpTimeInTransaction();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Missing prevOpTime field: " << oplogBSON,
                oplogPrevTsOption);

        _nextOpTime = oplogPrevTsOption.value();

        return oplogEntry;
    }

    repl::OpTime nextOpTime(OperationContext* opCtx) {
        BSONObj oplogBSON = findOneOplogEntry(_nextOpTime);

        auto prevOpTime = oplogBSON[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName];
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Missing prevOpTime field: " << oplogBSON,
                !prevOpTime.eoo() && prevOpTime.isABSONObj());

        auto returnOpTime = _nextOpTime;
        _nextOpTime = repl::OpTime::parse(prevOpTime.Obj());
        return returnOpTime;
    }

private:
    BSONObj findOneOplogEntry(repl::OpTime needle) {
        for (; _oplogIt != _oplogContents.rend(); _oplogIt++) {
            auto oplogBSON = _oplogIt->getDocument().toBson();
            auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogBSON));
            if (oplogEntry.getOpTime() == needle) {
                return oplogBSON;
            }
        }
        // We should never reach here unless the txn chain has fallen off the oplog.
        uasserted(ErrorCodes::IncompleteTransactionHistory,
                  str::stream() << "oplog with opTime " << needle.toBSON() << " cannot be found");
    }

    std::deque<DocumentSource::GetNextResult> _oplogContents;
    std::deque<DocumentSource::GetNextResult>::reverse_iterator _oplogIt;
    repl::OpTime _nextOpTime;
};

/**
 * Mock interface to allow specifiying mock results for the lookup pipeline.
 */
class MockMongoInterface final : public StubMongoProcessInterface {
public:
    MockMongoInterface(std::deque<DocumentSource::GetNextResult> mockResults)
        : _mockResults(std::move(mockResults)) {}

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        Pipeline* ownedPipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final {
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline(
            ownedPipeline, PipelineDeleter(ownedPipeline->getContext()->opCtx));

        pipeline->addInitialSource(
            DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
        return pipeline;
    }

    std::unique_ptr<TransactionHistoryIteratorBase> createTransactionHistoryIterator(
        repl::OpTime time) const {
        return std::unique_ptr<TransactionHistoryIteratorBase>(
            new MockTransactionHistoryIterator(_mockResults, time));
    }

    BSONObj getCollectionOptions(OperationContext* opCtx, const NamespaceString& nss) override {
        auto optionIter = _collectionOptions.find(nss);
        invariant(optionIter != _collectionOptions.end(),
                  str::stream() << nss.ns() << " was not registered");

        return optionIter->second;
    }

    boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID collectionUUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) {
        DBDirectClient client(expCtx->opCtx);
        auto result = client.findOne(nss, documentKey.toBson());
        if (result.isEmpty()) {
            return boost::none;
        }

        return Document(result.getOwned());
    }

    void setCollectionOptions(const NamespaceString& nss, const BSONObj option) {
        _collectionOptions[nss] = option;
    }

private:
    std::deque<DocumentSource::GetNextResult> _mockResults;
    std::map<NamespaceString, BSONObj> _collectionOptions;
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

repl::DurableOplogEntry makeApplyOpsOplog(std::vector<BSONObj> operations,
                                          repl::OpTime opTime,
                                          repl::OpTime prevOpTime,
                                          OperationSessionInfo sessionInfo,
                                          bool isPrepare,
                                          bool isPartial) {
    BSONObjBuilder applyOpsBuilder;
    BSONArrayBuilder opsArrayBuilder = applyOpsBuilder.subarrayStart("applyOps");
    for (const auto& operation : operations) {
        opsArrayBuilder.append(operation);
    }
    opsArrayBuilder.done();

    if (isPrepare) {
        applyOpsBuilder.append(repl::ApplyOpsCommandInfoBase::kPrepareFieldName, true);
    }
    if (isPartial) {
        applyOpsBuilder.append(repl::ApplyOpsCommandInfoBase::kPartialTxnFieldName, true);
    }

    return {opTime,
            repl::OpTypeEnum::kCommand,
            {},
            UUID::gen(),
            false /* fromMigrate */,
            0 /* version */,
            applyOpsBuilder.obj(), /* o */
            boost::none,           /* o2 */
            sessionInfo,
            boost::none /* upsert */,
            {} /* date */,
            {}, /* statementIds */
            prevOpTime /* prevWriteOpTime */,
            boost::none /* preImage */,
            boost::none /* postImage */,
            boost::none /* destinedRecipient */,
            boost::none /* idField */,
            boost::none /* needsRetryImage */};
}

bool validateOplogId(const Timestamp& clusterTime,
                     const mongo::Document& sourceDoc,
                     const repl::OplogEntry& oplogEntry) {
    auto oplogIdExpected = ReshardingDonorOplogId{clusterTime, sourceDoc["ts"].getTimestamp()};
    auto oplogId = ReshardingDonorOplogId::parse(IDLParserContext("ReshardingAggTest"),
                                                 oplogEntry.get_id()->getDocument().toBson());
    return oplogIdExpected == oplogId;
}

boost::intrusive_ptr<ExpressionContextForTest> createExpressionContext(OperationContext* opCtx) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(
        new ExpressionContextForTest(opCtx, kLocalOplogBufferNss));
    expCtx->setResolvedNamespace(kLocalOplogBufferNss, {kLocalOplogBufferNss, {}});
    expCtx->setResolvedNamespace(kRemoteOplogNss, {kRemoteOplogNss, {}});
    return expCtx;
}

class ReshardingAggTest : public AggregationContextFixture {
protected:
    const NamespaceString& localOplogBufferNss() {
        return kLocalOplogBufferNss;
    }

    boost::intrusive_ptr<ExpressionContextForTest> createExpressionContext() {
        return ::mongo::createExpressionContext(getOpCtx());
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


    ReshardingDonorOplogId getOplogId(const repl::MutableOplogEntry& oplog) {
        return ReshardingDonorOplogId::parse(IDLParserContext("ReshardingAggTest::getOplogId"),
                                             oplog.get_id()->getDocument().toBson());
    }

    std::unique_ptr<Pipeline, PipelineDeleter> createPipeline(
        std::deque<DocumentSource::GetNextResult> pipelineSource) {
        // Set up the oplog collection state for $lookup and $graphLookup calls.
        auto expCtx = createExpressionContext();
        expCtx->ns = kRemoteOplogNss;
        expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(pipelineSource);

        auto pipeline = resharding::createOplogFetchingPipelineForResharding(
            expCtx,
            ReshardingDonorOplogId(Timestamp::min(), Timestamp::min()),
            _reshardingCollUUID,
            {_destinedRecipient});

        pipeline->addInitialSource(DocumentSourceMock::createForTest(pipelineSource, expCtx));

        return pipeline;
    }

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
    ASSERT_BSONOBJ_BINARY_EQ(insertOplog.toBSON(), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(updateOplog.toBSON(), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(deleteOplog.toBSON(), next->toBson());

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
    ASSERT_BSONOBJ_BINARY_EQ((updateOplog.toBSON()), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(deleteOplog.toBSON(), next->toBson());

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
    ASSERT_BSONOBJ_BINARY_EQ(updateOplog.toBSON(), next->toBson());

    next = pipeline->getNext();
    ASSERT_BSONOBJ_BINARY_EQ(deleteOplog.toBSON(), next->toBson());

    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingAggTest, VerifyPipelineReturnsStartIndexBuildEntry) {
    const auto oplogBSON = fromjson(R"({
        "op" : "c",
        "ns" : "test.$cmd",
        "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
        "o" : {
          "startIndexBuild" : "weather",
          "indexBuildUUID" : { "binary": "bac65b70-e5c7-48f5-bc09-be78e69733a7", "$type": "04" },
          "indexes" : [ {
              "v" : 2,
              "key" : { "col" : 1 },
              "name" : "col_1"
            }
          ]
        },
        "ts" : { "$timestamp": { "t": 1612471173, "i": 2 } },
        "t" : { "$numberLong": "1" },
        "wall" : { "$date": "2021-02-04T20:39:33.860Z" },
        "v" : { "$numberLong": "2" }
    })");

    auto pipeline = createPipeline({Document(oplogBSON)});

    auto doc = pipeline->getNext();
    ASSERT(doc);

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(doc->toBson()));

    ASSERT(oplogEntry.isCommand());
    ASSERT(repl::OplogEntry::CommandType::kStartIndexBuild == oplogEntry.getCommandType());
    ASSERT_EQ(oplogBSON["ts"].timestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(oplogBSON["ts"].timestamp(), Document(oplogBSON), oplogEntry));

    doc = pipeline->getNext();
    ASSERT(!doc);
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
    expCtx->ns = kRemoteOplogNss;
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(pipelineSource);

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline =
        resharding::createOplogFetchingPipelineForResharding(
            expCtx,
            // Use the test to also exercise the stages for resuming. The timestamp passed in is
            // excluded from the results.
            ReshardingDonorOplogId(insertOplog.getTimestamp(), insertOplog.getTimestamp()),
            _reshardingCollUUID,
            {_destinedRecipient});
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
    expCtx->ns = kRemoteOplogNss;
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(pipelineSource);

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline =
        resharding::createOplogFetchingPipelineForResharding(
            expCtx,
            ReshardingDonorOplogId(Timestamp::min(), Timestamp::min()),
            _reshardingCollUUID,
            {_destinedRecipient});
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
    ASSERT(validateOplogId(clusterTime, Document::fromBsonWithMetaData(prepareEntry), oplogEntry));

    // We should not see the `commitTransaction` entry, since DSReshardingIterateTransaction
    // swallows it.
    ASSERT(!pipeline->getNext());
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

    // We don't need to support atomic applyOps in the resharding pipeline; we filter them out.
    ASSERT(!pipeline->getNext());
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

    // We should not observe the 'commitTransaction' entry, since DSReshardingIterateTransaction
    // swallows it.
    ASSERT(!pipeline->getNext());
}

// This test verifies that we don't return oplog entries that are not destined for the specified
// recipient shard. The test has an oplog that only has entries that stay on the source shard
// causing the pipeline to exclude the entire transaction.
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

    // We don't see any results since there are no events for the requested destinedRecipient in the
    // 'applyOps' and we swallow the 'commitTransaction' event internally.
    ASSERT(!pipeline->getNext());
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

    ASSERT(!pipeline->getNext());
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
              "ui": { "$binary": "rSg0RzXCTkmM+WGwkZz2GQ==", "$type": "04" },
              "o": { "_id": -18, "x": -2, "y": -3 },
              "destinedRecipient": "shard1"
            },
            {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": 18, "x": 2, "y": 3 },
              "destinedRecipient": "shard1"
            },
            {
              "op": "i",
              "ns": "test.foo",
              "ui": { "$binary": "iSa6jmEaQsK7Gjt4GfYQ7Q==", "$type": "04" },
              "o": { "_id": -18, "x": -2, "y": -3 },
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
    // We only get back 1 out of 3 entries in the second 'applyOps' because only one of them matches
    // both the correct UUID and the expected destinedRecipient.
    ASSERT_EQ(1, oplogEntry.getObject()["applyOps"].Obj().nFields());
    ASSERT_EQ(pipelineSource[1].getDocument()["ts"].getTimestamp(), oplogEntry.getTimestamp());
    ASSERT(validateOplogId(clusterTime, pipelineSource[1].getDocument(), oplogEntry));

    // We do not expect any further results because we swallow the 'commitTransaction' internally.
    ASSERT(!pipeline->getNext());
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

using ReshardingAggWithStorageTest = MockReplCoordServerFixture;

// Tests that find and modify oplog with image lookup gets converted to the old style oplog pairs
// with no-op pre/post image oplog.
TEST_F(ReshardingAggWithStorageTest, RetryableFindAndModifyWithImageLookup) {
    repl::OpTime opTime(Timestamp(43, 56), 1);
    const NamespaceString kCrudNs("foo", "bar");
    const UUID kCrudUUID = UUID::gen();
    const ShardId kMyShardId{"shard1"};
    ReshardingDonorOplogId id(opTime.getTimestamp(), opTime.getTimestamp());

    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNum(45);
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNum);

    const BSONObj preImage(BSON("_id" << 2 << "post" << 1));

    repl::ImageEntry imageEntry;
    imageEntry.set_id(lsid);
    imageEntry.setTxnNumber(txnNum);
    imageEntry.setTs(opTime.getTimestamp());
    imageEntry.setImageKind(repl::RetryImageEnum::kPreImage);
    imageEntry.setImage(preImage);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kConfigImagesNamespace.ns(), imageEntry.toBSON());

    repl::DurableOplogEntry oplog(opTime,
                                  repl::OpTypeEnum::kUpdate,
                                  kCrudNs,
                                  kCrudUUID,
                                  false /* fromMigrate */,
                                  0 /* version */,
                                  BSON("$set" << BSON("y" << 1)), /* o1 */
                                  BSON("_id" << 2),               /* o2 */
                                  sessionInfo,
                                  boost::none /* upsert */,
                                  {} /* date */,
                                  {1}, /* statementIds */
                                  boost::none /* prevWrite */,
                                  boost::none /* preImage */,
                                  boost::none /* postImage */,
                                  kMyShardId,
                                  Value(id.toBSON()),
                                  repl::RetryImageEnum::kPreImage);

    std::deque<DocumentSource::GetNextResult> pipelineSource{Document(oplog.toBSON())};
    auto expCtx = createExpressionContext(opCtx());
    expCtx->ns = NamespaceString::kRsOplogNamespace;

    {
        auto mockMongoInterface = std::make_shared<MockMongoInterface>(pipelineSource);
        // Register a dummy uuid just to not make test crash. The stub for findSingleDoc ignores
        // the UUID so it doesn't matter what the value here is.
        mockMongoInterface->setCollectionOptions(NamespaceString::kConfigImagesNamespace,
                                                 BSON("uuid" << UUID::gen()));
        expCtx->mongoProcessInterface = std::move(mockMongoInterface);
    }

    auto pipeline = resharding::createOplogFetchingPipelineForResharding(
        expCtx, ReshardingDonorOplogId(Timestamp::min(), Timestamp::min()), kCrudUUID, kMyShardId);

    pipeline->addInitialSource(DocumentSourceMock::createForTest(pipelineSource, expCtx));

    auto preImageOplogDoc = pipeline->getNext();
    ASSERT_TRUE(preImageOplogDoc);
    auto preImageOplogStatus = repl::DurableOplogEntry::parse(preImageOplogDoc->toBson());
    ASSERT_OK(preImageOplogStatus);

    auto preImageOplog = preImageOplogStatus.getValue();
    ASSERT_BSONOBJ_EQ(preImage, preImageOplog.getObject());
    ASSERT_EQ(OpType_serializer(repl::OpTypeEnum::kNoop),
              OpType_serializer(preImageOplog.getOpType()));

    auto updateOplogDoc = pipeline->getNext();
    ASSERT_TRUE(updateOplogDoc);
    auto updateOplogStatus = repl::DurableOplogEntry::parse(updateOplogDoc->toBson());

    auto updateOplog = updateOplogStatus.getValue();
    ASSERT_LT(preImageOplog.getOpTime(), updateOplog.getOpTime());
    ASSERT_TRUE(updateOplog.getPreImageOpTime());
    ASSERT_FALSE(updateOplog.getNeedsRetryImage());
    ASSERT_EQ(preImageOplog.getOpTime(), *updateOplog.getPreImageOpTime());
    ASSERT_EQ(OpType_serializer(repl::OpTypeEnum::kUpdate),
              OpType_serializer(updateOplog.getOpType()));
    ASSERT_BSONOBJ_EQ(oplog.getObject(), updateOplog.getObject());
    ASSERT_TRUE(updateOplog.getObject2());
    ASSERT_BSONOBJ_EQ(*oplog.getObject2(), *updateOplog.getObject2());
    ASSERT_EQ(oplog.getNss(), updateOplog.getNss());
    ASSERT_TRUE(updateOplog.getUuid());
    ASSERT_EQ(*oplog.getUuid(), *updateOplog.getUuid());
    ASSERT_BSONOBJ_EQ(oplog.getOperationSessionInfo().toBSON(),
                      updateOplog.getOperationSessionInfo().toBSON());

    ASSERT_FALSE(pipeline->getNext());
}

TEST_F(ReshardingAggWithStorageTest,
       RetryableFindAndModifyInsideInternalTransactionWithImageLookup) {
    const NamespaceString kCrudNs("foo", "bar");
    const UUID kCrudUUID = UUID::gen();
    const ShardId kMyShardId{"shard1"};

    const auto lsid = makeLogicalSessionIdWithTxnNumberAndUUIDForTest();
    const TxnNumber txnNum(45);
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNum);

    const repl::OpTime applyOpsOpTime1(Timestamp(1, 1), 1);
    const repl::OpTime applyOpsOpTime2(Timestamp(2, 2), 1);  // applyOps with 'needsRetryImage'.
    const repl::OpTime applyOpsOpTime3(Timestamp(3, 3), 1);

    auto inputInnerOp1 = repl::MutableOplogEntry::makeInsertOperation(
        kCrudNs, kCrudUUID, BSON("_id" << 1 << "a" << 1), BSON("_id" << 1));
    inputInnerOp1.setDestinedRecipient(kMyShardId);
    auto inputApplyOpsOplog1 = makeApplyOpsOplog(
        {inputInnerOp1.toBSON()}, applyOpsOpTime1, repl::OpTime(), sessionInfo, false, true);

    auto inputInnerOp2 = repl::MutableOplogEntry::makeUpdateOperation(
        kCrudNs, kCrudUUID, BSON("$set" << BSON("a" << 2)), BSON("_id" << 2));
    inputInnerOp2.setDestinedRecipient(kMyShardId);
    inputInnerOp2.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
    auto inputApplyOpsOplog2 = makeApplyOpsOplog(
        {inputInnerOp2.toBSON()}, applyOpsOpTime2, applyOpsOpTime1, sessionInfo, false, true);

    auto inputInnerOp3 = repl::MutableOplogEntry::makeInsertOperation(
        kCrudNs, kCrudUUID, BSON("_id" << 3 << "a" << 3), BSON("_id" << 3));
    inputInnerOp3.setDestinedRecipient(kMyShardId);
    auto inputApplyOpsOplog3 = makeApplyOpsOplog(
        {inputInnerOp3.toBSON()}, applyOpsOpTime3, applyOpsOpTime2, sessionInfo, false, false);

    const BSONObj preImage(BSON("_id" << 2));
    repl::ImageEntry imageEntry;
    imageEntry.set_id(lsid);
    imageEntry.setTxnNumber(txnNum);
    imageEntry.setTs(applyOpsOpTime2.getTimestamp());
    imageEntry.setImageKind(repl::RetryImageEnum::kPreImage);
    imageEntry.setImage(preImage);

    DBDirectClient client(opCtx());
    client.insert(NamespaceString::kConfigImagesNamespace.ns(), imageEntry.toBSON());

    auto createPipeline = [&](ReshardingDonorOplogId startAt) {
        std::deque<DocumentSource::GetNextResult> pipelineSource{
            Document{inputApplyOpsOplog1.toBSON()},
            Document(inputApplyOpsOplog2.toBSON()),
            Document{inputApplyOpsOplog3.toBSON()}};

        auto expCtx = createExpressionContext(opCtx());
        expCtx->ns = NamespaceString::kRsOplogNamespace;

        {
            auto mockMongoInterface = std::make_shared<MockMongoInterface>(pipelineSource);
            // Register a dummy uuid just to not make test crash. The stub for findSingleDoc ignores
            // the UUID so it doesn't matter what the value here is.
            mockMongoInterface->setCollectionOptions(NamespaceString::kConfigImagesNamespace,
                                                     BSON("uuid" << UUID::gen()));
            expCtx->mongoProcessInterface = std::move(mockMongoInterface);
        }

        auto pipeline = resharding::createOplogFetchingPipelineForResharding(
            expCtx, startAt, kCrudUUID, kMyShardId);
        pipeline->addInitialSource(DocumentSourceMock::createForTest(pipelineSource, expCtx));
        return pipeline;
    };

    // Create a pipeline and verify that it outputs the doc for the forged noop oplog entry
    // immediately before the downcoverted doc for the applyOps with the 'needsRetryImage' field.
    auto pipeline = createPipeline(ReshardingDonorOplogId(Timestamp::min(), Timestamp::min()));

    auto applyOpsOplogDoc1 = pipeline->getNext();
    ASSERT_TRUE(applyOpsOplogDoc1);
    auto swOutputApplyOpsOplog1 = repl::DurableOplogEntry::parse(applyOpsOplogDoc1->toBson());
    ASSERT_OK(swOutputApplyOpsOplog1);
    auto outputApplyOpsOplog1 = swOutputApplyOpsOplog1.getValue();
    ASSERT_BSONOBJ_EQ(inputApplyOpsOplog1.toBSON().removeField(repl::OplogEntry::kObjectFieldName),
                      outputApplyOpsOplog1.toBSON().removeFields(StringDataSet{
                          repl::OplogEntry::kObjectFieldName, repl::OplogEntry::k_idFieldName}));

    auto preImageOplogDoc = pipeline->getNext();
    ASSERT_TRUE(preImageOplogDoc);
    auto swPreImageOplog = repl::DurableOplogEntry::parse(preImageOplogDoc->toBson());
    ASSERT_OK(swPreImageOplog);
    auto preImageOplog = swPreImageOplog.getValue();
    ASSERT_BSONOBJ_EQ(preImage, preImageOplog.getObject());
    ASSERT_EQ(OpType_serializer(repl::OpTypeEnum::kNoop),
              OpType_serializer(preImageOplog.getOpType()));

    auto applyOpsOplogDoc2 = pipeline->getNext();
    ASSERT_TRUE(applyOpsOplogDoc2);
    auto swOutputApplyOpsOplog2 = repl::DurableOplogEntry::parse(applyOpsOplogDoc2->toBson());
    ASSERT_OK(swOutputApplyOpsOplog2);
    auto outputApplyOpsOplog2 = swOutputApplyOpsOplog2.getValue();
    ASSERT_BSONOBJ_EQ(inputApplyOpsOplog2.toBSON().removeField(repl::OplogEntry::kObjectFieldName),
                      outputApplyOpsOplog2.toBSON().removeFields(StringDataSet{
                          repl::OplogEntry::kObjectFieldName, repl::OplogEntry::k_idFieldName}));

    auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(outputApplyOpsOplog2.getObject());
    auto operationDocs = applyOpsInfo.getOperations();
    ASSERT_EQ(operationDocs.size(), 1U);
    auto outputInnerOp2 = repl::DurableReplOperation::parse(
        IDLParserContext{"RetryableFindAndModifyInsideInternalTransactionWithImageLookup"},
        operationDocs[0]);
    ASSERT_TRUE(outputInnerOp2.getPreImageOpTime());
    ASSERT_FALSE(outputInnerOp2.getNeedsRetryImage());
    ASSERT_EQ(preImageOplog.getOpTime(), *outputInnerOp2.getPreImageOpTime());
    ASSERT_EQ(OpType_serializer(repl::OpTypeEnum::kUpdate),
              OpType_serializer(outputInnerOp2.getOpType()));
    ASSERT_BSONOBJ_EQ(inputInnerOp2.getObject(), outputInnerOp2.getObject());
    ASSERT_TRUE(outputInnerOp2.getObject2());
    ASSERT_BSONOBJ_EQ(*inputInnerOp2.getObject2(), *outputInnerOp2.getObject2());

    auto applyOpsOplogDoc3 = pipeline->getNext();
    ASSERT_TRUE(applyOpsOplogDoc3);
    auto swOutputApplyOpsOplog3 = repl::DurableOplogEntry::parse(applyOpsOplogDoc3->toBson());
    ASSERT_OK(swOutputApplyOpsOplog3);
    auto outputApplyOpsOplog3 = swOutputApplyOpsOplog3.getValue();
    ASSERT_BSONOBJ_EQ(inputApplyOpsOplog3.toBSON().removeField(repl::OplogEntry::kObjectFieldName),
                      outputApplyOpsOplog3.toBSON().removeFields(StringDataSet{
                          repl::OplogEntry::kObjectFieldName, repl::OplogEntry::k_idFieldName}));

    ASSERT_FALSE(pipeline->getNext());

    // Create another pipeline and start fetching from after the doc for the pre-image, and verify
    // that the pipeline does not re-output the applyOps doc that comes before the pre-image doc.
    const auto startAt = ReshardingDonorOplogId::parse(
        IDLParserContext{"RetryableFindAndModifyInsideInternalTransactionWithImageLookup"},
        preImageOplog.get_id()->getDocument().toBson());
    auto newPipeline = createPipeline(startAt);

    auto next = newPipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_DOCUMENT_EQ(*next, *applyOpsOplogDoc2);

    next = newPipeline->getNext();
    ASSERT_TRUE(next);
    ASSERT_DOCUMENT_EQ(*next, *applyOpsOplogDoc3);

    ASSERT_FALSE(newPipeline->getNext());
}

}  // namespace
}  // namespace mongo
