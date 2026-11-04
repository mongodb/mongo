// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_end_of_transaction.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/change_stream_end_of_transaction_stage.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_constants.h"
#include "mongo/db/pipeline/change_stream_pipeline_helpers.h"
#include "mongo/db/pipeline/change_stream_reader_builder_mock.h"
#include "mongo/db/pipeline/change_stream_stage_test_fixture.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service_mock.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/split_pipeline.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"

#include <deque>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using namespace change_stream_test_helper;
using namespace std::literals::string_view_literals;

using exec::agg::ChangeStreamEndOfTransactionStage;
using exec::agg::EndOfTransactionControlEvent;
using exec::agg::EndOfTransactionScopes;
using exec::agg::GetNextResult;
using exec::agg::MockStage;

constexpr std::string_view kStageName = DocumentSourceChangeStreamEndOfTransaction::kStageName;

Document makeControlEvent(const Document& fields,
                          DocumentMetadataFields::ChangeStreamControlEventType type,
                          boost::optional<Value> sortKey = boost::none) {
    MutableDocument doc(fields);
    doc.metadata().setChangeStreamControlEvent(type);
    // A synthesized event inherits the control event's resume token as its sort key, which the
    // results merger uses to advance the client-visible post-batch resume token.
    if (sortKey) {
        doc.metadata().setSortKey(*sortKey, /*isSingleElementKey=*/true);
    }
    return doc.freeze();
}

Document makeAccountingControlEvent(const BSONObj& lsid,
                                    std::string_view shardId,
                                    Timestamp ts,
                                    long long txnNumber = 7) {
    const BSONObj scopes = BSON(
        "shardId" << shardId << "collectionParticipants"
                  << BSON("unittests.change_stream" << BSON_ARRAY("shardA" << "shardB"))
                  << "databaseParticipants" << BSON("unittests" << BSON_ARRAY("shardA" << "shardB"))
                  << "clusterParticipants" << BSON_ARRAY("shardA" << "shardB"));
    const BSONObj event =
        BSON("_id" << BSON("_data" << "resume-token") << "operationType"
                   << DocumentSourceChangeStream::kEndOfTransactionOpType << "clusterTime" << ts
                   << "wallTime" << Date_t() << "lsid" << lsid << "txnNumber" << txnNumber
                   << "endOfTransactionScopes" << scopes);
    return makeControlEvent(Document(event),
                            DocumentMetadataFields::ChangeStreamControlEventType::kAccounting,
                            Value(Document(BSON("_data" << "resume-token"))));
}

boost::intrusive_ptr<ChangeStreamEndOfTransactionStage> makeStage(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return make_intrusive<ChangeStreamEndOfTransactionStage>(kStageName, expCtx, nullptr);
}

// Creates a mock source stage seeded with 'inputs' and attaches it as the source of 'stage'. The
// returned stage must be kept alive for the duration of the test.
boost::intrusive_ptr<MockStage> setSource(
    std::deque<GetNextResult> inputs,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<ChangeStreamEndOfTransactionStage>& stage) {
    auto mock = MockStage::createForTest(std::move(inputs), expCtx);
    MockStage::setSource_forTest(stage, mock.get());
    return mock;
}

// Replaces the production post-batch resume token advancer with a no-op. The production advancer
// requires the topology-change stage as the source, but tests attach a mock source and do not
// exercise the merger's token.
void disablePbrtAdvancer(const boost::intrusive_ptr<ChangeStreamEndOfTransactionStage>& stage) {
    stage->setPbrtAdvancer_forTest([](const Document&) {});
}

bool pipelineContainsStage(const Pipeline& pipeline, std::string_view stageName) {
    for (const auto& stage : pipeline.getSources()) {
        if (stage->getSourceName() == stageName) {
            return true;
        }
    }
    return false;
}

void assertStageImmediatelyFollows(const Pipeline& pipeline,
                                   std::string_view predecessor,
                                   std::string_view successor) {
    const auto& sources = pipeline.getSources();
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
        if ((*it)->getSourceName() != predecessor) {
            continue;
        }
        auto next = std::next(it);
        ASSERT_TRUE(next != sources.cend()) << "no stage follows '" << predecessor << "'";
        ASSERT_EQ((*next)->getSourceName(), successor);
        return;
    }
    FAIL("predecessor stage not found in pipeline");
}

// Returns the end-of-transaction synthesis stage in 'pipeline', or nullptr if there is none.
DocumentSourceChangeStreamEndOfTransaction* findEndOfTransactionDS(const Pipeline& pipeline) {
    for (const auto& stage : pipeline.getSources()) {
        if (auto* ds = dynamic_cast<DocumentSourceChangeStreamEndOfTransaction*>(stage.get())) {
            return ds;
        }
    }
    return nullptr;
}

// Builds a user sub-pipeline from the given raw stages.
std::shared_ptr<Pipeline> makeUserPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const std::vector<BSONObj>& rawStages) {
    return pipeline_factory::makePipeline(rawStages, expCtx, pipeline_factory::kOptionsMinimal);
}

// Overrides the change stream spec's 'showEndOfTransaction' mode on 'expCtx'.
void setShowEndOfTransaction(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             ShowEndOfTransactionModeEnum mode) {
    auto spec = expCtx->getChangeStreamSpec();
    ASSERT_TRUE(spec);
    spec->setShowEndOfTransaction(mode);
    expCtx->setChangeStreamSpec(spec);
}

// Splits the pipeline and populates the synthesis stage's user sub-pipeline, mirroring what the
// mongos planner does after a split.
sharded_agg_helpers::SplitPipeline splitPipelineForTest(std::unique_ptr<Pipeline> pipeline) {
    auto split = sharded_agg_helpers::SplitPipeline::split(std::move(pipeline));
    change_stream::pipeline_helpers::setEndOfTransactionUserPipeline(*split.shardsPipeline,
                                                                     *split.mergePipeline);
    return split;
}

// The collection namespace the input change events belong to. The change stream's scope
// (collection/database/cluster) is expressed by the namespace on the ExpressionContext, not by the
// event namespace, so a single event namespace works for every scope.
const NamespaceString kEventNss =
    NamespaceString::createNamespaceString_forTest(boost::none, "unittests.change_stream");

