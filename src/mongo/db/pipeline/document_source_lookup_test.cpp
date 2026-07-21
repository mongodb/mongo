// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_lookup.h"

#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/document_source_lookup_test_util.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/pipeline/serverless_aggregation_context_fixture.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <deque>
#include <initializer_list>
#include <iostream>
#include <list>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using namespace test;

class DocumentSourceLookUpTest : public AggregationContextFixture {
protected:
    DocumentSourceLookUpTest() {
        ShardingState::create(getServiceContext());
        // By default, make a mock mongo interface without any results from the foreign collection.
        // Individual tests will make their own interface if they need mock results.
        getExpCtx()->setMongoProcessInterface(
            std::make_shared<DocumentSourceLookupMockMongoInterface>(
                std::deque<DocumentSource::GetNextResult>{}));
    }
};

const auto kExplain = query_shape::SerializationOptions{
    .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)};

// For tests which need to run in a replica set context.
class ReplDocumentSourceLookUpTest : public DocumentSourceLookUpTest {
public:
    void setUp() override {
        DocumentSourceLookUpTest::setUp();  // Will establish a feature compatibility version.
        auto service = getExpCtx()->getOperationContext()->getServiceContext();
        repl::ReplSettings settings;

        settings.setReplSetString("lookupTestSet/node1:12345");

        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service, settings);

        // Ensure that we are primary.
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
    }
};

// A 'let' variable defined in a $lookup stage is expected to be available to all sub-pipelines. For
// sub-pipelines below the immediate one, they are passed to via ExpressionContext. This test
// confirms that variables defined in the ExpressionContext are captured by the $lookup stage.
TEST_F(DocumentSourceLookUpTest, PreservesParentPipelineLetVariables) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto varId = expCtx->variablesParseState.defineVariable("foo");
    expCtx->variables.setValue(varId, Value(123));

    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << "coll"
                                      << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                      << "as"
                                      << "as"))
            .firstElement(),
        expCtx);
    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    ASSERT_EQ(varId, lookupStage->getVariablesParseState_forTest().getVariable("foo"));
    ASSERT_VALUE_EQ(Value(123), lookupStage->getVariables_forTest().getValue(varId, Document()));
}

TEST_F(DocumentSourceLookUpTest, AcceptsPipelineSyntax) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << "coll"
                                      << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                      << "as"
                                      << "as"))
            .firstElement(),
        expCtx);
    auto lookup = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT_TRUE(lookup->hasPipeline());
    ASSERT_FALSE(lookup->hasLocalFieldForeignFieldJoin());
}

TEST_F(DocumentSourceLookUpTest, AcceptsPipelineWithLetSyntax) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << "coll"
                                      << "let" << BSON("var1" << "$x") << "pipeline"
                                      << BSON_ARRAY(BSON("$project" << BSON("hasX" << "$$var1"))
                                                    << BSON("$match" << BSON("hasX" << true)))
                                      << "as"
                                      << "as"))
            .firstElement(),
        expCtx);
    auto lookup = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT_TRUE(lookup->hasPipeline());
    ASSERT_FALSE(lookup->hasLocalFieldForeignFieldJoin());
}

TEST_F(DocumentSourceLookUpTest, LookupEmptyPipelineDoesntUseDiskAndIsOKInATransaction) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << fromNs.coll() << "pipeline" << BSONArray() << "as"
                                      << "as"))
            .firstElement(),
        expCtx);
    auto lookup = static_cast<DocumentSourceLookUp*>(docSource.get());

    ASSERT_FALSE(lookup->hasLocalFieldForeignFieldJoin());
    ASSERT(lookup->constraints(PipelineSplitState::kUnsplit).diskRequirement ==
           DocumentSource::DiskUseRequirement::kNoDiskUse);
    ASSERT(lookup->constraints(PipelineSplitState::kUnsplit).transactionRequirement ==
           DocumentSource::TransactionRequirement::kAllowed);
}

TEST_F(DocumentSourceLookUpTest, LookupWithOutInPipelineNotAllowed) {
    auto ERROR_CODE_OUT_BANNED_IN_LOOKUP = 51047;
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from" << "coll"
                                          << "pipeline" << BSON_ARRAY(BSON("$out" << "target"))
                                          << "as"
                                          << "as"))
                .firstElement(),
            expCtx),
        AssertionException,
        ERROR_CODE_OUT_BANNED_IN_LOOKUP);
}

TEST_F(DocumentSourceLookUpTest, LiteParsedDocumentSourceLookupContainsExpectedNamespaces) {
    auto stageSpec = BSON(
        "$lookup" << BSON(
            "from" << "namespace1"
                   << "pipeline"
                   << BSON_ARRAY(BSON("$lookup" << BSON(
                                          "from" << "namespace2"
                                                 << "as"
                                                 << "lookup2"
                                                 << "pipeline"
                                                 << BSON_ARRAY(BSON("$match" << BSON("x" << 1))))))
                   << "as"
                   << "lookup1"));

    auto expCtx = getExpCtx();

    std::vector<BSONObj> pipeline;
    auto liteParsedLookup = LiteParsedLookUp::parse(
        expCtx->getNamespaceString(), stageSpec.firstElement(), LiteParserOptions{});
    auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();

    ASSERT_EQ(1ul,
              namespaceSet.count(
                  NamespaceString::createNamespaceString_forTest(boost::none, "test.namespace1")));
    ASSERT_EQ(1ul,
              namespaceSet.count(
                  NamespaceString::createNamespaceString_forTest(boost::none, "test.namespace2")));
}

TEST_F(DocumentSourceLookUpTest, RejectLookupWhenDepthLimitIsExceeded) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    expCtx->setSubPipelineDepth(internalMaxSubPipelineViewDepth.load());

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from" << "coll"
                                          << "pipeline"
                                          << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                                          << "as"))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::MaxSubPipelineDepthExceeded);
}

TEST_F(ReplDocumentSourceLookUpTest, RejectsPipelineWithChangeStreamStage) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // Verify that attempting to create a $lookup pipeline containing a $changeStream stage fails.
    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            fromjson("{$lookup: {from: 'coll', as: 'as', pipeline: [{$changeStream: {}}]}}")
                .firstElement(),
            expCtx),
        AssertionException,
        51047);
}

TEST_F(ReplDocumentSourceLookUpTest, RejectsSubPipelineWithChangeStreamStage) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // Verify that attempting to create a sub-$lookup pipeline containing a $changeStream stage
    // fails at parse time, even if the outer pipeline does not have a $changeStream stage.
    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            fromjson("{$lookup: {from: 'coll', as: 'as', pipeline: [{$match: {_id: 1}}, {$lookup: "
                     "{from: 'coll', as: 'subas', pipeline: [{$changeStream: {}}]}}]}}")
                .firstElement(),
            expCtx),
        AssertionException,
        51047);
}

TEST_F(DocumentSourceLookUpTest, AcceptsLocalFieldForeignFieldAndPipelineSyntax) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    try {
        auto docSource = DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from" << "coll"
                                          << "pipeline"
                                          << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                          << "localField"
                                          << "a"
                                          << "foreignField"
                                          << "b"
                                          << "as"
                                          << "as"))
                .firstElement(),
            expCtx);
        auto lookup = static_cast<DocumentSourceLookUp*>(docSource.get());
        ASSERT_TRUE(lookup->hasLocalFieldForeignFieldJoin());
        ASSERT_TRUE(lookup->hasPipeline());
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ErrorCodes::FailedToParse, ex.code());
    }
}

TEST_F(DocumentSourceLookUpTest, AcceptsLocalFieldForeignFieldAndPipelineWithLetSyntax) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    try {
        auto docSource = DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from" << "coll"
                                          << "let" << BSON("var1" << "$x") << "pipeline"
                                          << BSON_ARRAY(BSON("$project" << BSON("hasX" << "$$var1"))
                                                        << BSON("$match" << BSON("hasX" << true)))
                                          << "localField"
                                          << "a"
                                          << "foreignField"
                                          << "b"
                                          << "as"
                                          << "as"))
                .firstElement(),
            expCtx);
        auto lookup = static_cast<DocumentSourceLookUp*>(docSource.get());
        ASSERT_TRUE(lookup->hasLocalFieldForeignFieldJoin());
        ASSERT_TRUE(lookup->hasPipeline());
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ErrorCodes::FailedToParse, ex.code());
    }
}

