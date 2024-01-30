/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <deque>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/counters.h"
#include "mongo/idl/server_parameter_test_util.h"

namespace mongo {
namespace {
using boost::intrusive_ptr;
using std::deque;
using std::vector;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceLookUpTest = AggregationContextFixture;

const long long kDefaultMaxCacheSize = internalDocumentSourceLookupCacheSizeBytes.load();
const auto kExplain = ExplainOptions::Verbosity::kQueryPlanner;

// For tests which need to run in a replica set context.
class ReplDocumentSourceLookUpTest : public DocumentSourceLookUpTest {
public:
    void setUp() override {
        Test::setUp();  // Will establish a feature compatibility version.
        auto service = getExpCtx()->opCtx->getServiceContext();
        repl::ReplSettings settings;

        settings.setReplSetString("lookupTestSet/node1:12345");

        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service, settings);

        // Ensure that we are primary.
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
    }
};

auto makeLookUpFromBson(BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto docSource = DocumentSourceLookUp::createFromBson(elem, expCtx);
    auto lookup = static_cast<DocumentSourceLookUp*>(docSource.detach());
    return std::unique_ptr<DocumentSourceLookUp, DocumentSourceDeleter>(lookup,
                                                                        DocumentSourceDeleter());
}

// A 'let' variable defined in a $lookup stage is expected to be available to all sub-pipelines. For
// sub-pipelines below the immediate one, they are passed to via ExpressionContext. This test
// confirms that variables defined in the ExpressionContext are captured by the $lookup stage.
TEST_F(DocumentSourceLookUpTest, PreservesParentPipelineLetVariables) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto varId = expCtx->variablesParseState.defineVariable("foo");
    expCtx->variables.setValue(varId, Value(123));

    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from"
                               << "coll"
                               << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from"
                               << "coll"
                               << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from"
                               << "coll"
                               << "let"
                               << BSON("var1"
                                       << "$x")
                               << "pipeline"
                               << BSON_ARRAY(BSON("$project" << BSON("hasX"
                                                                     << "$$var1"))
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << fromNs.coll().toString() << "pipeline" << BSONArray()
                                      << "as"
                                      << "as"))
            .firstElement(),
        expCtx);
    auto lookup = static_cast<DocumentSourceLookUp*>(docSource.get());

    ASSERT_FALSE(lookup->hasLocalFieldForeignFieldJoin());
    ASSERT(lookup->constraints(Pipeline::SplitState::kUnsplit).diskRequirement ==
           DocumentSource::DiskUseRequirement::kNoDiskUse);
    ASSERT(lookup->constraints(Pipeline::SplitState::kUnsplit).transactionRequirement ==
           DocumentSource::TransactionRequirement::kAllowed);
}

TEST_F(DocumentSourceLookUpTest, LookupWithOutInPipelineNotAllowed) {
    auto ERROR_CODE_OUT_BANNED_IN_LOOKUP = 51047;
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});
    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(BSON("$lookup" << BSON("from"
                                                                    << "coll"
                                                                    << "pipeline"
                                                                    << BSON_ARRAY(BSON("$out"
                                                                                       << "target"))
                                                                    << "as"
                                                                    << "as"))
                                                 .firstElement(),
                                             expCtx),
        AssertionException,
        ERROR_CODE_OUT_BANNED_IN_LOOKUP);
}

TEST_F(DocumentSourceLookUpTest, LiteParsedDocumentSourceLookupContainsExpectedNamespaces) {
    auto stageSpec =
        BSON("$lookup" << BSON("from"
                               << "namespace1"
                               << "pipeline"
                               << BSON_ARRAY(BSON(
                                      "$lookup"
                                      << BSON("from"
                                              << "namespace2"
                                              << "as"
                                              << "lookup2"
                                              << "pipeline"
                                              << BSON_ARRAY(BSON("$match" << BSON("x" << 1))))))
                               << "as"
                               << "lookup1"));

    NamespaceString nss = NamespaceString::createNamespaceString_forTest(boost::none, "test.test");
    std::vector<BSONObj> pipeline;
    auto liteParsedLookup = DocumentSourceLookUp::LiteParsed::parse(nss, stageSpec.firstElement());
    auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();

    ASSERT_EQ(1ul,
              namespaceSet.count(
                  NamespaceString::createNamespaceString_forTest(boost::none, "test.namespace1")));
    ASSERT_EQ(1ul,
              namespaceSet.count(
                  NamespaceString::createNamespaceString_forTest(boost::none, "test.namespace2")));
    ASSERT_EQ(2ul, namespaceSet.size());
}