NamespaceString changeStreamNamespaceForScope(ChangeStreamType scope) {
    switch (scope) {
        case ChangeStreamType::kCollection:
            return kEventNss;
        case ChangeStreamType::kDatabase:
            return NamespaceString::makeCollectionlessAggregateNSS(
                DatabaseName::createDatabaseName_forTest(boost::none, kEventNss.db_forTest()));
        case ChangeStreamType::kAllDatabases:
            return NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin);
    }
    MONGO_UNREACHABLE;
}

// Builds a user-visible change event (insert/update/delete) belonging to a transaction, in the
// shape the shards deliver to the router after transformation.
Document makeChangeEvent(std::string_view opType,
                         const LogicalSessionFromClient& lsid,
                         long long txnNumber,
                         Timestamp ts,
                         BSONObj documentKey = BSON("_id" << 1)) {
    return Document(BSON("_id" << BSON("_data" << "resume-token") << "operationType" << opType
                               << "clusterTime" << ts << "wallTime" << Date_t() << "ns"
                               << BSON("db" << kEventNss.db_forTest() << "coll" << kEventNss.coll())
                               << "documentKey" << documentKey << "lsid" << lsid.toBSON()
                               << "txnNumber" << txnNumber));
}

// Builds an 'endOfTransaction' accounting control event emitted by 'shardId', in the shape the
// shards deliver to the router. The 'endOfTransactionScopes' metadata carries the participants for
// the watched scope, so the router knows when the transaction is complete for that scope.
Document makeEndOfTransactionControlEvent(ChangeStreamType scope,
                                          std::string_view shardId,
                                          const LogicalSessionFromClient& lsid,
                                          long long txnNumber,
                                          Timestamp ts) {
    BSONObjBuilder scopes;
    scopes.append("shardId", shardId);
    switch (scope) {
        case ChangeStreamType::kCollection:
            scopes.append("collectionParticipants",
                          BSON(kEventNss.ns_forTest() << BSON_ARRAY("shardA" << "shardB")));
            break;
        case ChangeStreamType::kDatabase:
            scopes.append("databaseParticipants",
                          BSON(kEventNss.db_forTest() << BSON_ARRAY("shardA" << "shardB")));
            break;
        case ChangeStreamType::kAllDatabases:
            scopes.append("clusterParticipants", BSON_ARRAY("shardA" << "shardB"));
            break;
    }

    const BSONObj event =
        BSON("_id" << BSON("_data" << "resume-token") << "operationType"
                   << DocumentSourceChangeStream::kEndOfTransactionOpType << "clusterTime" << ts
                   << "wallTime" << Date_t() << "lsid" << lsid.toBSON() << "txnNumber" << txnNumber
                   << "endOfTransactionScopes" << scopes.obj());
    return makeControlEvent(Document(event),
                            DocumentMetadataFields::ChangeStreamControlEventType::kAccounting,
                            Value(Document(BSON("_data" << "resume-token"))));
}

// Builds the input stream for one transaction: an insert, update and delete event, followed by the
// 'endOfTransaction' control events from each of the two participant shards.
std::deque<GetNextResult> makeTransactionInputs(ChangeStreamType scope,
                                                const LogicalSessionFromClient& lsid,
                                                long long txnNumber) {
    std::deque<GetNextResult> inputs;
    inputs.push_back(makeChangeEvent("insert", lsid, txnNumber, Timestamp(100, 1)));
    inputs.push_back(makeChangeEvent("update", lsid, txnNumber, Timestamp(100, 2)));
    inputs.push_back(makeChangeEvent("delete", lsid, txnNumber, Timestamp(100, 3)));
    // The accounting control events are delivered after the transaction's change events, one per
    // participant shard, in ascending timestamp order.
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeEndOfTransactionControlEvent(scope, "shardA", lsid, txnNumber, Timestamp(100, 4))));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeEndOfTransactionControlEvent(scope, "shardB", lsid, txnNumber, Timestamp(100, 5))));
    return inputs;
}

// Returns the end-of-transaction synthesis stage in 'pipeline', or nullptr if there is none.
ChangeStreamEndOfTransactionStage* findEndOfTransactionExecStage(exec::agg::Pipeline& pipeline) {
    for (const auto& stage : pipeline.getStages()) {
        if (auto* endOfTransaction =
                dynamic_cast<ChangeStreamEndOfTransactionStage*>(stage.get())) {
            return endOfTransaction;
        }
    }
    return nullptr;
}

// Builds a V2 router change stream pipeline for 'rawPipeline' on 'scope', feeds 'inputs' into the
// end-of-transaction synthesis stage, and returns every document the pipeline emits. The topology
// change stage is bypassed (its source is replaced by 'inputs'): it only performs shard targeting,
// which is irrelevant to end-of-transaction synthesis.
std::vector<Document> runEndOfTransactionStream(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    ChangeStreamType scope,
    const std::vector<BSONObj>& rawPipeline,
    std::deque<GetNextResult> inputs) {
    unittest::ServerParameterGuard synthesisFlagEnabler(
        "featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    expCtx->setNamespaceString(changeStreamNamespaceForScope(scope));
    expCtx->setInRouter(true);

    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

    auto split = splitPipelineForTest(std::move(pipeline));

    auto execPipeline = exec::agg::buildPipeline(split.mergePipeline->freeze());
    auto* endOfTransactionStage = findEndOfTransactionExecStage(*execPipeline);
    tassert(13766909,
            "expected an end-of-transaction stage in the router merge pipeline",
            endOfTransactionStage);

    auto mock = MockStage::createForTest(std::move(inputs), expCtx);
    MockStage::setSource_forTest(endOfTransactionStage, mock.get());
    endOfTransactionStage->setPbrtAdvancer_forTest([](const Document&) {});

    std::vector<Document> results;
    while (true) {
        auto next = execPipeline->getNextResult();
        if (!next.isAdvanced()) {
            break;
        }
        results.push_back(next.releaseDocument());
    }

    // Dispose the pipeline while 'mock' is still alive: the pipeline's stages hold a raw pointer to
    // it, so it must not be destroyed first.
    execPipeline->dispose();
    return results;
}

}  // namespace

//
// Stage infrastructure: parsing, serialization.
//