TEST_F(DocumentSourceLookUpTest, RejectsLocalFieldForeignFieldWhenLetIsSpecified) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from" << "coll"
                                          << "let" << BSON("var1" << "$a") << "localField"
                                          << "a"
                                          << "foreignField"
                                          << "b"
                                          << "as"
                                          << "as"))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceLookUpTest, RejectsInvalidLetVariableName) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from" << "coll"
                                          << "let"
                                          << BSON(""  // Empty variable name.
                                                  << "$a")
                                          << "pipeline"
                                          << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                                          << "as"))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::FailedToParse);

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from" << "coll"
                                          << "let" << BSON("^invalidFirstChar" << "$a")
                                          << "pipeline"
                                          << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                                          << "as"))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::FailedToParse);

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from" << "coll"
                                          << "let" << BSON("contains.invalidChar" << "$a")
                                          << "pipeline"
                                          << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                                          << "as"))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceLookUpTest, ShouldBeAbleToReParseSerializedStage) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << "coll"
                                      << "let" << BSON("local_x" << "$x") << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                                      << "as"))
            .firstElement(),
        expCtx);

    //
    // Serialize the $lookup stage and confirm contents.
    //
    std::vector<Value> serialization;
    lookupStage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);

    // The fields are in no guaranteed order, so we can't perform a simple Document comparison.
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::object);

    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage.computeSize(), 4ULL);
    ASSERT_VALUE_EQ(serializedStage["from"], Value(std::string("coll")));
    ASSERT_VALUE_EQ(serializedStage["as"], Value(std::string("as")));

    ASSERT_DOCUMENT_EQ(serializedStage["let"].getDocument(),
                       Document(fromjson("{local_x: \"$x\"}")));

    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::array);
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 1UL);

    ASSERT_EQ(serializedStage["pipeline"][0].getType(), BSONType::object);
    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][0]["$match"].getDocument(),
                       Document(fromjson("{x: 1}")));

    //
    // Create a new $lookup stage from the serialization. Serialize the new stage and confirm that
    // it is equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBson.firstElement(), expCtx);

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceLookUpTest, ShouldBeAbleToReParseSerializedStageWithUnwind) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupSpec = fromjson(
        "{$lookup: { from: 'coll', as: 'asField', pipeline: [{$match: {subfield: {$eq: 1}}}]}}");
    auto unwindSpec = fromjson("{$unwind: '$asField'}");
    auto pipeline = pipeline_factory::makePipeline(
        makeVector(lookupSpec, unwindSpec),
        expCtx,
        pipeline_factory::MakePipelineOptions{.attachCursorSource = false});

    auto sourceContainers = pipeline->getSources();
    ASSERT_EQ(sourceContainers.size(), 1);
    auto iter = sourceContainers.cbegin();
    ASSERT(typeid(DocumentSourceLookUp) == typeid(**iter));

    auto lookup = boost::dynamic_pointer_cast<DocumentSourceLookUp>((*iter));
    ASSERT(lookup->hasUnwindSrc());

    //
    // Serialize the $lookup stage and confirm contents.
    //
    std::vector<Value> serialization;
    lookup->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 2UL);
    serialization.clear();
    lookup->serializeToArray(serialization,
                             query_shape::SerializationOptions{.serializeForCloning = true});
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);

    auto serializedDoc = serialization[0].getDocument();
    ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::object);

    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage.computeSize(), 5ULL);
    ASSERT_VALUE_EQ(serializedStage["from"], Value(std::string("coll")));
    ASSERT_VALUE_EQ(serializedStage["as"], Value(std::string("asField")));
    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][0]["$match"].getDocument(),
                       Document(fromjson("{subfield: {$eq: 1}}")));

    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::array);
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 1UL);

    ASSERT_EQ(serializedStage["pipeline"][0].getType(), BSONType::object);
    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][0]["$match"].getDocument(),
                       Document(fromjson("{subfield: {$eq: 1}}")));

    ASSERT_EQ(serializedStage["$_internalUnwind"].getType(), BSONType::object);
    ASSERT_EQ(serializedStage["let"].getType(), BSONType::object);

    //
    // Create a new $lookup stage from the serialization. Serialize the new stage and confirm that
    // it is equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBson.firstElement(), expCtx);

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization,
                                   query_shape::SerializationOptions{.serializeForCloning = true});

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceLookUpTest, ShouldBeAbleToReParseSerializedStageWithUnwindAndMatch) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupSpec = fromjson(
        "{$lookup: { from: 'coll', as: 'asField', pipeline: [{$match: {subfield: {$eq: 1}}}]}}");
    auto unwindSpec = fromjson("{$unwind: '$asField'}");
    auto matchSpec = fromjson("{$match: {'asField.subfield2': {$eq: 2}}}");

    auto pipeline = pipeline_factory::makePipeline(
        makeVector(lookupSpec, unwindSpec, matchSpec),
        expCtx,
        pipeline_factory::MakePipelineOptions{.attachCursorSource = false});

    auto sourceContainers = pipeline->getSources();
    ASSERT_EQ(sourceContainers.size(), 1);
    auto iter = sourceContainers.cbegin();
    ASSERT(typeid(DocumentSourceLookUp) == typeid(**iter));

    auto lookup = boost::dynamic_pointer_cast<DocumentSourceLookUp>((*iter));
    ASSERT(lookup->hasUnwindSrc());

    std::vector<Value> serialization;
    lookup->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 2UL);
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::object);
    serializedDoc = serialization[1].getDocument();
    ASSERT_EQ(serializedDoc["$unwind"].getType(), BSONType::object);

    serialization.clear();
    lookup->serializeToArray(serialization,
                             query_shape::SerializationOptions{.serializeForCloning = true});
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);

    serializedDoc = serialization[0].getDocument();
    ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::object);

    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage.computeSize(), 5ULL);
    ASSERT_VALUE_EQ(serializedStage["from"], Value(std::string("coll")));
    ASSERT_VALUE_EQ(serializedStage["as"], Value(std::string("asField")));

    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::array);
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 2UL);

    ASSERT_EQ(serializedStage["pipeline"][0].getType(), BSONType::object);
    ASSERT_EQ(serializedStage["pipeline"][1].getType(), BSONType::object);
    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][0]["$match"].getDocument(),
                       Document(fromjson("{subfield: {$eq: 1}}")));
    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][1]["$match"].getDocument(),
                       Document(fromjson("{subfield2: {$eq: 2}}")));

    ASSERT_EQ(serializedStage["$_internalUnwind"].getType(), BSONType::object);
    ASSERT_EQ(serializedStage["let"].getType(), BSONType::object);

    //
    // Create a new $lookup stage from the serialization. Serialize the new stage and confirm that
    // it is equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBson.firstElement(), expCtx);

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization,
                                   query_shape::SerializationOptions{.serializeForCloning = true});

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceLookUpTest, ShouldBeAbleToReParseSerializedStageWithUnwindAndTwoMath) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupSpec = fromjson(
        "{$lookup: { from: 'coll', as: 'asField', pipeline: [{$match: {subfield: {$eq: 1}}}]}}");
    auto unwindSpec = fromjson("{$unwind: '$asField'}");
    auto matchSpec1 = fromjson("{$match: {'asField.subfield2': {$eq: 2}}}");
    auto matchSpec2 = fromjson("{$match: {'asField.subfield3': {$eq: 3}}}");

    auto pipeline = pipeline_factory::makePipeline(
        makeVector(lookupSpec, unwindSpec, matchSpec1, matchSpec2),
        expCtx,
        pipeline_factory::MakePipelineOptions{.attachCursorSource = false});

    auto sourceContainers = pipeline->getSources();
    ASSERT_EQ(sourceContainers.size(), 1);
    auto iter = sourceContainers.cbegin();
    ASSERT(typeid(DocumentSourceLookUp) == typeid(**iter));

    auto lookup = boost::dynamic_pointer_cast<DocumentSourceLookUp>((*iter));
    ASSERT(lookup->hasUnwindSrc());
    //
    // Serialize the $lookup stage and confirm contents.
    //
    std::vector<Value> serialization;
    lookup->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 2UL);
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::object);
    serializedDoc = serialization[1].getDocument();
    ASSERT_EQ(serializedDoc["$unwind"].getType(), BSONType::object);

    serialization.clear();
    lookup->serializeToArray(serialization,
                             query_shape::SerializationOptions{.serializeForCloning = true});
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);

    serializedDoc = serialization[0].getDocument();
    ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::object);

    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage.computeSize(), 5ULL);
    ASSERT_VALUE_EQ(serializedStage["from"], Value(std::string("coll")));
    ASSERT_VALUE_EQ(serializedStage["as"], Value(std::string("asField")));

    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::array);
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 2UL);

    ASSERT_EQ(serializedStage["pipeline"][0].getType(), BSONType::object);
    ASSERT_EQ(serializedStage["pipeline"][1].getType(), BSONType::object);
    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][0]["$match"].getDocument(),
                       Document(fromjson("{subfield: {$eq: 1}}")));
    ASSERT_DOCUMENT_EQ(
        serializedStage["pipeline"][1]["$match"].getDocument(),
        Document(fromjson("{$and: [{subfield2: {$eq: 2}}, {subfield3: {$eq: 3}}]}")));

    ASSERT_EQ(serializedStage["$_internalUnwind"].getType(), BSONType::object);
    ASSERT_EQ(serializedStage["let"].getType(), BSONType::object);

    //
    // Create a new $lookup stage from the serialization. Serialize the new stage and confirm that
    // it is equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBson.firstElement(), expCtx);

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization,
                                   query_shape::SerializationOptions{.serializeForCloning = true});

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceLookUpTest, ShouldBeAbleToReParseSerializedStageWithFieldsAndPipeline) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << "coll"
                                      << "let" << BSON("local_x" << "$x") << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                      << "localField"
                                      << "a"
                                      << "foreignField"
                                      << "b"
                                      << "as"
                                      << "as"))
            .firstElement(),
        expCtx);

    //
    // Serialize the $lookup stage and confirm contents.
    //
    std::vector<Value> serialization;
    lookupStage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::object);

    // The fields are in no guaranteed order, so we can't perform a simple Document comparison.
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::object);

    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage.computeSize(), 6ULL);
    ASSERT_VALUE_EQ(serializedStage["from"], Value(std::string("coll")));
    ASSERT_VALUE_EQ(serializedStage["as"], Value(std::string("as")));
    ASSERT_VALUE_EQ(serializedStage["localField"], Value(std::string("a")));
    ASSERT_VALUE_EQ(serializedStage["foreignField"], Value(std::string("b")));

    ASSERT_DOCUMENT_EQ(serializedStage["let"].getDocument(),
                       Document(fromjson("{local_x: \"$x\"}")));

    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::array);
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 1UL);

    ASSERT_EQ(serializedStage["pipeline"][0].getType(), BSONType::object);
    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][0]["$match"].getDocument(),
                       Document(fromjson("{x: 1}")));

    //
    // Create a new $lookup stage from the serialization. Serialize the new stage and confirm that
    // it is equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBson.firstElement(), expCtx);

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