TEST_F(DocumentSourceLookUpTest, RejectLookupWhenDepthLimitIsExceeded) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    expCtx->subPipelineDepth = internalMaxSubPipelineViewDepth.load();

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from"
                                   << "coll"
                                   << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                   << "as"
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    try {
        auto docSource = DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from"
                                   << "coll"
                                   << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    try {
        auto docSource = DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from"
                                   << "coll"
                                   << "let"
                                   << BSON("var1"
                                           << "$x")
                                   << "pipeline"
                                   << BSON_ARRAY(BSON("$project" << BSON("hasX"
                                                                         << "$$var1"))
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    ASSERT_THROWS_CODE(DocumentSourceLookUp::createFromBson(BSON("$lookup" << BSON("from"
                                                                                   << "coll"
                                                                                   << "let"
                                                                                   << BSON("var1"
                                                                                           << "$a")
                                                                                   << "localField"
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from"
                                   << "coll"
                                   << "let"
                                   << BSON(""  // Empty variable name.
                                           << "$a")
                                   << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                   << "as"
                                   << "as"))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::FailedToParse);

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from"
                                   << "coll"
                                   << "let"
                                   << BSON("^invalidFirstChar"
                                           << "$a")
                                   << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                   << "as"
                                   << "as"))
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::FailedToParse);

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            BSON("$lookup" << BSON("from"
                                   << "coll"
                                   << "let"
                                   << BSON("contains.invalidChar"
                                           << "$a")
                                   << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                   << "as"
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from"
                               << "coll"
                               << "let"
                               << BSON("local_x"
                                       << "$x")
                               << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                               << "as"))
            .firstElement(),
        expCtx);

    //
    // Serialize the $lookup stage and confirm contents.
    //
    vector<Value> serialization;
    lookupStage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);

    // The fields are in no guaranteed order, so we can't perform a simple Document comparison.
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::Object);

    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage.computeSize(), 4ULL);
    ASSERT_VALUE_EQ(serializedStage["from"], Value(std::string("coll")));
    ASSERT_VALUE_EQ(serializedStage["as"], Value(std::string("as")));

    ASSERT_DOCUMENT_EQ(serializedStage["let"].getDocument(),
                       Document(fromjson("{local_x: \"$x\"}")));

    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::Array);
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 1UL);

    ASSERT_EQ(serializedStage["pipeline"][0].getType(), BSONType::Object);
    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][0]["$match"].getDocument(),
                       Document(fromjson("{x: 1}")));

    //
    // Create a new $lookup stage from the serialization. Serialize the new stage and confirm that
    // it is equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBson.firstElement(), expCtx);

    vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceLookUpTest, ShouldBeAbleToReParseSerializedStageWithFieldsAndPipeline) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from"
                               << "coll"
                               << "let"
                               << BSON("local_x"
                                       << "$x")
                               << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
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
    vector<Value> serialization;
    lookupStage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);

    // The fields are in no guaranteed order, so we can't perform a simple Document comparison.
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_EQ(serializedDoc["$lookup"].getType(), BSONType::Object);

    auto serializedStage = serializedDoc["$lookup"].getDocument();
    ASSERT_EQ(serializedStage.computeSize(), 6ULL);
    ASSERT_VALUE_EQ(serializedStage["from"], Value(std::string("coll")));
    ASSERT_VALUE_EQ(serializedStage["as"], Value(std::string("as")));
    ASSERT_VALUE_EQ(serializedStage["localField"], Value(std::string("a")));
    ASSERT_VALUE_EQ(serializedStage["foreignField"], Value(std::string("b")));

    ASSERT_DOCUMENT_EQ(serializedStage["let"].getDocument(),
                       Document(fromjson("{local_x: \"$x\"}")));

    ASSERT_EQ(serializedStage["pipeline"].getType(), BSONType::Array);
    ASSERT_EQ(serializedStage["pipeline"].getArrayLength(), 1UL);

    ASSERT_EQ(serializedStage["pipeline"][0].getType(), BSONType::Object);
    ASSERT_DOCUMENT_EQ(serializedStage["pipeline"][0]["$match"].getDocument(),
                       Document(fromjson("{x: 1}")));

    //
    // Create a new $lookup stage from the serialization. Serialize the new stage and confirm that
    // it is equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBson.firstElement(), expCtx);

    vector<Value> newSerialization;
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto originalBSON = BSON("$lookup" << BSON("from" << BSON("db"
                                                              << "config"
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
    vector<Value> serialization;
    static const UnorderedFieldsBSONObjComparator kComparator;
    lookupStage->serializeToArray(serialization);
    auto serializedBSON = serialization[0].getDocument().toBson();
    ASSERT_EQ(kComparator.compare(serializedBSON, originalBSON), 0);

    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBSON.firstElement(), expCtx);

    vector<Value> newSerialization;
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto originalBSON =
        BSON("$lookup" << BSON("from" << BSON("db"
                                              << "config"
                                              << "coll"
                                              << "cache.chunks.test.foo")
                                      << "let"
                                      << BSON("local_x"
                                              << "$x")
                                      << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                      << "as"
                                      << "as"));
    auto lookupStage = DocumentSourceLookUp::createFromBson(originalBSON.firstElement(), expCtx);

    //
    // Serialize the $lookup stage and confirm contents.
    //
    vector<Value> serialization;
    static const UnorderedFieldsBSONObjComparator kComparator;
    lookupStage->serializeToArray(serialization);
    auto serializedBSON = serialization[0].getDocument().toBson();
    ASSERT_EQ(kComparator.compare(serializedBSON, originalBSON), 0);

    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBSON.firstElement(), expCtx);

    vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}