TEST_F(ChangeStreamStageTest, EndOfTransactionCreateFromInvalidInput) {
    const BSONObj spec = BSON(kStageName << BSON("foo" << "bar"));
    ASSERT_THROWS_CODE(DocumentSourceChangeStreamEndOfTransaction::createFromBson(
                           spec.firstElement(), getExpCtx()),
                       AssertionException,
                       13766900);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionSerialization) {
    const BSONObj spec = BSON(kStageName << BSONObj());
    auto docSource = DocumentSourceChangeStreamEndOfTransaction::createFromBson(spec.firstElement(),
                                                                                getExpCtx());

    std::vector<Value> serialization;
    docSource->serializeToArray(serialization);

    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_BSONOBJ_EQ(serialization[0].getDocument().toBson(), BSON(kStageName << BSONObj()));
}

TEST_F(ChangeStreamStageTest, EndOfTransactionEmptyForQueryStats) {
    auto docSource = DocumentSourceChangeStreamEndOfTransaction::create(getExpCtx());

    ASSERT_BSONOBJ_EQ(BSON(kStageName << BSONObj()), docSource->serialize().getDocument().toBson());

    auto opts = query_shape::SerializationOptions{
        .literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue};
    ASSERT(docSource->serialize(opts).missing());
}

//
// Control event parsing and serialization.
//

namespace {

// Builds an end-of-transaction accounting control event with every field populated. The
// participants cover the collection, database and cluster scopes of 'kEventNss'.
Document makeFullControlEvent(const LogicalSessionFromClient& lsid, Timestamp clusterTime) {
    const BSONObj scopes = BSON(
        "shardId" << "shardA" << "collectionParticipants"
                  << BSON(kEventNss.ns_forTest() << BSON_ARRAY("shardA" << "shardB"))
                  << "databaseParticipants" << BSON(kEventNss.db_forTest() << BSON_ARRAY("shardA"))
                  << "clusterParticipants" << BSON_ARRAY("shardA" << "shardB" << "shardC"));
    return Document(BSON("_id" << BSON("_data" << "resume-token") << "operationType"
                               << DocumentSourceChangeStream::kEndOfTransactionOpType
                               << "clusterTime" << clusterTime << "wallTime"
                               << Date_t::fromMillisSinceEpoch(42) << "commitTimestamp"
                               << Timestamp(1, 2) << "lsid" << lsid.toBSON() << "txnNumber" << 7LL
                               << "endOfTransactionScopes" << scopes));
}

}  // namespace

TEST_F(ChangeStreamStageTest, EndOfTransactionControlEventParsesEveryField) {
    const auto lsid = testLsid();
    const auto parsed = EndOfTransactionControlEvent::createFromDocument(
        makeFullControlEvent(lsid, Timestamp(100, 5)));

    ASSERT_EQ(parsed.clusterTime, Timestamp(100, 5));
    ASSERT_EQ(parsed.wallTime, Date_t::fromMillisSinceEpoch(42));
    ASSERT_BSONOBJ_EQ(parsed.lsid.toBSON(), lsid.toBSON());
    ASSERT_EQ(parsed.txnNumber, 7LL);
    ASSERT_TRUE(parsed.commitTimestamp);
    ASSERT_EQ(*parsed.commitTimestamp, Timestamp(1, 2));

    ASSERT_EQ(parsed.scopes.shardId, ShardId("shardA"));
    ASSERT_EQ(parsed.scopes.clusterParticipants,
              (std::set<ShardId>{ShardId("shardA"), ShardId("shardB"), ShardId("shardC")}));
    ASSERT_EQ(parsed.scopes.collectionParticipants,
              (std::map<std::string, std::set<ShardId>>{
                  {std::string{kEventNss.ns_forTest()}, {ShardId("shardA"), ShardId("shardB")}}}));
    ASSERT_EQ(parsed.scopes.databaseParticipants,
              (std::map<std::string, std::set<ShardId>>{
                  {std::string{kEventNss.db_forTest()}, {ShardId("shardA")}}}));
}