// Tests that $lookup with special 'from' syntax from: {db: config, coll: cache.chunks.*} can
// be round tripped.
TEST_F(DocumentSourceLookUpTest, LookupReParseSerializedStageWithFromDBAndColl) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest(
        boost::none, "config", "cache.chunks.test.foo");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto originalBSON = BSON("$lookup" << BSON("from" << BSON("db" << "config"
                                                                   << "coll"
                                                                   << "cache.chunks.test.foo")
                                                      << "localField"
                                                      << "x"
                                                      << "foreignField"
                                                      << "id"
                                                      << "as"
                                                      << "results"));
    auto lookupStage = DocumentSourceLookUp::createFromBson(originalBSON.firstElement(), expCtx);

    //
    // Serialize the $lookup stage and confirm contents.
    //
    std::vector<Value> serialization;
    static const UnorderedFieldsBSONObjComparator kComparator;
    lookupStage->serializeToArray(serialization);
    auto serializedBSON = serialization[0].getDocument().toBson();
    ASSERT_EQ(kComparator.compare(serializedBSON, originalBSON), 0);

    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBSON.firstElement(), expCtx);

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

// Tests that $lookup with 'let' and special 'from' syntax from: {db: config, coll: cache.chunks.*}
// can be round tripped.
TEST_F(DocumentSourceLookUpTest, LookupWithLetReParseSerializedStageWithFromDBAndColl) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest(
        boost::none, "config", "cache.chunks.test.foo");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto originalBSON =
        BSON("$lookup" << BSON("from" << BSON("db" << "config"
                                                   << "coll"
                                                   << "cache.chunks.test.foo")
                                      << "let" << BSON("local_x" << "$x") << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                                      << "as"));
    auto lookupStage = DocumentSourceLookUp::createFromBson(originalBSON.firstElement(), expCtx);

    //
    // Serialize the $lookup stage and confirm contents.
    //
    std::vector<Value> serialization;
    static const UnorderedFieldsBSONObjComparator kComparator;
    lookupStage->serializeToArray(serialization);
    auto serializedBSON = serialization[0].getDocument().toBson();
    ASSERT_EQ(kComparator.compare(serializedBSON, originalBSON), 0);

    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBSON.firstElement(), expCtx);

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

// Tests that $lookup with '$documents' can be round tripped.
TEST_F(DocumentSourceLookUpTest, LookupReParseSerializedStageWithDocumentsPipelineStage) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "$cmd.aggregate");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto originalBSON =
        BSON("$lookup" << BSON("localField"
                               << "y"
                               << "foreignField"
                               << "x"
                               << "pipeline"
                               << BSON_ARRAY(BSON("$documents"
                                                  << BSON_ARRAY(BSON("x" << 5) << BSON("y" << 15))))
                               << "as"
                               << "as"));
    auto lookupStage = DocumentSourceLookUp::createFromBson(originalBSON.firstElement(), expCtx);

    //
    // Serialize the $lookup stage and confirm contents.
    //
    for (auto& opts :
         {query_shape::SerializationOptions{
              query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue},
          query_shape::SerializationOptions{}}) {
        std::vector<Value> serialization;
        lookupStage->serializeToArray(serialization, opts);
        ASSERT_EQ(serialization.size(), 1UL);
        auto serializedDoc = serialization[0].getDocument();
        ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::object);

        // Ensure the $documents desugared to $queue properly.
        auto serializedStage = serializedDoc["$lookup"].getDocument();
        ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::array);
        ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 4UL);

        ASSERT_EQ(serializedStage["pipeline"][0].getType(), BSONType::object);
        ASSERT_EQ(serializedStage["pipeline"][0]["$queue"].getType(), BSONType::array);

        auto roundTripped =
            DocumentSourceLookUp::createFromBson(serializedDoc.toBson().firstElement(), expCtx);

        std::vector<Value> newSerialization;
        roundTripped->serializeToArray(newSerialization, opts);

        ASSERT_EQ(newSerialization.size(), 1UL);
        ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
    }
}

// Tests that $lookup with '$search' can be round tripped.
TEST_F(DocumentSourceLookUpTest, LookupReParseSerializedStageWithSearchPipelineStage) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto originalBSON = BSON(
        "$lookup" << BSON("from" << "coll"
                                 << "pipeline"
                                 << BSON_ARRAY(BSON("$search" << BSON("term" << "asdf"))) << "as"
                                 << "as"));
    auto lookupStage = DocumentSourceLookUp::createFromBson(originalBSON.firstElement(), expCtx);

    //
    // Serialize the $lookup stage and confirm contents.
    //
    std::vector<Value> serialization;
    lookupStage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::object);

    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::array);
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 1UL);

    // If the serialized pipeline doesn't have $search as the first stage, extractSourceStage()
    // needs to be updated to include the desugared $search for sharded queries to work properly.
    ASSERT_EQ(serializedStage["pipeline"][0].getType(), BSONType::object);
    ASSERT_EQ(serializedStage["pipeline"][0]["$search"].getType(), BSONType::object);
    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][0]["$search"]["mongotQuery"].getDocument(),
                       Document(fromjson("{term: 'asdf'}")));

    auto roundTripped =
        DocumentSourceLookUp::createFromBson(serializedDoc.toBson().firstElement(), expCtx);

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