// Tests that $lookup with 'collation' can be round tripped.
TEST_F(DocumentSourceLookUpTest, LookupReParseSerializedStageWithCollation) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto originalBSON = BSON(
        "$lookup" << BSON("from"
                          << "coll"
                          << "let"
                          << BSON("local_x"
                                  << "$x")
                          << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                          << "as"
                          << "_internalCollation"
                          << BSON("locale"
                                  << "en_US"
                                  << "caseLevel" << false << "caseFirst"
                                  << "off"
                                  << "strength" << 1 << "numericOrdering" << false << "alternate"
                                  << "non-ignorable"
                                  << "maxVariable"
                                  << "punct"
                                  << "normalization" << false << "backwards" << false << "version"
                                  << "57.1")));
    auto lookupStage = DocumentSourceLookUp::createFromBson(originalBSON.firstElement(), expCtx);

    //
    // Serialize the $lookup stage and confirm contents.
    //
    vector<Value> serialization;
    static const UnorderedFieldsBSONObjComparator kComparator;
    lookupStage->serializeToArray(serialization);
    auto serializedBSON = serialization[0].getDocument().toBson();
    std::cout << "serializedBSON: " << serializedBSON << std::endl;
    ASSERT_EQ(kComparator.compare(serializedBSON, originalBSON), 0);

    auto roundTripped = DocumentSourceLookUp::createFromBson(serializedBSON.firstElement(), expCtx);

    vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization);

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}


// $lookup : {from : {db: <>, coll: <>}} syntax doesn't work for a namespace that isn't
// config.cache.chunks*.
TEST_F(DocumentSourceLookUpTest, RejectsPipelineFromDBAndCollWithBadDBAndColl) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            fromjson("{$lookup: {from: {db: 'test', coll: 'coll'}, as: 'as', pipeline: []}}")
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::FailedToParse);
}

// $lookup : {from : {db: <>, coll: <>}} syntax doesn't work for a namespace when "coll" is
// "cache.chunks.*" but "db" is not "config".
TEST_F(DocumentSourceLookUpTest, RejectsPipelineFromDBAndCollWithBadColl) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "config", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    ASSERT_THROWS_CODE(
        DocumentSourceLookUp::createFromBson(
            fromjson("{$lookup: {from: {db: 'config', coll: 'coll'}, as: 'as', pipeline: []}}")
                .firstElement(),
            expCtx),
        AssertionException,
        ErrorCodes::FailedToParse);
}

// $lookup : {from : {db: <>, coll: <>}} syntax fails when "db" is config but "coll" is
// not "cache.chunks.*".
TEST_F(DocumentSourceLookUpTest, RejectsPipelineFromDBAndCollWithBadDB) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest(
        boost::none, "test", "cache.chunks.test.foo");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    ASSERT_THROWS_CODE(DocumentSourceLookUp::createFromBson(
                           fromjson("{$lookup: {from: {db: 'test', coll: 'cache.chunks.test.foo'}, "
                                    "as: 'as', pipeline: []}}")
                               .firstElement(),
                           expCtx),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

// Tests that $lookup distributedPlanLogic() is boost::none, allowing for the stage to run on each
// shard, when it reads from config.cache.chunks.* namespaces using from: {db: <> , coll: <> }
// syntax.
TEST_F(DocumentSourceLookUpTest, FromDBAndCollDistributedPlanLogic) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest(
        boost::none, "config", "cache.chunks.test.foo");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << BSON("db"
                                              << "config"
                                              << "coll"
                                              << "cache.chunks.test.foo")
                                      << "let"
                                      << BSON("local_x"
                                              << "$x")
                                      << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                      << "as"
                                      << "as"))
            .firstElement(),
        expCtx);

    ASSERT(!lookupStage->distributedPlanLogic());
}

// Tests $lookup distributedPlanLogic() is prohibited from executing on the shardsStage for standard
// $lookup with from: <string> syntax.
TEST_F(DocumentSourceLookUpTest, LookupDistributedPlanLogic) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto lookupStage = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from"
                               << "coll"
                               << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                               << "as"))
            .firstElement(),
        expCtx);
    ASSERT(lookupStage->distributedPlanLogic());
    ASSERT(lookupStage->distributedPlanLogic()->shardsStage == nullptr);
    ASSERT_EQ(lookupStage->distributedPlanLogic()->mergingStages.size(), 1);
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
    vector<Value> inputArray = {Value(1), Value(2)};
    Document input = DOC("local" << Value(inputArray));
    BSONObj matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
        input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_BSONOBJ_EQ(matchStage, fromjson("{$match: {foreign: {$in: [1, 2]}}}"));
}

TEST(MakeMatchStageFromInput, ArrayValueWithRegexUsesOrQuery) {
    BSONRegEx regex("^a");
    vector<Value> inputArray = {Value(1), Value(regex), Value(2)};
    Document input = DOC("local" << Value(inputArray));
    BSONObj matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
        input, FieldPath("local"), "foreign", BSONObj());
    ASSERT_BSONOBJ_EQ(
        matchStage,
        BSON("$match" << BSON("$or" << BSON_ARRAY(BSON("foreign" << BSON("$eq" << Value(1)))
                                                  << BSON("foreign" << BSON("$eq" << regex))
                                                  << BSON("foreign" << BSON("$eq" << Value(2)))))));
}

//
// Execution tests.
//