TEST_F(ChangeStreamStageTest, EndOfTransactionControlEventWithoutAccountingMetadataTasserts) {
    // An accounting control event must carry the 'endOfTransactionScopes' metadata.
    const BSONObj event =
        BSON("_id" << BSON("_data" << "resume-token") << "operationType"
                   << DocumentSourceChangeStream::kEndOfTransactionOpType << "clusterTime"
                   << Timestamp(100, 5) << "wallTime" << Date_t::fromMillisSinceEpoch(42) << "lsid"
                   << testLsid().toBSON() << "txnNumber" << 7LL);
    ASSERT_TASSERT_CODE(EndOfTransactionControlEvent::createFromDocument(Document(event)),
                        13766915);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionControlEventWithoutCommitTimestampParses) {
    const auto lsid = testLsid();
    const BSONObj event =
        BSON("_id" << BSON("_data" << "resume-token") << "operationType"
                   << DocumentSourceChangeStream::kEndOfTransactionOpType << "clusterTime"
                   << Timestamp(100, 5) << "wallTime" << Date_t::fromMillisSinceEpoch(42) << "lsid"
                   << lsid.toBSON() << "txnNumber" << 7LL << "endOfTransactionScopes"
                   << BSON("shardId" << "shardA" << "clusterParticipants" << BSON_ARRAY("shardA")));
    const auto parsed = EndOfTransactionControlEvent::createFromDocument(Document(event));
    ASSERT_FALSE(parsed.commitTimestamp);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionControlEventMissingLsidThrows) {
    const BSONObj event =
        BSON("_id" << BSON("_data" << "resume-token") << "operationType"
                   << DocumentSourceChangeStream::kEndOfTransactionOpType << "clusterTime"
                   << Timestamp(100, 5) << "wallTime" << Date_t::fromMillisSinceEpoch(42)
                   << "txnNumber" << 7LL << "endOfTransactionScopes"
                   << BSON("shardId" << "shardA" << "clusterParticipants" << BSON_ARRAY("shardA")));
    ASSERT_THROWS_CODE(EndOfTransactionControlEvent::createFromDocument(Document(event)),
                       AssertionException,
                       13766904);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionScopesMissingShardIdThrows) {
    const BSONObj scopes = BSON("clusterParticipants" << BSON_ARRAY("shardA"));
    ASSERT_THROWS_CODE(
        EndOfTransactionScopes::createFromDocument(Document(scopes)), AssertionException, 13766901);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionControlEventSerializesToUserVisibleEvent) {
    const auto lsid = testLsid();
    const auto parsed = EndOfTransactionControlEvent::createFromDocument(
        makeFullControlEvent(lsid, Timestamp(100, 5)));
    const Value resumeToken{Document(BSON("_data" << "resume-token"))};

    // The serialized form is the user-visible 'endOfTransaction' event: it uses the resume token as
    // both the '_id' and the sort key, keeps the event fields, and omits the internal accounting
    // metadata.
    const Document event = parsed.toDocument(resumeToken);
    ASSERT_BSONOBJ_EQ(event["_id"].getDocument().toBson(), BSON("_data" << "resume-token"));
    ASSERT_VALUE_EQ(event.metadata().getSortKey(), resumeToken);
    ASSERT_EQ(event["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
    ASSERT_EQ(event["clusterTime"].getTimestamp(), Timestamp(100, 5));
    ASSERT_EQ(event["wallTime"].getDate(), Date_t::fromMillisSinceEpoch(42));
    ASSERT_EQ(event["commitTimestamp"].getTimestamp(), Timestamp(1, 2));
    ASSERT_BSONOBJ_EQ(event["lsid"].getDocument().toBson(), lsid.toBSON());
    ASSERT_EQ(event["txnNumber"].getLong(), 7LL);
    ASSERT_BSONOBJ_EQ(event["operationDescription"].getDocument().toBson(),
                      BSON("lsid" << lsid.toBSON() << "txnNumber" << 7LL));
    ASSERT(event["endOfTransactionScopes"].missing());
}

TEST_F(ChangeStreamStageTest, EndOfTransactionControlEventEquality) {
    const auto lsid = testLsid();
    const auto a = EndOfTransactionControlEvent::createFromDocument(
        makeFullControlEvent(lsid, Timestamp(100, 5)));
    const auto b = EndOfTransactionControlEvent::createFromDocument(
        makeFullControlEvent(lsid, Timestamp(100, 5)));
    const auto other = EndOfTransactionControlEvent::createFromDocument(
        makeFullControlEvent(lsid, Timestamp(200, 5)));
    ASSERT_TRUE(a == b);
    ASSERT_FALSE(a == other);
}

//
// Stage behavior.
//

TEST_F(ChangeStreamStageTest, EndOfTransactionPassesThroughNonControlEvents) {
    auto stage = makeStage(getExpCtx());

    std::deque<GetNextResult> inputs;
    inputs.push_back(GetNextResult(Document(BSON("operationType" << "insert" << "x" << 1))));
    inputs.push_back(GetNextResult::makePauseExecution());
    inputs.push_back(GetNextResult::makeEOF());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_EQ(next.getDocument()["operationType"].getStringData(), "insert");

    ASSERT_TRUE(stage->getNext().isPaused());
    ASSERT_TRUE(stage->getNext().isEOF());
}

TEST_F(ChangeStreamStageTest, EndOfTransactionUserDocumentWithMatchingOpTypePassesThrough) {
    auto stage = makeStage(getExpCtx());

    // A user document with 'operationType: "endOfTransaction"' is not a control event and must pass
    // through untouched.
    std::deque<GetNextResult> inputs;
    inputs.push_back(GetNextResult(
        Document(BSON("operationType" << DocumentSourceChangeStream::kEndOfTransactionOpType))));
    inputs.push_back(GetNextResult::makeEOF());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_EQ(next.getDocument()["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionFailsOnTopologyControlEvent) {
    auto stage = makeStage(getExpCtx());

    std::deque<GetNextResult> inputs;
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeControlEvent(Document(BSON("operationType" << "moveChunk")),
                         DocumentMetadataFields::ChangeStreamControlEventType::kTopology)));
    inputs.push_back(GetNextResult::makeEOF());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);

    // A topology control event is not an accounting event, so the stage rejects it.
    ASSERT_TASSERT_CODE(stage->getNext(), 13766906);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionFailsOnAccountingControlEventWithUnexpectedOpType) {
    auto stage = makeStage(getExpCtx());

    std::deque<GetNextResult> inputs;
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeControlEvent(Document(BSON("operationType" << "insert")),
                         DocumentMetadataFields::ChangeStreamControlEventType::kAccounting)));
    inputs.push_back(GetNextResult::makeEOF());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);

    // An accounting control event must carry the 'endOfTransaction' operation type.
    ASSERT_TASSERT_CODE(stage->getNext(), 13766903);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionAccumulatesAndEmitsOnCompletion) {
    const BSONObj lsid = testLsid().toBSON();
    const Timestamp tsA(100, 1);
    const Timestamp tsB(100, 2);

    std::deque<GetNextResult> inputs;
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardA", tsA)));
    inputs.push_back(GetNextResult(Document(BSON("operationType" << "insert" << "x" << 1))));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardB", tsB)));
    inputs.push_back(GetNextResult::makeEOF());

    auto stage = makeStage(getExpCtx());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);
    disablePbrtAdvancer(stage);

    // The first accounting record does not complete the transaction, so it is swallowed and the
    // unrelated event passes through.
    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_EQ(next.getDocument()["operationType"].getStringData(), "insert");
    ASSERT_EQ(stage->numTrackedTransactions_forTest(), 1UL);

    // The second accounting record completes the transaction, so the synthesized event is emitted
    // in the same getNext() call.
    next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    const Document& emitted = next.getDocument();
    ASSERT_FALSE(emitted.metadata().isChangeStreamControlEvent());
    ASSERT_EQ(emitted["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
    ASSERT_EQ(emitted["clusterTime"].getTimestamp(), tsB);
    ASSERT_BSONOBJ_EQ(emitted["_id"].getDocument().toBson(), BSON("_data" << "resume-token"));
    ASSERT(emitted["endOfTransactionScopes"].missing());
    ASSERT_EQ(stage->numTrackedTransactions_forTest(), 0UL);

    ASSERT_TRUE(stage->getNext().isEOF());
}

TEST_F(ChangeStreamStageTest, EndOfTransactionAdvancesPbrtOnlyWhenEventIsEmitted) {
    const BSONObj lsid = testLsid().toBSON();

    // The batch ends immediately after the second accounting record, which completes the
    // transaction and synthesizes the event.
    std::deque<GetNextResult> inputs;
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardA", Timestamp(100, 1))));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardB", Timestamp(100, 2))));
    inputs.push_back(GetNextResult::makeEOF());

    auto stage = makeStage(getExpCtx());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);

    std::vector<Document> advancedTokens;
    stage->setPbrtAdvancer_forTest(
        [&advancedTokens](const Document& doc) { advancedTokens.push_back(doc); });

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_EQ(next.getDocument()["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);

    // The batch ends immediately after the synthesized event.
    ASSERT_TRUE(stage->getNext().isEOF());

    // The synthesized event was forwarded exactly once, carrying the event's resume token as its
    // sort key, so the merger advances the post-batch resume token to the event rather than to a
    // high-water-mark token derived from the swallowed control event.
    ASSERT_EQ(advancedTokens.size(), 1UL);
    ASSERT_TRUE(advancedTokens[0].metadata().hasSortKey());
    ASSERT_VALUE_EQ(advancedTokens[0].metadata().getSortKey(),
                    next.getDocument().metadata().getSortKey());
    ASSERT_VALUE_EQ(advancedTokens[0]["_id"], next.getDocument()["_id"]);
}

//
// User sub-pipeline identification.
//

TEST_F(ChangeStreamStageTest, EndOfTransactionCapturesShardUserStagesInOrder) {
    unittest::ServerParameterGuard synthesisFlagEnabler(
        "featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("showExpandedEvents" << true << "version"
                                                          << "v2"
                                                          << "showEndOfTransaction"
                                                          << "whenAvailable")),
        fromjson("{$match: {operationType: 'insert'}}"),
        fromjson("{$project: {operationType: 1}}"),
        fromjson("{$addFields: {foo: 'bar'}}")};
    auto pipeline = buildTestPipelineForCollection(rawPipeline);
    auto split = splitPipelineForTest(std::move(pipeline));

    auto* ds = findEndOfTransactionDS(*split.mergePipeline);
    ASSERT_TRUE(ds);
    const auto& userPipeline = ds->getUserPipeline();
    ASSERT_TRUE(userPipeline);

    // Only the user-defined single-document transformation stages are captured, in shard order.
    std::vector<std::string_view> names;
    for (const auto& stage : userPipeline->getSources()) {
        names.push_back(stage->getSourceName());
    }
    ASSERT_EQ(names.size(), 3UL);
    ASSERT_EQ(names[0], "$match");
    ASSERT_EQ(names[1], "$project");
    ASSERT_EQ(names[2], "$addFields");
}