// Tests for the mongot_lookup_prefix helpers, which determine where the localField/foreignField
// equality $match must be placed within a mongot $lookup subpipeline.

// These exercise free functions over BSONObj and need no fixture, so use a plain TEST suite.
TEST(MongotLookupPrefixTest, IsSourceStage) {
    using namespace search_helper_bson_obj::mongot_lookup_prefix;

    // User-facing mongot source stages.
    ASSERT_TRUE(isSourceStage(BSON("$search" << BSON("term" << "x"))));
    ASSERT_TRUE(isSourceStage(BSON("$searchMeta" << BSON("term" << "x"))));
    ASSERT_TRUE(isSourceStage(BSON("$vectorSearch" << BSON("queryVector" << BSON_ARRAY(1 << 2)))));

    // Legacy desugared mongot source stage.
    ASSERT_TRUE(isSourceStage(BSON(DocumentSourceInternalSearchMongotRemote::kStageName
                                   << BSON("mongotQuery" << BSON("term" << "asdf")))));

    // Extension desugared mongot source stages.
    ASSERT_TRUE(isSourceStage(BSON(DocumentSourceInternalDocumentResultsAndMetadata::kStageName
                                   << BSON("source" << "$_extensionSearch"))));
    ASSERT_TRUE(
        isSourceStage(BSON(search_helpers::kExtensionSearchStageName << BSON("term" << "asdf"))));
    ASSERT_TRUE(isSourceStage(
        BSON(search_helpers::kExtensionSearchMetaStageName << BSON("term" << "asdf"))));
    ASSERT_TRUE(isSourceStage(BSON(search_helpers::kExtensionVectorSearchStageName
                                   << BSON("queryVector" << BSON_ARRAY(1 << 2)))));

    // Non-source stages must not match, including a non-storedSource $replaceRoot.
    ASSERT_FALSE(isSourceStage(BSON("$match" << BSON("x" << 1))));
    ASSERT_FALSE(isSourceStage(BSON("$project" << BSON("x" << 1))));
    ASSERT_FALSE(
        isSourceStage(BSON(DocumentSourceInternalSearchIdLookUp::kStageName << BSONObj())));
    ASSERT_FALSE(isSourceStage(fromjson("{$replaceRoot: {newRoot: '$x'}}")));
}

TEST(MongotLookupPrefixTest, IsSupportStage) {
    using namespace search_helper_bson_obj::mongot_lookup_prefix;

    // idLookup.
    ASSERT_TRUE(isSupportStage(
        BSON(DocumentSourceInternalSearchIdLookUp::kStageName << BSON("limit" << 100))));

    // The storedSource $replaceRoot in both the legacy and extension desugared shapes.
    ASSERT_TRUE(isSupportStage(
        fromjson("{$replaceRoot: {newRoot: {$ifNull: ['$storedSource', '$$ROOT']}}}")));
    ASSERT_TRUE(isSupportStage(fromjson("{$replaceRoot: {newRoot: '$storedSource'}}")));

    // Non-support stages must not match.
    ASSERT_FALSE(isSupportStage(BSON("$match" << BSON("x" << 1))));
    ASSERT_FALSE(isSupportStage(BSON("$search" << BSON("term" << "x"))));
    // A generic $replaceRoot must not match.
    ASSERT_FALSE(isSupportStage(fromjson("{$replaceRoot: {newRoot: '$x'}}")));
    // Each part of the legacy $ifNull shape must match: a wrong first arg, a wrong second arg, and
    // an $ifNull with the wrong arg count are all rejected.
    ASSERT_FALSE(
        isSupportStage(fromjson("{$replaceRoot: {newRoot: {$ifNull: ['$other', '$$ROOT']}}}")));
    ASSERT_FALSE(isSupportStage(
        fromjson("{$replaceRoot: {newRoot: {$ifNull: ['$storedSource', '$$NOW']}}}")));
    ASSERT_FALSE(isSupportStage(
        fromjson("{$replaceRoot: {newRoot: {$ifNull: ['$storedSource', '$$ROOT', 1]}}}")));
}

TEST(MongotLookupPrefixTest, ExtractPrefix) {
    using namespace search_helper_bson_obj::mongot_lookup_prefix;

    // Empty and non-mongot pipelines have no prefix. prefixEndIdx agrees.
    ASSERT_TRUE(extractPrefix({}).empty());
    ASSERT_EQ(prefixEndIdx({}), 0U);
    ASSERT_TRUE(extractPrefix({BSON("$match" << BSON("x" << 1))}).empty());
    ASSERT_EQ(prefixEndIdx({BSON("$match" << BSON("x" << 1))}), 0U);
    // idLookup without a leading source stage is not a valid prefix start.
    ASSERT_TRUE(extractPrefix({BSON(DocumentSourceInternalSearchIdLookUp::kStageName << BSONObj())})
                    .empty());

    // A lone $search is a one-stage prefix; trailing non-support stages are excluded.
    const auto search = BSON("$search" << BSON("term" << "x"));
    {
        auto prefix = extractPrefix({search});
        ASSERT_EQ(prefix.size(), 1U);
        ASSERT_BSONOBJ_EQ(prefix[0], search);
    }
    {
        auto prefix = extractPrefix({search, BSON("$match" << BSON("x" << 1))});
        ASSERT_EQ(prefix.size(), 1U);
        ASSERT_BSONOBJ_EQ(prefix[0], search);
    }

    // Bug 1: desugared source + idLookup. Old extractSourceStage returned {} here, pushing the
    // $match before the source stage. The full prefix is both stages, in order.
    {
        const auto mongotRemote = BSON(DocumentSourceInternalSearchMongotRemote::kStageName
                                       << BSON("mongotQuery" << BSON("term" << "asdf")));
        const auto idLookup = BSON(DocumentSourceInternalSearchIdLookUp::kStageName << BSONObj());
        auto prefix = extractPrefix({mongotRemote, idLookup});
        ASSERT_EQ(prefix.size(), 2U);
        ASSERT_BSONOBJ_EQ(prefix[0], mongotRemote);
        ASSERT_BSONOBJ_EQ(prefix[1], idLookup);
    }

    // Bug 1 (extension): the extension-desugared DRM + idLookup (the non-storedSource extension
    // $search/$vectorSearch shape). Both stages form the prefix.
    {
        const auto drm = BSON(DocumentSourceInternalDocumentResultsAndMetadata::kStageName << BSON(
                                  "source" << BSON("$_extensionSearch" << BSON("term" << "asdf"))));
        const auto idLookup = BSON(DocumentSourceInternalSearchIdLookUp::kStageName << BSONObj());
        auto prefix = extractPrefix({drm, idLookup});
        ASSERT_EQ(prefix.size(), 2U);
        ASSERT_BSONOBJ_EQ(prefix[0], drm);
        ASSERT_BSONOBJ_EQ(prefix[1], idLookup);
    }

    // Bug 2 (legacy): storedSource source + $ifNull $replaceRoot. The $match must go after the
    // $replaceRoot so the join runs on the promoted (un-nested) fields. The prefix is the whole
    // pipeline.
    {
        const auto mongotRemote = BSON(DocumentSourceInternalSearchMongotRemote::kStageName
                                       << BSON("mongotQuery" << BSON("term" << "asdf")));
        const auto replaceRoot =
            fromjson("{$replaceRoot: {newRoot: {$ifNull: ['$storedSource', '$$ROOT']}}}");
        auto prefix = extractPrefix({mongotRemote, replaceRoot});
        ASSERT_EQ(prefix.size(), 2U);
        ASSERT_BSONOBJ_EQ(prefix[0], mongotRemote);
        ASSERT_BSONOBJ_EQ(prefix[1], replaceRoot);
    }

    // Bug 2 (extension): the extension-desugared DRM + {newRoot: "$storedSource"} $replaceRoot,
    // followed by user stages. The prefix must span both mongot stages so the join $match lands
    // after storedSource promotion (this is the exact shape that produced empty results on the
    // shard for lookup_match.js). Trailing user stages are excluded.
    {
        const auto drm =
            BSON(DocumentSourceInternalDocumentResultsAndMetadata::kStageName << BSON(
                     "source" << BSON("$_extensionSearch" << BSON("returnStoredSource" << true))));
        const auto replaceRoot = fromjson("{$replaceRoot: {newRoot: '$storedSource'}}");
        std::vector<BSONObj> storedSourceExtension = {drm,
                                                      replaceRoot,
                                                      fromjson("{$project: {_id: false}}"),
                                                      fromjson("{$set: {x: {$add: [1, '$x']}}}")};
        auto prefix = extractPrefix(storedSourceExtension);
        ASSERT_EQ(prefix.size(), 2U);
        ASSERT_EQ(prefixEndIdx(storedSourceExtension), 2U);
        ASSERT_BSONOBJ_EQ(prefix[0], drm);
        ASSERT_BSONOBJ_EQ(prefix[1], replaceRoot);
    }
}