/**
 * A mock MongoProcessInterface which allows mocking a foreign pipeline. If
 * 'removeLeadingQueryStages' is true then any $match, $sort or $project fields at the start of the
 * pipeline will be removed, simulating the pipeline changes which occur when
 * PipelineD::prepareCursorSource absorbs stages into the PlanExecutor.
 */
class MockMongoInterface final : public StubMongoProcessInterface {
public:
    MockMongoInterface(deque<DocumentSource::GetNextResult> mockResults,
                       bool removeLeadingQueryStages = false)
        : _mockResults(std::move(mockResults)),
          _removeLeadingQueryStages(removeLeadingQueryStages) {}

    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) final {
        return false;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        Pipeline* ownedPipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final {
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline(
            ownedPipeline, PipelineDeleter(ownedPipeline->getContext()->opCtx));

        while (_removeLeadingQueryStages && !pipeline->getSources().empty()) {
            if (pipeline->popFrontWithName("$match") || pipeline->popFrontWithName("$sort") ||
                pipeline->popFrontWithName("$project")) {
                continue;
            }
            break;
        }

        pipeline->addInitialSource(
            DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
        return pipeline;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        const AggregateCommandRequest& aggRequest,
        Pipeline* pipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final {
        // Implement this method should any test cases require setting aggregate command options via
        // 'aggRequest'.
        MONGO_UNREACHABLE;
    }

private:
    deque<DocumentSource::GetNextResult> _mockResults;
    bool _removeLeadingQueryStages = false;
};

TEST_F(DocumentSourceLookUpTest, ShouldPropagatePauses) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    // Mock the input of a foreign namespace, pausing every other result.
    auto mockLocalSource =
        DocumentSourceMock::createForTest({Document{{"foreignId", 0}},
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document{{"foreignId", 1}},
                                           DocumentSource::GetNextResult::makePauseExecution()},
                                          expCtx);

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                             Document{{"_id", 1}}};
    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::move(mockForeignContents));

    // Set up the $lookup stage.
    auto lookupSpec = Document{{"$lookup",
                                Document{{"from", fromNs.coll()},
                                         {"localField", "foreignId"_sd},
                                         {"foreignField", "_id"_sd},
                                         {"as", "foreignDocs"_sd}}}}
                          .toBson();
    auto lookup = makeLookUpFromBson(lookupSpec.firstElement(), expCtx);
    lookup->setSource(mockLocalSource.get());

    auto next = lookup->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 0}, {"foreignDocs", {Document{{"_id", 0}}}}}));

    ASSERT_TRUE(lookup->getNext().isPaused());

    next = lookup->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 1}, {"foreignDocs", {Document{{"_id", 1}}}}}));

    ASSERT_TRUE(lookup->getNext().isPaused());

    ASSERT_TRUE(lookup->getNext().isEOF());
    ASSERT_TRUE(lookup->getNext().isEOF());
}

TEST_F(DocumentSourceLookUpTest, ShouldPropagatePausesWhileUnwinding) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    // Mock out the foreign collection.
    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                             Document{{"_id", 1}}};
    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::move(mockForeignContents));

    // Mock its input, pausing every other result.
    auto mockLocalSource =
        DocumentSourceMock::createForTest({Document{{"foreignId", 0}},
                                           DocumentSource::GetNextResult::makePauseExecution(),
                                           Document{{"foreignId", 1}},
                                           DocumentSource::GetNextResult::makePauseExecution()},
                                          expCtx);

    // Set up the $lookup stage.
    auto lookupSpec = Document{{"$lookup",
                                Document{{"from", fromNs.coll()},
                                         {"localField", "foreignId"_sd},
                                         {"foreignField", "_id"_sd},
                                         {"as", "foreignDoc"_sd}}}}
                          .toBson();
    auto lookup = makeLookUpFromBson(lookupSpec.firstElement(), expCtx);

    const bool preserveNullAndEmptyArrays = false;
    const boost::optional<std::string> includeArrayIndex = boost::none;
    lookup->setUnwindStage(DocumentSourceUnwind::create(
        expCtx, "foreignDoc", preserveNullAndEmptyArrays, includeArrayIndex));

    lookup->setSource(mockLocalSource.get());

    auto next = lookup->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 0}, {"foreignDoc", Document{{"_id", 0}}}}));

    ASSERT_TRUE(lookup->getNext().isPaused());

    next = lookup->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 1}, {"foreignDoc", Document{{"_id", 1}}}}));

    ASSERT_TRUE(lookup->getNext().isPaused());

    ASSERT_TRUE(lookup->getNext().isEOF());
    ASSERT_TRUE(lookup->getNext().isEOF());
}

TEST_F(DocumentSourceLookUpTest, LookupReportsAsFieldIsModified) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    // Set up the $lookup stage.
    auto lookupSpec = Document{{"$lookup",
                                Document{{"from", fromNs.coll()},
                                         {"localField", "foreignId"_sd},
                                         {"foreignField", "_id"_sd},
                                         {"as", "foreignDocs"_sd}}}}
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    // Set up the $lookup stage.
    auto lookupSpec = Document{{"$lookup",
                                Document{{"from", fromNs.coll()},
                                         {"localField", "foreignId"_sd},
                                         {"foreignField", "_id"_sd},
                                         {"as", "foreignDoc"_sd}}}}
                          .toBson();
    auto lookup = makeLookUpFromBson(lookupSpec.firstElement(), expCtx);

    const bool preserveNullAndEmptyArrays = false;
    const boost::optional<std::string> includeArrayIndex = std::string("arrIndex");
    lookup->setUnwindStage(DocumentSourceUnwind::create(
        expCtx, "foreignDoc", preserveNullAndEmptyArrays, includeArrayIndex));

    auto modifiedPaths = lookup->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(2U, modifiedPaths.paths.size());
    ASSERT_EQ(1U, modifiedPaths.paths.count("foreignDoc"));
    ASSERT_EQ(1U, modifiedPaths.paths.count("arrIndex"));
}