TEST_F(ChangeStreamStageTest, EndOfTransactionHasNoUserPipelineWithoutUserStages) {
    unittest::ServerParameterGuard synthesisFlagEnabler(
        "featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("showExpandedEvents" << true << "version"
                                                          << "v2"
                                                          << "showEndOfTransaction"
                                                          << "whenAvailable"))};
    auto pipeline = buildTestPipelineForCollection(rawPipeline);
    auto split = splitPipelineForTest(std::move(pipeline));

    auto* ds = findEndOfTransactionDS(*split.mergePipeline);
    ASSERT_TRUE(ds);
    ASSERT_FALSE(ds->getUserPipeline());
}

TEST_F(ChangeStreamStageTest, EndOfTransactionCapturesOnlySingleDocumentTransformationStages) {
    unittest::ServerParameterGuard synthesisFlagEnabler(
        "featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("showExpandedEvents" << true << "version"
                                                          << "v2"
                                                          << "showEndOfTransaction"
                                                          << "whenAvailable")),
        fromjson("{$redact: {$cond: [true, '$$DESCEND', '$$PRUNE']}}")};
    auto pipeline = buildTestPipelineForCollection(rawPipeline);
    auto split = splitPipelineForTest(std::move(pipeline));

    auto* ds = findEndOfTransactionDS(*split.mergePipeline);
    ASSERT_TRUE(ds);
    const auto& userPipeline = ds->getUserPipeline();
    ASSERT_TRUE(userPipeline);
    ASSERT_EQ(userPipeline->getSources().size(), 1UL);
    ASSERT_EQ(userPipeline->getSources().front()->getSourceName(), "$redact");
}

TEST_F(ChangeStreamStageTest, EndOfTransactionDoesNotCaptureSplitLargeEventStage) {
    unittest::ServerParameterGuard synthesisFlagEnabler(
        "featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("showExpandedEvents" << true << "version"
                                                          << "v2"
                                                          << "showEndOfTransaction"
                                                          << "whenAvailable")),
        fromjson("{$match: {operationType: 'insert'}}"),
        fromjson("{$changeStreamSplitLargeEvent: {}}")};
    auto pipeline = buildTestPipelineForCollection(rawPipeline);
    auto split = splitPipelineForTest(std::move(pipeline));

    auto* ds = findEndOfTransactionDS(*split.mergePipeline);
    ASSERT_TRUE(ds);
    const auto& userPipeline = ds->getUserPipeline();
    ASSERT_TRUE(userPipeline);

    // '$changeStreamSplitLargeEvent' runs on the shards only and is stateful (its output can fall
    // out of step with its input), so it must never be re-applied to synthesized events on the
    // router. Only the single-document transformation stages are re-applied.
    std::vector<std::string_view> names;
    for (const auto& stage : userPipeline->getSources()) {
        names.push_back(stage->getSourceName());
    }
    ASSERT_EQ(names.size(), 1UL);
    ASSERT_EQ(names[0], "$match");
}

//
// User sub-pipeline execution.
//

TEST_F(ChangeStreamStageTest, EndOfTransactionUserPipelineFiltersSynthesizedEvent) {
    auto userPipeline =
        makeUserPipeline(getExpCtx(), {fromjson("{$match: {operationType: 'insert'}}")});
    auto stage = make_intrusive<ChangeStreamEndOfTransactionStage>(
        kStageName, getExpCtx(), std::move(userPipeline));

    const BSONObj lsid = testLsid().toBSON();
    std::deque<GetNextResult> inputs;
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardA", Timestamp(100, 1))));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardB", Timestamp(100, 2))));
    inputs.push_back(GetNextResult::makeEOF());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);

    // The synthesized event has 'operationType: endOfTransaction', which the user '$match' filters
    // out, so nothing is emitted.
    ASSERT_TRUE(stage->getNext().isEOF());
}