// $lookup : {from : {db: <>, coll: <>}} syntax doesn't work for a namespace that isn't
// config.cache.chunks.*, config.collections, config.chunks, or local.oplog.rs.
TEST_F(DocumentSourceLookUpTest, RejectsPipelineFromDBAndCollWithBadDBAndColl) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto stageSpec =
        fromjson("{$lookup: {from: {db: 'test', coll: 'coll'}, as: 'as', pipeline: []}}");

    ASSERT_THROWS_CODE(LiteParsedLookUp::parse(expCtx->getNamespaceString(),
                                               stageSpec.firstElement(),
                                               LiteParserOptions{}),
                       AssertionException,
                       ErrorCodes::FailedToParse);

    ASSERT_THROWS_CODE(DocumentSourceLookUp::createFromBson(stageSpec.firstElement(), expCtx),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

// $lookup : {from : {db: <>, coll: <>}} syntax fails when "db" is "config" but "coll" is
// not "cache.chunks.*", "collections", or "chunks".
TEST_F(DocumentSourceLookUpTest, RejectsPipelineFromDBAndCollWithBadColl) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "config", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto stageSpec =
        fromjson("{$lookup: {from: {db: 'config', coll: 'coll'}, as: 'as', pipeline: []}}");

    ASSERT_THROWS_CODE(LiteParsedLookUp::parse(expCtx->getNamespaceString(),
                                               stageSpec.firstElement(),
                                               LiteParserOptions{}),
                       AssertionException,
                       ErrorCodes::FailedToParse);

    ASSERT_THROWS_CODE(DocumentSourceLookUp::createFromBson(stageSpec.firstElement(), expCtx),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

// $lookup : {from : {db: <>, coll: <>}} syntax doesn't work for a namespace when "coll" is
// "cache.chunks.*" but "db" is not "config".
TEST_F(DocumentSourceLookUpTest, RejectsPipelineFromDBAndCollWithBadDB) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest(
        boost::none, "test", "cache.chunks.test.foo");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto stageSpec = fromjson(
        "{$lookup: {from: {db: 'test', coll: 'cache.chunks.test.foo'}, "
        "as: 'as', pipeline: []}}");

    ASSERT_THROWS_CODE(LiteParsedLookUp::parse(expCtx->getNamespaceString(),
                                               stageSpec.firstElement(),
                                               LiteParserOptions{}),
                       AssertionException,
                       ErrorCodes::FailedToParse);

    ASSERT_THROWS_CODE(DocumentSourceLookUp::createFromBson(stageSpec.firstElement(), expCtx),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceLookUpTest, ExplainSerializesSubpipeline) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << "coll"
                                      << "let" << BSON("local_x" << "$x") << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                                      << "as"))
            .firstElement(),
        expCtx);

    std::vector<Value> serialization;
    lookupStage->serializeToArray(serialization, kExplain);
    ASSERT_EQ(serialization.size(), 1UL);

    auto serializedDoc = serialization[0].getDocument();
    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::array);
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 1UL);

    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][0]["$match"].getDocument(),
                       Document(fromjson("{x: {$eq: 1}}")));
}

TEST_F(DocumentSourceLookUpTest, ExplainSerializesSubpipelineIncludingViewStages) {
    auto expCtx = getExpCtx();
    NamespaceString viewNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "myView");
    auto viewPipeline = BSON_ARRAY(BSON("$addFields" << BSON("viewField" << 1)));

    std::vector<BSONObj> viewPipelineVec;
    for (const auto& stage : viewPipeline) {
        viewPipelineVec.push_back(stage.Obj().getOwned());
    }

    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{viewNs, {viewNs, viewPipelineVec, boost::none, true}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << "myView"
                                      << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                      << "as"
                                      << "results"))
            .firstElement(),
        expCtx);

    std::vector<Value> serialization;
    lookupStage->serializeToArray(serialization, kExplain);
    ASSERT_EQ(serialization.size(), 1UL);

    auto serializedDoc = serialization[0].getDocument();
    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::array);

    // The resolved pipeline should include both the view pipeline ($addFields) and the user
    // pipeline ($match), i.e. 2 stages instead of just the 1 stage from the user pipeline.
    // After optimization, the $match may be reordered before the $addFields since the $match
    // predicate does not depend on the field added by the view pipeline.
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 2UL);
    ASSERT_FALSE(serializedStage["pipeline"][0].getDocument().getField("$match").missing());
    ASSERT_FALSE(serializedStage["pipeline"][1].getDocument().getField("$addFields").missing());
}

TEST_F(DocumentSourceLookUpTest, RejectsUserSuppliedIsHybridSearchWhenExtensionsFlagOn) {
    // When featureFlagExtensionsInsideHybridSearch is on, the stage-params dispatch path is taken
    // instead of createFromBson, and must equally reject a user-supplied $_internalIsHybridSearch.
    auto ifrCtx = IncrementalFeatureRolloutContext::forTest(std::vector<BSONObj>{
        BSON("name" << "featureFlagExtensionsInsideHybridSearch" << "value" << true)});

    // A client with a transport session and no internal tag is an external (user) client.
    auto client = getServiceContext()->getService()->makeClient(
        "external", transport::MockSession::create(/*transportLayer=*/nullptr));
    auto opCtx = client->makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx.get())
                      .ns(getExpCtx()->getNamespaceString())
                      .ifrContext(ifrCtx)
                      .build();

    const std::vector<BSONObj> rawPipeline = {
        BSON("$lookup" << BSON("from" << "coll" << "pipeline" << BSONArray() << "as" << "out"
                                      << "$_internalIsHybridSearch" << true))};
    LiteParsedPipeline liteParsedPipeline(
        expCtx->getNamespaceString(), rawPipeline, false, LiteParserOptions{.ifrContext = ifrCtx});
    ASSERT_THROWS_CODE(
        Pipeline::parseFromLiteParsed(liteParsedPipeline, expCtx), AssertionException, 5491300);
}

TEST_F(DocumentSourceLookUpTest,
       BansLocalForeignFieldSyntaxForHybridSearchLookupWhenExtensionsFlagOff) {
    // With the featureFlagExtensionsInsideHybridSearch flag off, $lookup with a hybrid search
    // subpipeline rejects localField/foreignField syntax.
    auto ifrCtx = IncrementalFeatureRolloutContext::forTest(std::vector<BSONObj>{
        BSON("name" << "featureFlagExtensionsInsideHybridSearch" << "value" << false)});
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(getOpCtx())
                      .ns(getExpCtx()->getNamespaceString())
                      .ifrContext(ifrCtx)
                      .build();
    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from" << "coll" << "localField" << "x" << "foreignField" << "x"
                                          << "pipeline" << BSONArray() << "as" << "out"
                                          << "$_internalIsHybridSearch" << true))
                .firstElement(),
            expCtx),
        AssertionException,
        12982600);
}

TEST_F(DocumentSourceLookUpTest,
       AllowsLocalForeignFieldSyntaxForHybridSearchLookupWhenExtensionsFlagOn) {
    // With the featureFlagExtensionsInsideHybridSearch flag on, the localField/foreignField
    // restriction is lifted for $lookup with a hybrid search subpipeline.
    auto ifrCtx = IncrementalFeatureRolloutContext::forTest(std::vector<BSONObj>{
        BSON("name" << "featureFlagExtensionsInsideHybridSearch" << "value" << true)});
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(getOpCtx())
                      .ns(getExpCtx()->getNamespaceString())
                      .ifrContext(ifrCtx)
                      .build();
    expCtx->setMongoProcessInterface(std::make_shared<DocumentSourceLookupMockMongoInterface>(
        std::deque<DocumentSource::GetNextResult>{}));

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << "coll" << "localField" << "x" << "foreignField" << "x"
                                      << "pipeline" << BSONArray() << "as" << "out"
                                      << "$_internalIsHybridSearch" << true))
            .firstElement(),
        expCtx);
    ASSERT(lookupStage);
}