BSONObj sequentialCacheStageObj(const StringData status = "kBuilding"_sd,
                                const long long maxSizeBytes = kDefaultMaxCacheSize) {
    return BSON("$sequentialCache" << BSON("maxSizeBytes" << maxSizeBytes << "status" << status));
}

TEST_F(DocumentSourceLookUpTest, ShouldCacheNonCorrelatedSubPipelinePrefix) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{});

    auto docSource = DocumentSourceLookUp::createFromBson(
        fromjson("{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x:1}}, {$sort: {x: 1}}, "
                 "{$addFields: {varField: '$$var1'}}], from: 'coll', as: 'as'}}")
            .firstElement(),
        expCtx);

    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    auto subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 5));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{mock: {}}, {$match: {x:{$eq: 1}}}, {$sort: {sortKey: {x: 1}}}, "
                      << sequentialCacheStageObj() << ", {$addFields: {varField: {$const: 5} }}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest,
       ShouldDiscoverVariablesReferencedInFacetPipelineAfterAnExhaustiveAllStage) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{});

    // In the $facet stage here, the correlated $match stage comes after a $group stage which
    // returns EXHAUSTIVE_ALL for its dependencies. Verify that we continue enumerating the $facet
    // pipeline's variable dependencies after this point, so that the $facet stage is correctly
    // identified as correlated and the cache is placed before it in the $lookup pipeline.
    auto docSource = DocumentSourceLookUp::createFromBson(
        fromjson("{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x:1}}, {$sort: {x: 1}}, "
                 "{$facet: {facetPipe: [{$group: {_id: '$_id'}}, {$match: {$expr: {$eq: ['$_id', "
                 "'$$var1']}}}]}}], from: 'coll', as: 'as'}}")
            .firstElement(),
        expCtx);

    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    auto subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 5));
    ASSERT(subPipeline);

    // Note that the second $match stage should be moved up to before the $group stage, since $group
    // should swap with $match when filtering on $_id.
    auto expectedPipe =
        fromjson(str::stream() << "[{mock: {}},"
                                  " {$match: {x:{$eq: 1}}},"
                                  " {$sort: {sortKey: {x: 1}}},"
                               << sequentialCacheStageObj()
                               << ",{$facet: {facetPipe: ["
                                  "   {$internalFacetTeeConsumer: {}},"
                                  "   {$match: {$and: [{_id: {$_internalExprEq: 5}},"
                                  "                    {$expr: {$eq: ['$_id', {$const: 5}]}}]}},"
                                  "   {$group: {_id: '$_id'}}]}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest, ExprEmbeddedInMatchExpressionShouldBeOptimized) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{});

    // This pipeline includes a $match stage that itself includes a $expr expression.
    auto docSource = DocumentSourceLookUp::createFromBson(
        fromjson("{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {$expr: {$eq: "
                 "['$_id','$$var1']}}}], from: 'coll', as: 'as'}}")
            .firstElement(),
        expCtx);

    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    auto subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 5));
    ASSERT(subPipeline);

    auto sources = subPipeline->getSources();
    ASSERT_GTE(sources.size(), 2u);

    // The first source is our mock data source, and the second should be the $match expression.
    auto secondSource = *(++sources.begin());
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
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{});

    // The $project stage defines a local variable with the same name as the $lookup 'let' variable.
    // Verify that the $project is identified as non-correlated and the cache is placed after it.
    auto docSource = DocumentSourceLookUp::createFromBson(
        fromjson("{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x: 1}}, {$sort: {x: 1}}, "
                 "{$project: {projectedField: {$let: {vars: {var1: 'abc'}, in: "
                 "'$$var1'}}, _id: false}}, {$addFields: {varField: {$sum: ['$x', '$$var1']}}}], "
                 "from: 'coll', "
                 "as: 'as'}}")
            .firstElement(),
        expCtx);

    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    auto subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 5));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{mock: {}}, {$match: {x: {$eq: 1}}}, {$sort: {sortKey: {x: 1}}}, "
                         "{$project: {projectedField: {$let: {vars: {var1: {$const: 'abc'}}, "
                         "in: '$$var1'}}, _id: false}},"
                      << sequentialCacheStageObj()
                      << ", {$addFields: {varField: {$sum: ['$x', {$const: 5}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest, ShouldInsertCacheBeforeCorrelatedNestedLookup) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{});

    // Create a $lookup stage whose pipeline contains nested $lookups. The third-level $lookup
    // refers to a 'let' variable defined in the top-level $lookup. Verify that the second-level
    // $lookup is correctly identified as a correlated stage and the cache is placed before it.
    auto docSource = DocumentSourceLookUp::createFromBson(
        fromjson("{$lookup: {from: 'coll', as: 'as', let: {var1: '$_id'}, pipeline: [{$match: "
                 "{x:1}}, {$sort: {x: 1}}, {$lookup: {from: 'coll', as: 'subas', pipeline: "
                 "[{$match: {x: 1}}, {$lookup: {from: 'coll', as: 'subsubas', pipeline: [{$match: "
                 "{$expr: {$eq: ['$y', '$$var1']}}}]}}]}}, {$addFields: {varField: '$$var1'}}]}}")
            .firstElement(),
        expCtx);

    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    auto subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 5));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{mock: {}}, {$match: {x:{$eq: 1}}}, {$sort: {sortKey: {x: 1}}}, "
                      << sequentialCacheStageObj()
                      << ", {$lookup: {from: 'coll', as: 'subas', let: {}, pipeline: "
                         "[{$match: {x: 1}}, {$lookup: {from: 'coll', as: 'subsubas', "
                         "pipeline: [{$match: {$expr: {$eq: ['$y', '$$var1']}}}]}}]}}, "
                         "{$addFields: {varField: {$const: 5}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest,
       ShouldIgnoreNestedLookupLetVariablesShadowingOuterLookupLetVariablesWhenFindingPrefix) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{});

    // The nested $lookup stage defines a 'let' variable with the same name as the top-level 'let'.
    // Verify the nested $lookup is identified as non-correlated and the cache is placed after it.
    auto docSource = DocumentSourceLookUp::createFromBson(
        fromjson("{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x:1}}, {$sort: {x: 1}}, "
                 "{$lookup: {let: {var1: '$y'}, pipeline: [{$match: {$expr: { $eq: ['$z', "
                 "'$$var1']}}}], from: 'coll', as: 'subas'}}, {$addFields: {varField: '$$var1'}}], "
                 "from: 'coll', as: 'as'}}")
            .firstElement(),
        expCtx);

    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    auto subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 5));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{mock: {}}, {$match: {x:{$eq: 1}}}, {$sort: {sortKey: {x: 1}}}, "
                         "{$lookup: {from: 'coll', as: 'subas', let: {var1: '$y'}, "
                         "pipeline: [{$match: {$expr: { $eq: ['$z', '$$var1']}}}]}}, "
                      << sequentialCacheStageObj() << ", {$addFields: {varField: {$const: 5} }}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest, ShouldCacheEntirePipelineIfNonCorrelated) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto docSource = DocumentSourceLookUp::createFromBson(
        fromjson("{$lookup: {let: {}, pipeline: [{$match: {x:1}}, {$sort: {x: 1}}, {$lookup: "
                 "{pipeline: [{$match: {y: 5}}], from: 'coll', as: 'subas'}}, {$addFields: "
                 "{constField: 5}}], from: 'coll', as: 'as'}}")
            .firstElement(),
        expCtx);

    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    expCtx->mongoProcessInterface =
        std::make_shared<MockMongoInterface>(std::deque<DocumentSource::GetNextResult>{});

    auto subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 5));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream()
        << "[{mock: {}}, {$match: {x:{$eq: 1}}}, {$sort: {sortKey: {x: 1}}}, {$lookup: {from: "
           "'coll', as: 'subas', let: {}, pipeline: [{$match: {y: 5}}]}}, {$addFields: "
           "{constField: {$const: 5}}}, "
        << sequentialCacheStageObj() << "]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest,
       ShouldReplaceNonCorrelatedPrefixWithCacheAfterFirstSubPipelineIteration) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    deque<DocumentSource::GetNextResult> mockForeignContents{
        Document{{"x", 0}}, Document{{"x", 1}}, Document{{"x", 2}}};
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockForeignContents);

    auto docSource = DocumentSourceLookUp::createFromBson(
        fromjson(
            "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x: {$gte: 0}}}, {$sort: {x: "
            "1}}, {$addFields: {varField: {$sum: ['$x', '$$var1']}}}], from: 'coll', as: 'as'}}")
            .firstElement(),
        expCtx);

    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    // Prepare the mocked local source.
    auto mockLocalSource = DocumentSourceMock::createForTest(
        {Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}}, expCtx);

    lookupStage->setSource(mockLocalSource.get());

    // Confirm that the empty 'kBuilding' cache is placed just before the correlated $addFields.
    auto subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 0));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{mock: {}}, {$match: {x: {$gte: 0}}}, {$sort: {sortKey: {x: 1}}}, "
                      << sequentialCacheStageObj("kBuilding")
                      << ", {$addFields: {varField: {$sum: ['$x', {$const: 0}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));

    // Verify the first result (non-cached) from the $lookup, for local document {_id: 0}.
    auto nonCachedResult = lookupStage->getNext();
    ASSERT(nonCachedResult.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        Document{fromjson(
            "{_id: 0, as: [{x: 0, varField: 0}, {x: 1, varField: 1}, {x: 2, varField: 2}]}")},
        nonCachedResult.getDocument());

    // Preview the subpipeline that will be used to process the second local document {_id: 1}. The
    // sub-pipeline cache has been built on the first iteration, and is now serving in place of the
    // mocked foreign input source and the non-correlated stages at the start of the pipeline.
    subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 1));
    ASSERT(subPipeline);

    expectedPipe =
        fromjson(str::stream() << "[" << sequentialCacheStageObj("kServing")
                               << ", {$addFields: {varField: {$sum: ['$x', {$const: 1}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));

    // Verify that the rest of the results are correctly constructed from the cache.
    auto cachedResult = lookupStage->getNext();
    ASSERT(cachedResult.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        Document{fromjson(
            "{_id: 1, as: [{x: 0, varField: 1}, {x: 1, varField: 2}, {x: 2, varField: 3}]}")},
        cachedResult.getDocument());

    cachedResult = lookupStage->getNext();
    ASSERT(cachedResult.isAdvanced());
    ASSERT_DOCUMENT_EQ(
        Document{fromjson(
            "{_id: 2, as: [{x: 0, varField: 2}, {x: 1, varField: 3}, {x: 2, varField: 4}]}")},
        cachedResult.getDocument());
}