TEST_F(ChangeStreamStageTest, EndOfTransactionUserPipelineTransformsSynthesizedEvent) {
    auto userPipeline =
        makeUserPipeline(getExpCtx(), {fromjson("{$addFields: {synthesized: true}}")});
    auto stage = make_intrusive<ChangeStreamEndOfTransactionStage>(
        kStageName, getExpCtx(), std::move(userPipeline));

    const BSONObj lsid = testLsid().toBSON();
    std::deque<GetNextResult> inputs;
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardA", Timestamp(100, 1))));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardB", Timestamp(100, 2))));
    inputs.push_back(GetNextResult::makeEOF());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);
    disablePbrtAdvancer(stage);

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_EQ(next.getDocument()["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
    ASSERT_TRUE(next.getDocument()["synthesized"].getBool());
}

TEST_F(ChangeStreamStageTest, EndOfTransactionUserPipelineFilteredEventDoesNotAdvancePbrt) {
    auto userPipeline =
        makeUserPipeline(getExpCtx(), {fromjson("{$match: {operationType: 'insert'}}")});
    auto stage = make_intrusive<ChangeStreamEndOfTransactionStage>(
        kStageName, getExpCtx(), std::move(userPipeline));

    std::vector<Document> advancedTokens;
    stage->setPbrtAdvancer_forTest(
        [&advancedTokens](const Document& doc) { advancedTokens.push_back(doc); });

    const BSONObj lsid = testLsid().toBSON();
    std::deque<GetNextResult> inputs;
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardA", Timestamp(100, 1))));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardB", Timestamp(100, 2))));
    inputs.push_back(GetNextResult::makeEOF());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);

    ASSERT_TRUE(stage->getNext().isEOF());
    ASSERT_TRUE(advancedTokens.empty());
    ASSERT_EQ(stage->numTrackedTransactions_forTest(), 0UL);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionUserPipelineSurvivingEventAdvancesPbrt) {
    auto userPipeline =
        makeUserPipeline(getExpCtx(), {fromjson("{$addFields: {synthesized: true}}")});
    auto stage = make_intrusive<ChangeStreamEndOfTransactionStage>(
        kStageName, getExpCtx(), std::move(userPipeline));

    std::vector<Document> advancedTokens;
    stage->setPbrtAdvancer_forTest(
        [&advancedTokens](const Document& doc) { advancedTokens.push_back(doc); });

    const BSONObj lsid = testLsid().toBSON();
    std::deque<GetNextResult> inputs;
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardA", Timestamp(100, 1))));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardB", Timestamp(100, 2))));
    inputs.push_back(GetNextResult::makeEOF());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);

    auto next = stage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_TRUE(next.getDocument()["synthesized"].getBool());
    ASSERT_EQ(advancedTokens.size(), 1UL);
    ASSERT_TRUE(advancedTokens[0]["synthesized"].getBool());
}

TEST_F(ChangeStreamStageTest, EndOfTransactionUserPipelineIsReusedForMultipleTransactions) {
    auto userPipeline =
        makeUserPipeline(getExpCtx(), {fromjson("{$addFields: {synthesized: true}}")});
    auto stage = make_intrusive<ChangeStreamEndOfTransactionStage>(
        kStageName, getExpCtx(), std::move(userPipeline));

    const BSONObj lsid = testLsid().toBSON();
    std::deque<GetNextResult> inputs;
    // Two independent transactions, each completed by both shards.
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardA", Timestamp(100, 1), /*txnNumber=*/7)));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardB", Timestamp(100, 2), /*txnNumber=*/7)));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardA", Timestamp(200, 1), /*txnNumber=*/8)));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(
        makeAccountingControlEvent(lsid, "shardB", Timestamp(200, 2), /*txnNumber=*/8)));
    inputs.push_back(GetNextResult::makeEOF());
    auto mock = setSource(std::move(inputs), getExpCtx(), stage);
    disablePbrtAdvancer(stage);

    auto first = stage->getNext();
    ASSERT_TRUE(first.isAdvanced());
    ASSERT_TRUE(first.getDocument()["synthesized"].getBool());
    ASSERT_EQ(first.getDocument()["clusterTime"].getTimestamp(), Timestamp(100, 2));

    auto second = stage->getNext();
    ASSERT_TRUE(second.isAdvanced());
    ASSERT_TRUE(second.getDocument()["synthesized"].getBool());
    ASSERT_EQ(second.getDocument()["clusterTime"].getTimestamp(), Timestamp(200, 2));

    ASSERT_TRUE(stage->getNext().isEOF());
}

//
// Explain output.
//

TEST_F(ChangeStreamStageTest, EndOfTransactionExplainReportsUserPipeline) {
    unittest::ServerParameterGuard synthesisFlagEnabler(
        "featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("showExpandedEvents" << true << "version"
                                                          << "v2"
                                                          << "showEndOfTransaction"
                                                          << "whenAvailable")),
        fromjson("{$match: {operationType: 'insert'}}"),
        fromjson("{$project: {operationType: 1}}")};
    auto pipeline = buildTestPipelineForCollection(rawPipeline);
    auto split = splitPipelineForTest(std::move(pipeline));

    auto* ds = findEndOfTransactionDS(*split.mergePipeline);
    ASSERT_TRUE(ds);

    query_shape::SerializationOptions opts;
    opts.verbosity = ExplainOptions::Verbosity::kQueryPlanner;
    const Value serialized = ds->serialize(opts);

    const Value pipelineField =
        serialized.getDocument()[DocumentSourceChangeStream::kStageName]["pipeline"];
    ASSERT_EQ(pipelineField.getType(), BSONType::array);
    ASSERT_EQ(pipelineField.getArray().size(), 2UL);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionUserPipelineIsNeutralForQueryShape) {
    unittest::ServerParameterGuard synthesisFlagEnabler(
        "featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);
    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("showExpandedEvents" << true << "version"
                                                          << "v2"
                                                          << "showEndOfTransaction"
                                                          << "whenAvailable")),
        fromjson("{$match: {operationType: 'insert'}}"),
        fromjson("{$project: {operationType: 1}}")};
    auto pipeline = buildTestPipelineForCollection(rawPipeline);
    auto split = splitPipelineForTest(std::move(pipeline));

    auto* ds = findEndOfTransactionDS(*split.mergePipeline);
    ASSERT_TRUE(ds);
    ASSERT_TRUE(ds->getUserPipeline());

    // The query shape hash is computed from a shapifying serialization, which omits internal change
    // stream stages entirely. The user sub-pipeline reported in the explain output therefore cannot
    // affect the query shape hash.
    ASSERT_TRUE(
        ds->serialize(query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions)
            .missing());
}