TEST_F(DocumentSourceLookUpTest,
       CreateFromStageParamsRoutesThroughLppConstructorForLocalForeignFieldView) {
    // Exercises the createFromStageParams path for $lookup:{from:view, localField, foreignField}
    // with a pre-resolved LPP (shouldParseLpp = true). This mirrors the $unionWith fix: instead of
    // falling back to createFromBson, the drain loop's pre-resolved LPP is used directly.
    auto mainNss = NamespaceString::createNamespaceString_forTest("test", "main");
    auto viewNss = NamespaceString::createNamespaceString_forTest("test", "myView");
    auto backingNss = NamespaceString::createNamespaceString_forTest("test", "backing");

    // Enable featureFlagExtensionsInsideHybridSearch so the stage-params dispatch path is used
    // instead of the BSON-only fallback. The view pipeline is already stitched into the StageParams
    // during lite-parsing, so re-applying the view definition is incorrect.
    auto ifrCtx = IncrementalFeatureRolloutContext::forTest(std::vector<BSONObj>{
        BSON("name" << "featureFlagExtensionsInsideHybridSearch" << "value" << true)});
    auto expCtx =
        ExpressionContextBuilder{}.opCtx(getOpCtx()).ns(mainNss).ifrContext(ifrCtx).build();
    expCtx->setMongoProcessInterface(std::make_shared<DocumentSourceLookupMockMongoInterface>(
        std::deque<DocumentSource::GetNextResult>{}));

    // Build a view entry with shouldParseLpp = true to simulate what the drain loop produces.
    BSONObj viewStage = BSON("$addFields" << BSON("viewField" << 1));
    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    opts.shouldParseLpp = true;
    ResolvedNamespaceMap nsMap;
    nsMap.emplace(
        viewNss,
        ResolvedNamespace(viewNss, backingNss, std::vector<BSONObj>{viewStage}, BSONObj{}, opts));
    expCtx->setResolvedNamespaces(std::move(nsMap));

    // Construct stage params for $lookup:{from:viewNss, localField:"id", foreignField:"id", as:"r"}
    // with liteParsedPipeline absent (the localField/foreignField form produces no subpipeline
    // LPP).
    auto lookupBson =
        BSON("$lookup" << BSON("from" << viewNss.coll() << "localField" << "id" << "foreignField"
                                      << "id" << "as" << "r"));
    LookUpStageParams params(viewNss,
                             "r",
                             {},
                             BSONObj{},
                             std::string{"id"},
                             std::string{"id"},
                             boost::none,
                             false,
                             false,
                             lookupBson,
                             boost::none);

    auto sources = DocumentSourceLookUp::createFromStageParams(params, expCtx);
    ASSERT_EQ(sources.size(), 1U);

    auto* lookup = dynamic_cast<DocumentSourceLookUp*>(sources.front().get());
    ASSERT_TRUE(lookup != nullptr);

    // The introspection pipeline must contain exactly the one view stage ($addFields).
    const auto* subPipeline = lookup->getSubPipeline();
    ASSERT_TRUE(subPipeline != nullptr);
    ASSERT_EQ(subPipeline->size(), 1U);
    ASSERT_EQ(subPipeline->front()->getSourceName(), "$addFields"sv);

    // The BSON resolved pipeline must also carry the view stage (set by the base constructor).
    ASSERT_EQ(lookup->getResolvedPipelineForTest().size(), 2U);  // view stage + $match placeholder
    ASSERT_FALSE(lookup->getResolvedPipelineForTest()[0].getField("$addFields").eoo());
}

TEST_F(DocumentSourceLookUpTest,
       ExplainSerializesSubpipelineWithoutFieldMatchPlaceholderForFieldsAndPipeline) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << "coll"
                                      << "let" << BSON("local_x" << "$x") << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("a" << 1))
                                                    << BSON("$match" << BSON("b" << 2))
                                                    << BSON("$match" << BSON("c" << 3)))
                                      << "localField"
                                      << "x"
                                      << "foreignField"
                                      << "y"
                                      << "as"
                                      << "as"))
            .firstElement(),
        expCtx);

    std::vector<Value> serialization;
    lookupStage->serializeToArray(serialization, kExplain);
    ASSERT_EQ(serialization.size(), 1UL);

    auto serializedDoc = serialization[0].getDocument();
    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::array);

    // The placeholder $match for localField/foreignField should be stripped from explain output.
    // The 3 user-specified $match stages are merged into a single $match by optimization.
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 1UL);
    ASSERT_FALSE(serializedStage["pipeline"][0].getDocument().getField("$match").missing());
}

// $lookup : {from: {db: <>, coll: <>}} syntax is allowed when parseCtx.allowGenericForeignDbLookup
// or expCtx.allowGenericForeignDbLookup is true.
TEST_F(DocumentSourceLookUpTest, AllowsPipelineFromDBAndCollWithContextFlag) {
    auto expCtx = getExpCtx();

    auto stageSpec =
        fromjson("{$lookup: {from: {db: 'test2', coll: 'target_coll'}, as: 'as', pipeline: []}}");

    auto fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test2", "target_coll");

    {
        // Lite parsing
        LiteParserOptions options{.allowGenericForeignDbLookup = true};
        auto liteParsedLookup = LiteParsedLookUp::parse(
            expCtx->getNamespaceString(), stageSpec.firstElement(), options);
        auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();

        ASSERT_EQ(1ul, namespaceSet.count(fromNs));
    }

    {
        // "Heavy" parsing
        expCtx->setAllowGenericForeignDbLookup_forTest(true);
        expCtx->setResolvedNamespaces(
            ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
        auto docSource = DocumentSourceLookUp::createFromBson(stageSpec.firstElement(), expCtx);
        auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
        ASSERT(lookupStage);

        ASSERT_EQ(lookupStage->getFromNs(), fromNs);
    }
}

// Tests that $lookup distributedPlanLogic() is boost::none, allowing for the stage to run on each
// shard, when it reads from config.cache.chunks.* namespaces using from: {db: <> , coll: <> }
// syntax.
TEST_F(DocumentSourceLookUpTest, FromDBAndCollDistributedPlanLogic) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest(
        boost::none, "config", "cache.chunks.test.foo");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << BSON("db" << "config"
                                                   << "coll"
                                                   << "cache.chunks.test.foo")
                                      << "let" << BSON("local_x" << "$x") << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                                      << "as"))
            .firstElement(),
        expCtx);

    ASSERT(!lookupStage->distributedPlanLogic(nullptr));
}

// Tests $lookup distributedPlanLogic() is prohibited from executing on the shardsStage for standard
// $lookup with from: <string> syntax.
TEST_F(DocumentSourceLookUpTest, LookupDistributedPlanLogic) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << "coll"
                                      << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                      << "as"
                                      << "as"))
            .firstElement(),
        expCtx);
    ASSERT(lookupStage->distributedPlanLogic(nullptr));
    ASSERT(lookupStage->distributedPlanLogic(nullptr)->shardsStage == nullptr);
    ASSERT_EQ(lookupStage->distributedPlanLogic(nullptr)->mergingStages.size(), 1);
}

TEST(MakeMatchStageFromInput, NonArrayValueUsesEqQuery) {
    auto input = Document{{"local", 1}};
    BSONObj matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
        input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_BSONOBJ_EQ(matchStage, fromjson("{$match: {foreign: {$eq: 1}}}"));
}

TEST(MakeMatchStageFromInput, RegexValueUsesEqQuery) {
    BSONRegEx regex("^a");
    Document input = DOC("local" << Value(regex));
    BSONObj matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
        input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_BSONOBJ_EQ(matchStage, BSON("$match" << BSON("foreign" << BSON("$eq" << regex))));
}

TEST(MakeMatchStageFromInput, ArrayValueUsesInQuery) {
    std::vector<Value> inputArray = {Value(1), Value(2)};
    Document input = DOC("local" << Value(inputArray));
    BSONObj matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
        input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_BSONOBJ_EQ(matchStage, fromjson("{$match: {foreign: {$in: [1, 2]}}}"));
}