TEST_F(DocumentSourceLookUpTest,
       ShouldAbandonCacheIfMaxSizeIsExceededAfterFirstSubPipelineIteration) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"x", 0}},
                                                             Document{{"x", 1}}};
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(mockForeignContents);

    // Ensure the cache is abandoned after the first iteration by setting its max size to 0.
    size_t maxCacheSizeBytes = 0;
    auto docSource = DocumentSourceLookUp::createFromBsonWithCacheSize(
        fromjson(
            "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x: {$gte: 0}}}, {$sort: {x: "
            "1}}, {$addFields: {varField: {$sum: ['$x', '$$var1']}}}], from: 'coll', as: 'as'}}")
            .firstElement(),
        expCtx,
        maxCacheSizeBytes);

    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    // Prepare the mocked local and foreign sources.
    auto mockLocalSource =
        DocumentSourceMock::createForTest({Document{{"_id", 0}}, Document{{"_id", 1}}}, expCtx);

    lookupStage->setSource(mockLocalSource.get());

    // Confirm that the empty 'kBuilding' cache is placed just before the correlated $addFields.
    auto subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 0));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{mock: {}}, {$match: {x: {$gte: 0}}}, {$sort: {sortKey: {x: 1}}}, "
                      << sequentialCacheStageObj("kBuilding", 0ll)
                      << ", {$addFields: {varField: {$sum: ['$x', {$const: 0}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));

    // Get the first result from the stage, for local document {_id: 0}.
    auto firstResult = lookupStage->getNext();
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{_id: 0, as: [{x: 0, varField: 0}, {x: 1, varField: 1}]}")},
        firstResult.getDocument());

    // Preview the subpipeline that will be used to process the second local document {_id: 1}. The
    // sub-pipeline cache exceeded its max size on the first iteration, was abandoned, and is now
    // absent from the pipeline.
    subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 1));
    ASSERT(subPipeline);

    expectedPipe = fromjson(str::stream()
                            << "[{mock: {}}, {$match: {x: {$gte: 0}}}, {$sort: {sortKey: {x: 1}}}, "
                               "{$addFields: {varField: {$sum: ['$x', {$const: 1}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));

    // Verify that the second document is constructed correctly without the cache.
    auto secondResult = lookupStage->getNext();

    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{_id: 1, as: [{x: 0, varField: 1}, {x: 1, varField: 2}]}")},
        secondResult.getDocument());
}