//
// Pipeline placement.
//

TEST_F(ChangeStreamStageTest, EndOfTransactionFollowsTopologyStageInV2CollectionPipeline) {
    unittest::ServerParameterGuard synthesisFlagEnabler(
        "featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);

    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("showExpandedEvents" << true << "version"
                                                          << "v2"
                                                          << "showEndOfTransaction"
                                                          << "whenAvailable"))};

    auto pipeline = buildTestPipelineForCollection(rawPipeline);

    assertStageImmediatelyFollows(
        *pipeline, change_stream_constants::stage_names::kHandleTopologyChangeV2, kStageName);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionFollowsTopologyStageInV2DatabasePipeline) {
    unittest::ServerParameterGuard synthesisFlagEnabler(
        "featureFlagEndOfTransactionChangeEventSynthesis", true);
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);

    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("showExpandedEvents" << true << "version"
                                                          << "v2"
                                                          << "showEndOfTransaction"
                                                          << "required"))};

    auto pipeline = buildTestPipelineForDatabase(rawPipeline);

    assertStageImmediatelyFollows(
        *pipeline, change_stream_constants::stage_names::kHandleTopologyChangeV2, kStageName);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionAbsentWithoutOptInInV2RouterPipeline) {
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);

    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("showExpandedEvents" << true << "version" << "v2"))};

    auto pipeline = buildTestPipelineForCollection(rawPipeline);

    ASSERT_TRUE(pipelineContainsStage(
        *pipeline, change_stream_constants::stage_names::kHandleTopologyChangeV2));
    ASSERT_FALSE(pipelineContainsStage(*pipeline, kStageName));
}

TEST_F(ChangeStreamStageTest, EndOfTransactionAbsentWhenOptionIsOffInV2RouterPipeline) {
    unittest::ServerParameterGuard preciseShardTargetingEnabler(
        "featureFlagChangeStreamPreciseShardTargeting", true);
    unittest::ServerParameterGuard changeStreamReaderV2Enabler("featureFlagChangeStreamReaderV2",
                                                               true);

    ScopedDataToShardsAllocationQueryServiceMock queryServiceMock;
    ScopedChangeStreamReaderBuilderMock readerBuilder(
        std::make_unique<ChangeStreamReaderBuilderMock>());

    const std::vector<BSONObj> rawPipeline = {
        BSON("$changeStream" << BSON("showExpandedEvents" << true << "version"
                                                          << "v2"
                                                          << "showEndOfTransaction"
                                                          << "off"))};

    auto pipeline = buildTestPipelineForCollection(rawPipeline);

    ASSERT_TRUE(pipelineContainsStage(
        *pipeline, change_stream_constants::stage_names::kHandleTopologyChangeV2));
    ASSERT_FALSE(pipelineContainsStage(*pipeline, kStageName));
}

TEST_F(ChangeStreamStageTest, EndOfTransactionAbsentInV1RouterPipeline) {
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec};

    auto pipeline = buildTestPipelineForCollection(rawPipeline);

    ASSERT_TRUE(pipelineContainsStage(*pipeline,
                                      change_stream_constants::stage_names::kHandleTopologyChange));
    ASSERT_FALSE(pipelineContainsStage(*pipeline, kStageName));
}

TEST_F(ChangeStreamStageTest, EndOfTransactionAbsentWhenNotOnRouter) {
    const std::vector<BSONObj> rawPipeline = {kShowExpandedEventsSpec};

    auto expCtx = getExpCtx();
    expCtx->setNamespaceString(
        NamespaceString::createNamespaceString_forTest(boost::none, "a.collection"));
    expCtx->setInRouter(false);

    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

    ASSERT_FALSE(pipelineContainsStage(
        *pipeline, change_stream_constants::stage_names::kHandleTopologyChangeV2));
    ASSERT_FALSE(pipelineContainsStage(*pipeline, kStageName));
}

//
// End-to-end synthesis through the router merge pipeline.
//

const BSONObj kEndOfTransactionSpec =
    BSON("$changeStream" << BSON("version" << "v2" << "showEndOfTransaction" << "whenAvailable"));
const BSONObj kEndOfTransactionExpandedSpec = BSON(
    "$changeStream" << BSON("showExpandedEvents" << true << "version" << "v2"
                                                 << "showEndOfTransaction" << "whenAvailable"));
const BSONObj kEndOfTransactionClusterSpec = BSON(
    "$changeStream" << BSON("allChangesForCluster" << true << "showExpandedEvents" << true
                                                   << "version" << "v2" << "showEndOfTransaction"
                                                   << "whenAvailable"));