TEST(MakeMatchStageFromInput, ArrayValueWithRegexUsesOrQuery) {
    BSONRegEx regex("^a");
    std::vector<Value> inputArray = {Value(1), Value(regex), Value(2)};
    Document input = DOC("local" << Value(inputArray));
    BSONObj matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
        input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_BSONOBJ_EQ(
        matchStage,
        BSON("$match" << BSON("$or" << BSON_ARRAY(BSON("foreign" << BSON("$eq" << Value(1)))
                                                  << BSON("foreign" << BSON("$eq" << regex))
                                                  << BSON("foreign" << BSON("$eq" << Value(2)))))));
}

TEST_F(DocumentSourceLookUpTest, LookupReportsAsFieldIsModified) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // Set up the $lookup stage.
    auto lookupSpec = Document{{"$lookup",
                                Document{{"from", fromNs.coll()},
                                         {"localField", "foreignId"sv},
                                         {"foreignField", "_id"sv},
                                         {"as", "foreignDocs"sv}}}}
                          .toBson();
    auto lookup = makeLookUpFromBson(lookupSpec.firstElement(), expCtx);

    auto modifiedPaths = lookup->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(1U, modifiedPaths.paths.size());
    ASSERT_EQ(1U, modifiedPaths.paths.count("foreignDocs"));
}

TEST_F(DocumentSourceLookUpTest, LookupReportsFieldsModifiedByAbsorbedUnwind) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // Set up the $lookup stage.
    auto lookupSpec = Document{{"$lookup",
                                Document{{"from", fromNs.coll()},
                                         {"localField", "foreignId"sv},
                                         {"foreignField", "_id"sv},
                                         {"as", "foreignDoc"sv}}}}
                          .toBson();
    auto lookup = makeLookUpFromBson(lookupSpec.firstElement(), expCtx);

    const bool preserveNullAndEmptyArrays = false;
    const boost::optional<std::string> includeArrayIndex = std::string("arrIndex");
    lookup->setUnwindStage_forTest(DocumentSourceUnwind::create(
        expCtx, "foreignDoc", preserveNullAndEmptyArrays, includeArrayIndex));

    auto modifiedPaths = lookup->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(2U, modifiedPaths.paths.size());
    ASSERT_EQ(1U, modifiedPaths.paths.count("foreignDoc"));
    ASSERT_EQ(1U, modifiedPaths.paths.count("arrIndex"));
}

TEST_F(DocumentSourceLookUpTest, ShouldCacheNonCorrelatedSubPipelinePrefix) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x:1}}, {$sort: {x: 1}}, "
        "{$addFields: {varField: '$$var1'}}], from: 'coll', as: 'as'}}",
        expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);

    auto subpipelineExpCtx = lookupDS->getSubpipelineExpCtx();
    auto subPipeline = lookupStage->buildPipeline(subpipelineExpCtx, DOC("_id" << 5));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{$mock: {}}, {$match: {x:{$eq: 1}}}, {$sort: {sortKey: {x: 1}}}, "
                      << sequentialCacheStageObj(getExpCtx()->getOperationContext())
                      << ", {$addFields: {varField: {$const: 5}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest,
       ShouldDiscoverVariablesReferencedInFacetPipelineAfterAnExhaustiveAllStage) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // In the $facet stage here, the correlated $match stage comes after a $group stage which
    // returns EXHAUSTIVE_ALL for its dependencies. Verify that we continue enumerating the $facet
    // pipeline's variable dependencies after this point, so that the $facet stage is correctly
    // identified as correlated and the cache is placed befor
    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x:1}}, {$sort: {x: 1}}, "
        "{$facet: {facetPipe: [{$group: {_id: '$_id'}}, {$match: {$expr: {$eq: ['$_id', "
        "'$$var1']}}}]}}], from: 'coll', as: 'as'}}",
        expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);

    auto subpipelineExpCtx = lookupDS->getSubpipelineExpCtx();
    auto subPipeline = lookupStage->buildPipeline(subpipelineExpCtx, DOC("_id" << 5));
    ASSERT(subPipeline);

    // Note that the second $match stage should be moved up to before the $group stage, since $group
    // should swap with $match when filtering on $_id.
    auto expectedPipe =
        fromjson(str::stream() << "[{$mock: {}},"
                                  " {$match: {x:{$eq: 1}}},"
                                  " {$sort: {sortKey: {x: 1}}},"
                               << sequentialCacheStageObj(getExpCtx()->getOperationContext())
                               << ",{$facet: {facetPipe: ["
                                  "   {$internalFacetTeeConsumer: {}},"
                                  "   {$match: {$and: [{_id: {$_internalExprEq: 5}},"
                                  "                    {$expr: {$eq: ['$_id', {$const: 5}]}}]}},"
                                  "   {$group: {_id: '$_id', $willBeMerged: false}}]}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest, ExprEmbeddedInMatchExpressionShouldBeOptimized) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // This pipeline includes a $match stage that itself includes a $expr expression.
    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {$expr: {$eq: "
        "['$_id','$$var1']}}}], from: 'coll', as: 'as'}}",
        expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);

    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 5));
    ASSERT(subPipeline);

    const auto& sources = subPipeline->getSources();
    ASSERT_GTE(sources.size(), 2u);

    // The first source is our mock data source, and the second should be the $match expression.
    auto secondSource = *(++sources.cbegin());
    auto& matchSource = dynamic_cast<const DocumentSourceMatch&>(*secondSource);

    // Ensure that the '$$var' in the embedded expression got optimized to ExpressionConstant.
    auto expectedMatch =
        fromjson("{$and: [{_id: {$_internalExprEq: 5}}, {$expr: {$eq: ['$_id', {$const: 5}]}}]}");

    ASSERT_VALUE_EQ(Value(matchSource.getMatchExpression()->serialize()), Value(expectedMatch));
}