TEST_F(DocumentSourceLookUpTest, ShouldNotCacheIfCorrelatedStageIsAbsorbedIntoPlanExecutor) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    const bool removeLeadingQueryStages = true;
    expCtx->mongoProcessInterface = std::make_shared<MockMongoInterface>(
        std::deque<DocumentSource::GetNextResult>{}, removeLeadingQueryStages);

    auto docSource = DocumentSourceLookUp::createFromBson(
        fromjson("{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {$expr: { $gte: ['$x', "
                 "'$$var1']}}}, {$sort: {x: 1}}, {$addFields: {varField: {$sum: ['$x', "
                 "'$$var1']}}}], from: 'coll', as: 'as'}}")
            .firstElement(),
        expCtx);

    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);

    auto subPipeline = lookupStage->getSubPipeline_forTest(DOC("_id" << 0));
    ASSERT(subPipeline);

    auto expectedPipe =
        fromjson("[{mock: {}}, {$addFields: {varField: {$sum: ['$x', {$const: 0}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));
}

TEST_F(DocumentSourceLookUpTest, IncrementNestedAggregateOpCounterOnCreateButNotOnCopy) {
    auto testOpCounter = [&](const NamespaceString& nss, const int expectedIncrease) {
        auto resolvedNss = StringMap<ExpressionContext::ResolvedNamespace>{
            {nss.coll().toString(), {nss, std::vector<BSONObj>()}}};
        auto countBeforeCreate = globalOpCounters.getNestedAggregate()->load();

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
        auto countAfterCreate = globalOpCounters.getNestedAggregate()->load();
        ASSERT_EQ(countAfterCreate - countBeforeCreate, expectedIncrease);

        // Copy the DocumentSourceLookUp and verify that the counter doesn't increase.
        auto newExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
        newExpCtx->setResolvedNamespaces(resolvedNss);
        DocumentSourceLookUp newLookup{*originalLookup, newExpCtx};
        auto countAfterCopy = globalOpCounters.getNestedAggregate()->load();
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
    auto fromNs = NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    BSONArrayBuilder pipeline;
    pipeline << BSON("$match" << BSON("a"
                                      << "myStr"));
    pipeline << BSON("$project" << BSON("_id" << 0 << "a" << 1));
    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from" << fromNs.coll() << "localField"
                                      << "foo"
                                      << "foreignField"
                                      << "bar"
                                      << "let"
                                      << BSON("var1"
                                              << "$x")
                                      << "pipeline" << pipeline.arr() << "as"
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

using DocumentSourceLookUpServerlessTest = ServerlessAggregationContextFixture;

TEST_F(DocumentSourceLookUpServerlessTest,
       LiteParsedDocumentSourceLookupContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();

    auto stageSpec =
        BSON("$lookup" << BSON("from"
                               << "namespace1"
                               << "pipeline"
                               << BSON_ARRAY(BSON(
                                      "$lookup"
                                      << BSON("from"
                                              << "namespace2"
                                              << "as"
                                              << "lookup2"
                                              << "pipeline"
                                              << BSON_ARRAY(BSON("$match" << BSON("x" << 1))))))
                               << "as"
                               << "lookup1"));

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), _targetColl);
    auto liteParsedLookup = DocumentSourceLookUp::LiteParsed::parse(nss, stageSpec.firstElement());
    auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();

    ASSERT_EQ(1ul,
              namespaceSet.count(NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(),
                                                                                "namespace1")));
    ASSERT_EQ(1ul,
              namespaceSet.count(NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(),
                                                                                "namespace2")));
    ASSERT_EQ(2ul, namespaceSet.size());
}