TEST_F(ChangeStreamStageTest, EndOfTransactionStreamSynthesizesForCollectionScope) {
    const auto lsid = testLsid();
    auto results = runEndOfTransactionStream(
        getExpCtx(),
        ChangeStreamType::kCollection,
        {kEndOfTransactionExpandedSpec},
        makeTransactionInputs(ChangeStreamType::kCollection, lsid, /*txnNumber=*/7));

    ASSERT_EQ(results.size(), 4UL);
    ASSERT_EQ(results[0]["operationType"].getStringData(), "insert");
    ASSERT_EQ(results[1]["operationType"].getStringData(), "update");
    ASSERT_EQ(results[2]["operationType"].getStringData(), "delete");
    ASSERT_EQ(results[3]["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
    ASSERT_EQ(results[3]["clusterTime"].getTimestamp(), Timestamp(100, 5));
    ASSERT_EQ(results[3]["txnNumber"].getLong(), 7LL);
    ASSERT_BSONOBJ_EQ(results[3]["_id"].getDocument().toBson(), BSON("_data" << "resume-token"));
    ASSERT_BSONOBJ_EQ(results[3]["operationDescription"].getDocument().toBson(),
                      BSON("lsid" << lsid.toBSON() << "txnNumber" << 7LL));
    ASSERT(results[3]["endOfTransactionScopes"].missing());
}

TEST_F(ChangeStreamStageTest, EndOfTransactionStreamSynthesizesForDatabaseScope) {
    const auto lsid = testLsid();
    auto results = runEndOfTransactionStream(
        getExpCtx(),
        ChangeStreamType::kDatabase,
        {kEndOfTransactionExpandedSpec},
        makeTransactionInputs(ChangeStreamType::kDatabase, lsid, /*txnNumber=*/7));

    ASSERT_EQ(results.size(), 4UL);
    ASSERT_EQ(results[3]["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
    ASSERT_EQ(results[3]["clusterTime"].getTimestamp(), Timestamp(100, 5));
}

TEST_F(ChangeStreamStageTest, EndOfTransactionStreamSynthesizesForClusterScope) {
    const auto lsid = testLsid();
    auto results = runEndOfTransactionStream(
        getExpCtx(),
        ChangeStreamType::kAllDatabases,
        {kEndOfTransactionClusterSpec},
        makeTransactionInputs(ChangeStreamType::kAllDatabases, lsid, /*txnNumber=*/7));

    ASSERT_EQ(results.size(), 4UL);
    ASSERT_EQ(results[3]["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
    ASSERT_EQ(results[3]["clusterTime"].getTimestamp(), Timestamp(100, 5));
}

TEST_F(ChangeStreamStageTest, EndOfTransactionStreamSynthesizesWithoutShowExpandedEvents) {
    const auto lsid = testLsid();
    auto results = runEndOfTransactionStream(
        getExpCtx(),
        ChangeStreamType::kCollection,
        {kEndOfTransactionSpec},
        makeTransactionInputs(ChangeStreamType::kCollection, lsid, /*txnNumber=*/7));

    // The classic operation-type filter is present when 'showExpandedEvents' is off; it admits
    // 'endOfTransaction' because the stream opted in.
    ASSERT_EQ(results.size(), 4UL);
    ASSERT_EQ(results[3]["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionStreamUserMatchFiltersSynthesizedEvent) {
    const auto lsid = testLsid();
    std::deque<GetNextResult> inputs;
    inputs.push_back(makeChangeEvent("insert", lsid, /*txnNumber=*/7, Timestamp(100, 1)));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(makeEndOfTransactionControlEvent(
        ChangeStreamType::kCollection, "shardA", lsid, /*txnNumber=*/7, Timestamp(100, 4))));
    inputs.push_back(GetNextResult::makeAdvancedControlDocument(makeEndOfTransactionControlEvent(
        ChangeStreamType::kCollection, "shardB", lsid, /*txnNumber=*/7, Timestamp(100, 5))));

    auto results = runEndOfTransactionStream(
        getExpCtx(),
        ChangeStreamType::kCollection,
        {kEndOfTransactionExpandedSpec, fromjson("{$match: {operationType: 'insert'}}")},
        std::move(inputs));

    // The user '$match' runs on the shards, so only the insert reaches the router; the synthesized
    // 'endOfTransaction' event is filtered out by the re-applied user pipeline.
    ASSERT_EQ(results.size(), 1UL);
    ASSERT_EQ(results[0]["operationType"].getStringData(), "insert");
}

TEST_F(ChangeStreamStageTest, EndOfTransactionStreamUserProjectTransformsSynthesizedEvent) {
    const auto lsid = testLsid();
    auto results = runEndOfTransactionStream(
        getExpCtx(),
        ChangeStreamType::kCollection,
        {kEndOfTransactionExpandedSpec, fromjson("{$project: {operationType: 1, txnNumber: 1}}")},
        makeTransactionInputs(ChangeStreamType::kCollection, lsid, /*txnNumber=*/7));

    // The synthesized 'endOfTransaction' event is projected down to the requested fields.
    ASSERT_EQ(results.size(), 4UL);
    ASSERT_EQ(results[3]["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
    ASSERT_EQ(results[3]["txnNumber"].getLong(), 7LL);
    ASSERT(results[3]["clusterTime"].missing());
}

TEST_F(ChangeStreamStageTest, EndOfTransactionStreamDoesNotReapplySplitLargeEvent) {
    const auto lsid = testLsid();
    auto results = runEndOfTransactionStream(
        getExpCtx(),
        ChangeStreamType::kCollection,
        {kEndOfTransactionExpandedSpec, fromjson("{$changeStreamSplitLargeEvent: {}}")},
        makeTransactionInputs(ChangeStreamType::kCollection, lsid, /*txnNumber=*/7));

    // '$changeStreamSplitLargeEvent' runs on the shards only, so the router emits the synthesized
    // event without splitting it.
    ASSERT_EQ(results.size(), 4UL);
    ASSERT_EQ(results[3]["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
}

TEST_F(ChangeStreamStageTest, EndOfTransactionStreamSynthesizesForMultipleTransactions) {
    const auto lsid = testLsid();
    std::deque<GetNextResult> inputs;
    for (auto txnNumber : {7LL, 8LL}) {
        auto txn = makeTransactionInputs(ChangeStreamType::kCollection, lsid, txnNumber);
        inputs.insert(
            inputs.end(), std::make_move_iterator(txn.begin()), std::make_move_iterator(txn.end()));
    }

    auto results = runEndOfTransactionStream(getExpCtx(),
                                             ChangeStreamType::kCollection,
                                             {kEndOfTransactionExpandedSpec},
                                             std::move(inputs));

    ASSERT_EQ(results.size(), 8UL);
    ASSERT_EQ(results[3]["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
    ASSERT_EQ(results[3]["txnNumber"].getLong(), 7LL);
    ASSERT_EQ(results[7]["operationType"].getStringData(),
              DocumentSourceChangeStream::kEndOfTransactionOpType);
    ASSERT_EQ(results[7]["txnNumber"].getLong(), 8LL);
}

}  // namespace mongo