TEST_F(DocumentSourceLookUpTest,
       ShouldIgnoreLocalVariablesShadowingLetVariablesWhenFindingNonCorrelatedPrefix) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // The $project stage defines a local variable with the same name as the $lookup 'let'
    // variable. Verify that the $project is identified as non-correlated and the cache is
    // placed after it.
    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x: 1}}, {$sort: {x: 1}}, "
        "{$project: {projectedField: {$let: {vars: {var1: '$x'}, in: "
        "'$$var1'}}, _id: false}}, {$addFields: {varField: {$sum: ['$x', '$$var1']}}}], "
        "from: 'coll', "
        "as: 'as'}}",
        expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);

    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 5));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{$mock: {}}, {$match: {x: {$eq: 1}}}, {$sort: {sortKey: {x: 1}}}, "
                         "{$project: {projectedField: {$let: {vars: {var1: '$x'}, "
                         "in: '$$var1'}}, _id: false}},"
                      << sequentialCacheStageObj(getExpCtx()->getOperationContext())
                      << ", {$addFields: {varField: {$sum: ['$x', {$const: 5}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest, ShouldInsertCacheBeforeCorrelatedNestedLookup) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // Create a $lookup stage whose pipeline contains nested $lookups. The third-level $lookup
    // refers to a 'let' variable defined in the top-level $lookup. Verify that the second-level
    // $lookup is correctly identified as a correlated stage and the cache is placed before it.
    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {from: 'coll', as: 'as', let: {var1: '$_id'}, pipeline: [{$match: "
        "{x:1}}, {$sort: {x: 1}}, {$lookup: {from: 'coll', as: 'subas', pipeline: "
        "[{$match: {x: 1}}, {$lookup: {from: 'coll', as: 'subsubas', pipeline: [{$match: "
        "{$expr: {$eq: ['$y', '$$var1']}}}]}}]}}, {$addFields: {varField: '$$var1'}}]}}",
        expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);

    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 5));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{$mock: {}}, {$match: {x:{$eq: 1}}}, {$sort: {sortKey: {x: 1}}}, "
                      << sequentialCacheStageObj(getExpCtx()->getOperationContext())
                      << ", {$lookup: {from: 'coll', as: 'subas', let: {}, pipeline: "
                         "[{$match: {x: {$eq: 1}}}, {$lookup: {from: 'coll', as: 'subsubas', "
                         "let: {}, pipeline: [{$match: {$and: [{y: {$_internalExprEq: 5}}, "
                         "{$expr: {$eq: ['$y', {$const: 5}]}}]}}]}}]}}, "
                         "{$addFields: {varField: {$const: 5}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest,
       ShouldIgnoreNestedLookupLetVariablesShadowingOuterLookupLetVariablesWhenFindingPrefix) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // The nested $lookup stage defines a 'let' variable with the same name as the top-level 'let'.
    // Verify the nested $lookup is identified as non-correlated and the cache is placed after it.
    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x:1}}, {$sort: {x: 1}}, "
        "{$lookup: {let: {var1: '$y'}, pipeline: [{$match: {$expr: { $eq: ['$z', "
        "'$$var1']}}}], from: 'coll', as: 'subas'}}, {$addFields: {varField: '$$var1'}}], "
        "from: 'coll', as: 'as'}}",
        expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);

    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 5));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{$mock: {}}, {$match: {x:{$eq: 1}}}, {$sort: {sortKey: {x: 1}}}, "
                         "{$lookup: {from: 'coll', as: 'subas', let: {var1: '$y'}, "
                         "pipeline: [{$match: {$expr: { $eq: ['$z', '$$var1']}}}]}}, "
                      << sequentialCacheStageObj(getExpCtx()->getOperationContext())
                      << ", {$addFields: {varField: {$const: 5} }}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest, ShouldCacheEntirePipelineIfNonCorrelated) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {}, pipeline: [{$match: {x:1}}, {$sort: {x: 1}}, {$lookup: "
        "{pipeline: [{$match: {y: 5}}], from: 'coll', as: 'subas'}}, {$addFields: "
        "{constField: 5}}], from: 'coll', as: 'as'}}",
        expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);

    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 5));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream()
        << "[{$mock: {}}, {$match: {x:{$eq: 1}}}, {$sort: {sortKey: {x: 1}}}, {$lookup: {from: "
           "'coll', as: 'subas', let: {}, pipeline: [{$match: {y: {$eq: 5}}}]}}, {$addFields: "
           "{constField: {$const: 5}}}, "
        << sequentialCacheStageObj(getExpCtx()->getOperationContext()) << "]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest, ShouldNotCacheIfCorrelatedStageIsAbsorbedIntoPlanExecutor) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    const bool removeLeadingQueryStages = true;
    expCtx->setMongoProcessInterface(std::make_shared<DocumentSourceLookupMockMongoInterface>(
        std::deque<DocumentSource::GetNextResult>{}, removeLeadingQueryStages));

    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {$expr: { $gte: ['$x', "
        "'$$var1']}}}, {$sort: {x: 1}}, {$addFields: {varField: {$sum: ['$x', "
        "'$$var1']}}}], from: 'coll', as: 'as'}}",
        expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);

    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 0));
    ASSERT(subPipeline);

    auto expectedPipe =
        fromjson("[{$mock: {}}, {$addFields: {varField: {$sum: ['$x', {$const: 0}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest, IncrementNestedAggregateOpCounterOnCreateButNotOnCopy) {
    auto testOpCounter = [&](const NamespaceString& nss, const int expectedIncrease) {
        auto resolvedNss = ResolvedNamespaceMap{{nss, {nss, std::vector<BSONObj>()}}};
        auto countBeforeCreate = globalOpCounters().nestedAggregates->value();

        // Create a DocumentSourceLookUp and verify that the counter increases by the expected
        // amount.
        auto originalExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
        originalExpCtx->setResolvedNamespaces(resolvedNss);
        auto docSource = DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from" << nss.coll() << "pipeline"
                                          << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                                          << "as"))
                .firstElement(),
            originalExpCtx);
        auto originalLookup = static_cast<DocumentSourceLookUp*>(docSource.get());
        auto countAfterCreate = globalOpCounters().nestedAggregates->value();
        ASSERT_EQ(countAfterCreate - countBeforeCreate, expectedIncrease);

        // Copy the DocumentSourceLookUp and verify that the counter doesn't increase.
        auto newExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
        newExpCtx->setResolvedNamespaces(resolvedNss);
        DocumentSourceLookUp newLookup{*originalLookup, newExpCtx};
        auto countAfterCopy = globalOpCounters().nestedAggregates->value();
        ASSERT_EQ(countAfterCopy - countAfterCreate, 0);
    };

    testOpCounter(NamespaceString::createNamespaceString_forTest("testDb", "testColl"), 1);
    // $lookup against internal databases should not cause the counter to get incremented.
    testOpCounter(NamespaceString::createNamespaceString_forTest("config", "testColl"), 0);
    testOpCounter(NamespaceString::createNamespaceString_forTest("admin", "testColl"), 0);
    testOpCounter(NamespaceString::createNamespaceString_forTest("local", "testColl"), 0);
}

TEST_F(DocumentSourceLookUpTest, RedactsCorrectlyWithPipeline) {
    auto expCtx = getExpCtx();
    auto fromNs = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    BSONArrayBuilder pipeline;
    pipeline << BSON("$match" << BSON("a" << "myStr"));
    pipeline << BSON("$project" << BSON("_id" << 0 << "a" << 1));
    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << fromNs.coll() << "localField"
                                      << "foo"
                                      << "foreignField"
                                      << "bar"
                                      << "let" << BSON("var1" << "$x") << "pipeline"
                                      << pipeline.arr() << "as"
                                      << "out"))
            .firstElement(),
        expCtx);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$lookup": {
                "from": "HASH<coll>",
                "as": "HASH<out>",
                "localField": "HASH<foo>",
                "foreignField": "HASH<bar>",
                "let": {
                    "HASH<var1>": "$HASH<x>"
                },
                "pipeline": [
                    {
                        "$match": {
                            "HASH<a>": {
                                "$eq": "?string"
                            }
                        }
                    },
                    {
                        "$project": {
                            "HASH<a>": true,
                            "HASH<_id>": false
                        }
                    }
                ]
            }
        })",
        redact(*docSource));
}

// Tests the parse logic for a serialized lookup stage with serializedForCloning enabled and
// absorbed $unwind.
TEST_F(DocumentSourceLookUpTest, LookupParseSerializedStageWithAbsorbedUnwind) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto lookup = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON(
                 "from" << "coll"
                        << "pipeline"
                        << BSON_ARRAY(BSON("$match" << BSON("subfield" << BSON("$eq" << 1))))
                        << "as"
                        << "asField"
                        << "$_internalUnwind" << BSON("$unwind" << BSON("path" << "$asField"))))
            .firstElement(),
        expCtx);

    ASSERT(dynamic_cast<DocumentSourceLookUp*>(lookup.get())->hasUnwindSrc());
}

static boost::intrusive_ptr<DocumentSourceLookUp> makeLookupWithLet(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, NamespaceString fromNs) {
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto spec =
        BSON("$lookup" << BSON("from" << fromNs.coll() << "let" << BSON("v" << "$x") << "pipeline"
                                      << BSON_ARRAY(BSON("$match" << BSON("y" << "$$v"))) << "as"
                                      << "out"));
    auto ds = DocumentSourceLookUp::createFromBson(spec.firstElement(), expCtx);
    return boost::static_pointer_cast<DocumentSourceLookUp>(ds);
}

TEST_F(DocumentSourceLookUpTest, LetVariablesCloneRebindsExpressionContext) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto opCtx = getOpCtx();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, nss);

    // Build an original $lookup with a let expression
    auto lookup = makeLookupWithLet(expCtx, nss);

    // Sanity: expressions in _letVariables use original expCtx
    for (auto& var : lookup->getLetVariables()) {
        ASSERT_EQ(var.expression->getExpressionContext(), expCtx);
    }

    // Clone with a new top-level ExpressionContext
    auto newExpCtx = make_intrusive<ExpressionContextForTest>(opCtx, nss);
    auto lookupClone = static_pointer_cast<DocumentSourceLookUp>(lookup->clone(newExpCtx));

    // Check that every let expression in the clone now points to the new context
    for (auto& var : lookupClone->getLetVariables()) {
        ASSERT_EQ(var.expression->getExpressionContext(), newExpCtx);
    }
}
}  // namespace
}  // namespace mongo