TEST_F(
    DocumentSourceLookUpServerlessTest,
    LiteParsedDocumentSourceLookupObjExpectedNamespacesInServerlessWhenPassingInNssWithTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();

    auto stageSpec =
        BSON("$lookup" << BSON(
                 "from" << BSON("db"
                                << "config"
                                << "coll"
                                << "cache.chunks.test.foo")
                        << "pipeline"
                        << BSON_ARRAY(BSON(
                               "$lookup"
                               << BSON("from" << BSON("db"
                                                      << "local"
                                                      << "coll"
                                                      << "oplog.rs")
                                              << "as"
                                              << "lookup2"
                                              << "pipeline"
                                              << BSON_ARRAY(BSON("$match" << BSON("x" << 1))))))
                        << "as"
                        << "lookup1"));
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), _targetColl);

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        // The result must match one of several system const NamespaceStrings, which means parse()
        // will fail an assertion if nss contains any tenantId.
        ASSERT_THROWS_CODE(DocumentSourceLookUp::LiteParsed::parse(nss, stageSpec.firstElement()),
                           AssertionException,
                           ErrorCodes::FailedToParse);
    }
}

TEST_F(DocumentSourceLookUpServerlessTest,
       LiteParsedDocumentSourceLookupObjExpectedNamespacesInServerlessWithFlags) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();

    auto stageSpec =
        BSON("$lookup" << BSON(
                 "from" << BSON("db"
                                << "config"
                                << "coll"
                                << "cache.chunks.test.foo")
                        << "pipeline"
                        << BSON_ARRAY(BSON(
                               "$lookup"
                               << BSON("from" << BSON("db"
                                                      << "local"
                                                      << "coll"
                                                      << "oplog.rs")
                                              << "as"
                                              << "lookup2"
                                              << "pipeline"
                                              << BSON_ARRAY(BSON("$match" << BSON("x" << 1))))))
                        << "as"
                        << "lookup1"));

    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        boost::none, expCtx->ns.dbName().toString(), _targetColl);

    for (bool flagStatus : {false, true}) {
        RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID",
                                                                   flagStatus);

        auto liteParsedLookup =
            DocumentSourceLookUp::LiteParsed::parse(nss, stageSpec.firstElement());
        auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();

        ASSERT_EQ(1ul,
                  namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                      boost::none, "config", "cache.chunks.test.foo")));
        ASSERT_EQ(1ul,
                  namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                      boost::none, "local", "oplog.rs")));
        ASSERT_EQ(2ul, namespaceSet.size());
    }
}

TEST_F(DocumentSourceLookUpServerlessTest, CreateFromBSONContainsExpectedNamespacesInServerless) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    ASSERT(expCtx->ns.tenantId());

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll");
    expCtx->setResolvedNamespaces(StringMap<ExpressionContext::ResolvedNamespace>{
        {fromNs.coll().toString(), {fromNs, std::vector<BSONObj>()}}});

    auto docSource = DocumentSourceLookUp::createFromBson(
        BSON("$lookup" << BSON("from"
                               << "coll"
                               << "pipeline" << BSON_ARRAY(BSON("$match" << BSON("x" << 1))) << "as"
                               << "as"))
            .firstElement(),
        expCtx);
    auto lookupStage = static_cast<DocumentSourceLookUp*>(docSource.get());
    ASSERT(lookupStage);
    ASSERT_EQ(lookupStage->getFromNs(),
              NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "coll"));
}

}  // namespace
}  // namespace mongo
