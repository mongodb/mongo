// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_graph_lookup.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/graph_lookup_mock_mongo_interface.h"
#include "mongo/db/pipeline/lite_parsed_graph_lookup.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/pipeline/serverless_aggregation_context_fixture.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <deque>
#include <initializer_list>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceGraphLookUpTest = AggregationContextFixture;

// For use when the foreign namespace doesn't matter much, or we're not explicitly testing view
// resolution logic.
const NamespaceString kForeignNs =
    NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");

constexpr std::string_view kViewName = "foreignView";
const NamespaceString kViewNss =
    NamespaceString::createNamespaceString_forTest(boost::none, "test", kViewName);
constexpr std::string_view kBackingCollName = "actualColl";
const NamespaceString kBackingNss =
    NamespaceString::createNamespaceString_forTest(boost::none, "test", kBackingCollName);

std::unique_ptr<LiteParsedGraphLookUp> parseLiteGraphLookup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::string_view targetNs = kForeignNs.coll()) {
    auto stageSpec = BSON("$graphLookup" << BSON("from" << targetNs << "startWith" << "$a"
                                                        << "connectFromField" << "b"
                                                        << "connectToField" << "c"
                                                        << "as" << "d"));
    return LiteParsedGraphLookUp::parse(
        expCtx->getNamespaceString(), stageSpec.firstElement(), LiteParserOptions{});
}

struct SerializeGraphLookupWithResolvedViewOptions {
    bool flagEnabled = true;
    query_shape::SerializationOptions serOpts =
        query_shape::SerializationOptions{.isSerializingForRemoteDispatch = true};
    bool inRouter = false;
};

BSONObj serializeGraphLookupWithResolvedView(
    const boost::intrusive_ptr<ExpressionContext>& testExpCtx,
    const SerializeGraphLookupWithResolvedViewOptions& options = {}) {
    auto ifrCtx = IncrementalFeatureRolloutContext::forTest(std::vector<BSONObj>{BSON(
        "name" << "featureFlagExtensionsInsideHybridSearch" << "value" << options.flagEnabled)});
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(testExpCtx->getOperationContext())
                      .ns(testExpCtx->getNamespaceString())
                      .ifrContext(ifrCtx)
                      .inRouter(options.inRouter)
                      .build();

    ResolvedNamespaceViewOptions viewOpts;
    viewOpts.involvedNamespaceIsAView = true;
    viewOpts.shouldParseLpp = true;
    ResolvedNamespaceMap resolvedNss;
    resolvedNss.emplace(kViewNss,
                        ResolvedNamespace(kViewNss,
                                          kBackingNss,
                                          std::vector<BSONObj>{BSON("$match" << BSON("x" << 1))},
                                          BSONObj{},
                                          viewOpts));
    expCtx->setResolvedNamespaces(std::move(resolvedNss));

    auto spec = BSON("$graphLookup" << BSON("from" << kViewName << "startWith" << "$a"
                                                   << "connectFromField" << "b"
                                                   << "connectToField" << "c" << "as" << "d"));
    auto ds = DocumentSourceGraphLookUp::createFromBson(spec.firstElement(), expCtx);
    std::vector<Value> serialized;
    ds->serializeToArray(serialized, options.serOpts);
    ASSERT_EQ(1ul, serialized.size());
    return serialized[0].getDocument().toBson();
}

//
// Evaluation.
//

TEST_F(DocumentSourceGraphLookUpTest, GraphLookupShouldReportAsFieldIsModified) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<test::GraphLookUpMockMongoInterface>(
        std::deque<DocumentSource::GetNextResult>{}));
    auto graphLookupStage = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::createPathFromString(
            expCtx.get(), "startPoint", expCtx->variablesParseState),
        boost::none,
        boost::none,
        boost::none,
        boost::none);

    auto modifiedPaths = graphLookupStage->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(1U, modifiedPaths.paths.size());
    ASSERT_EQ(1U, modifiedPaths.paths.count("results"));
}

TEST_F(DocumentSourceGraphLookUpTest, GraphLookupShouldReportFieldsModifiedByAbsorbedUnwind) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<test::GraphLookUpMockMongoInterface>(
        std::deque<DocumentSource::GetNextResult>{}));
    auto unwindStage =
        DocumentSourceUnwind::create(expCtx, "results", false, std::string("arrIndex"));
    auto graphLookupStage = DocumentSourceGraphLookUp::create(
        expCtx,
        fromNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::createPathFromString(
            expCtx.get(), "startPoint", expCtx->variablesParseState),
        boost::none,
        boost::none,
        boost::none,
        unwindStage);

    auto modifiedPaths = graphLookupStage->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(2U, modifiedPaths.paths.size());
    ASSERT_EQ(1U, modifiedPaths.paths.count("results"));
    ASSERT_EQ(1U, modifiedPaths.paths.count("arrIndex"));
}

TEST_F(DocumentSourceGraphLookUpTest, IncrementNestedAggregateOpCounterOnCreateButNotOnCopy) {
    auto testOpCounter = [&](const NamespaceString& nss, const int expectedIncrease) {
        auto resolvedNss = ResolvedNamespaceMap{{nss, {nss, std::vector<BSONObj>()}}};
        auto countBeforeCreate = globalOpCounters().nestedAggregates->value();

        // Create a DocumentSourceGraphLookUp and verify that the counter increases by the expected
        // amount.
        auto originalExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
        originalExpCtx->setResolvedNamespaces(resolvedNss);
        auto docSource = DocumentSourceGraphLookUp::createFromBson(
            BSON("$graphLookup" << BSON("from" << nss.coll() << "startWith"
                                               << "$x"
                                               << "connectFromField"
                                               << "id"
                                               << "connectToField"
                                               << "id"
                                               << "as"
                                               << "connections"))
                .firstElement(),
            originalExpCtx);
        auto originalGraphLookup = static_cast<DocumentSourceGraphLookUp*>(docSource.get());
        auto countAfterCreate = globalOpCounters().nestedAggregates->value();
        ASSERT_EQ(countAfterCreate - countBeforeCreate, expectedIncrease);

        // Copy the DocumentSourceGraphLookUp and verify that the counter doesn't increase.
        auto newExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
        newExpCtx->setResolvedNamespaces(resolvedNss);
        DocumentSourceGraphLookUp newGraphLookup{*originalGraphLookup, newExpCtx};
        auto countAfterCopy = globalOpCounters().nestedAggregates->value();
        ASSERT_EQ(countAfterCopy - countAfterCreate, 0);
    };

    testOpCounter(NamespaceString::createNamespaceString_forTest("testDb", "testColl"), 1);
    // $graphLookup against internal databases should not cause the counter to get incremented.
    testOpCounter(NamespaceString::createNamespaceString_forTest("config", "testColl"), 0);
    testOpCounter(NamespaceString::createNamespaceString_forTest("admin", "testColl"), 0);
    testOpCounter(NamespaceString::createNamespaceString_forTest("local", "testColl"), 0);
}

TEST_F(DocumentSourceGraphLookUpTest, RedactionStartWithSingleField) {
    NamespaceString graphLookupNs(NamespaceString::createNamespaceString_forTest(
        getExpCtx()->getNamespaceString().dbName(), "coll"));
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{graphLookupNs, {graphLookupNs, std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        "$graphLookup": {
            "from": "coll",
            "startWith": "$a.b",
            "connectFromField": "c.d",
            "connectToField": "e.f",
            "as": "x",
            "depthField": "y",
            "maxDepth": 5,
            "restrictSearchWithMatch": {
                "foo": "abc",
                "bar.baz": { "$gt": 5 }
            }
        }
    })");
    auto docSource = DocumentSourceGraphLookUp::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$graphLookup": {
                "from": "HASH<coll>",
                "as": "HASH<x>",
                "connectToField": "HASH<e>.HASH<f>",
                "connectFromField": "HASH<c>.HASH<d>",
                "startWith": "$HASH<a>.HASH<b>",
                "depthField": "HASH<y>",
                "maxDepth": "?number",
                "restrictSearchWithMatch": {
                    "$and": [
                        {
                            "HASH<foo>": {
                                "$eq": "?string"
                            }
                        },
                        {
                            "HASH<bar>.HASH<baz>": {
                                "$gt": "?number"
                            }
                        }
                    ]
                }
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceGraphLookUpTest, RedactionStartWithArrayOfFields) {
    NamespaceString graphLookupNs(NamespaceString::createNamespaceString_forTest(
        getExpCtx()->getNamespaceString().dbName(), "coll"));
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{graphLookupNs, {graphLookupNs, std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $graphLookup: {
            from: "coll",
            startWith: ["$a.b", "$bar.baz"],
            connectFromField: "x",
            connectToField: "y",
            as: "z"
        }
    })");
    auto docSource = DocumentSourceGraphLookUp::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$graphLookup": {
                "from": "HASH<coll>",
                "as": "HASH<z>",
                "connectToField": "HASH<y>",
                "connectFromField": "HASH<x>",
                "startWith": ["$HASH<a>.HASH<b>", "$HASH<bar>.HASH<baz>"]
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceGraphLookUpTest, RedactionWithAbsorbedUnwind) {
    auto expCtx = getExpCtx();

    NamespaceString graphLookupNs(NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "coll"));
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{graphLookupNs, {graphLookupNs, std::vector<BSONObj>()}}});

    auto unwindStage = DocumentSourceUnwind::create(expCtx, "results", false, boost::none);
    auto graphLookupStage = DocumentSourceGraphLookUp::create(
        getExpCtx(),
        graphLookupNs,
        "results",
        "from",
        "to",
        ExpressionFieldPath::createPathFromString(
            expCtx.get(), "startPoint", expCtx->variablesParseState),
        boost::none,
        boost::none,
        boost::none,
        unwindStage);

    auto serialized = redactToArray(*graphLookupStage);
    ASSERT_EQ(2, serialized.size());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$graphLookup": {
                "from": "HASH<coll>",
                "as": "HASH<results>",
                "connectToField": "HASH<to>",
                "connectFromField": "HASH<from>",
                "startWith": "$HASH<startPoint>"
            }
        })",
        serialized[0].getDocument().toBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$unwind": {
                path: "$HASH<results>"
            }
        })",
        serialized[1].getDocument().toBson());
}

using DocumentSourceGraphLookupServerlessTest = ServerlessAggregationContextFixture;

TEST_F(DocumentSourceGraphLookupServerlessTest,
       LiteParsedDocumentSourceLookupStringExpectedNamespacesInServerless) {
    unittest::ServerParameterGuard multitenancySupportController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    auto originalBSON = BSON("$graphLookup" << BSON("from" << "foo"
                                                           << "startWith"
                                                           << "$x"
                                                           << "connectFromField"
                                                           << "id"
                                                           << "connectToField"
                                                           << "id"
                                                           << "as"
                                                           << "connections"));

    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), _targetColl);
    auto liteParsedLookup =
        LiteParsedGraphLookUp::parse(nss, originalBSON.firstElement(), LiteParserOptions{});
    auto namespaceSet = liteParsedLookup->getInvolvedNamespaces();
    ASSERT_EQ(1, namespaceSet.size());
    ASSERT_EQ(1ul,
              namespaceSet.count(NamespaceString::createNamespaceString_forTest(
                  expCtx->getNamespaceString().dbName(), "foo")));
}

TEST_F(DocumentSourceGraphLookupServerlessTest,
       CreateFromBSONContainsExpectedNamespacesInServerless) {
    unittest::ServerParameterGuard multitenancyController("multitenancySupport", true);

    auto expCtx = getExpCtx();
    auto tenantId = expCtx->getNamespaceString().tenantId();
    ASSERT(tenantId);

    NamespaceString graphLookupNs(NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "foo"));
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{graphLookupNs, {graphLookupNs, std::vector<BSONObj>()}}});

    auto spec = BSON("$graphLookup" << BSON("from" << "foo"
                                                   << "startWith"
                                                   << "$x"
                                                   << "connectFromField"
                                                   << "id"
                                                   << "connectToField"
                                                   << "id"
                                                   << "as"
                                                   << "connections"));
    auto graphLookupStage = DocumentSourceGraphLookUp::createFromBson(spec.firstElement(), expCtx);
    auto pipeline =
        Pipeline::create({DocumentSourceMock::createForTest({}, expCtx), graphLookupStage}, expCtx);
    auto involvedNssSet = pipeline->getInvolvedCollections();
    ASSERT_EQ(involvedNssSet.size(), 1UL);
    ASSERT_EQ(1ul, involvedNssSet.count(graphLookupNs));
}

TEST_F(DocumentSourceGraphLookUpTest, StartWithCloneRebindsExpressionContext) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto resolvedNss = ResolvedNamespaceMap{{nss, {nss, std::vector<BSONObj>()}}};

    auto opCtx = getOpCtx();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, nss);
    expCtx->setResolvedNamespaces(resolvedNss);

    auto spec = fromjson(R"({
        "$graphLookup": {
            "from": "coll",
            "startWith": { "$cond": [ { "$eq": ["$f", "v"] }, "$f", "$$REMOVE" ] },
            "connectFromField": "f",
            "connectToField": "f",
            "as": "j"
        }
    })");

    auto ds = DocumentSourceGraphLookUp::createFromBson(spec.firstElement(), expCtx);
    DocumentSourceGraphLookUp* docSource = static_cast<DocumentSourceGraphLookUp*>(ds.get());

    // docSource points to the ExpressionContext
    ASSERT_EQ(docSource->getStartWithField()->getExpressionContext(), expCtx);

    // Clone with a new top-level ExpressionContext
    auto newExpCtx = make_intrusive<ExpressionContextForTest>(getOpCtx(), nss);
    newExpCtx->setResolvedNamespaces(resolvedNss);
    auto dsClone = docSource->clone(newExpCtx);
    DocumentSourceGraphLookUp* docSourceClone =
        static_cast<DocumentSourceGraphLookUp*>(dsClone.get());

    // docSource still points to the original ExpressionContext
    ASSERT_EQ(docSource->getStartWithField()->getExpressionContext(), expCtx);
    // clonedDocSource points to the new ExpressionContext
    ASSERT_EQ(docSourceClone->getStartWithField()->getExpressionContext(), newExpCtx);
}

TEST_F(DocumentSourceGraphLookUpTest, LiteParsedGraphLookupInvolvedNamespacesReturnsForeignNss) {
    auto liteParsed = parseLiteGraphLookup(getExpCtx());
    auto nssSet = liteParsed->getInvolvedNamespaces();
    ASSERT_EQ(1ul, nssSet.size());
    ASSERT_EQ(1ul, nssSet.count(kForeignNs));
}

TEST_F(DocumentSourceGraphLookUpTest, LiteParsedGraphLookupForeignExecutionNamespacesIsEmpty) {
    auto liteParsed = parseLiteGraphLookup(getExpCtx());
    stdx::unordered_set<NamespaceString> nssSet;
    liteParsed->getForeignExecutionNamespaces(nssSet);
    ASSERT_TRUE(nssSet.empty());
}

TEST_F(DocumentSourceGraphLookUpTest, LiteParsedGraphLookupRequiredPrivileges) {
    auto liteParsed = parseLiteGraphLookup(getExpCtx());
    auto privileges = liteParsed->requiredPrivileges(/*isMongos*/ false,
                                                     /*bypassDocumentValidation*/ false);
    ASSERT_EQ(1ul, privileges.size());
    ASSERT_EQ(privileges[0].getResourcePattern(), ResourcePattern::forExactNamespace(kForeignNs));
    ASSERT_TRUE(privileges[0].getActions().contains(ActionType::find));
}

TEST_F(DocumentSourceGraphLookUpTest, LiteParsedGraphLookupHasEmptySubPipelinesWithoutViewBinding) {
    auto liteParsed = parseLiteGraphLookup(getExpCtx());
    ASSERT_TRUE(liteParsed->getMutableSubPipelines()->empty());
}

TEST_F(DocumentSourceGraphLookUpTest, CreateFromBsonRejectsDuplicateFields) {
    auto spec = BSON("$graphLookup" << BSON("from" << "coll"
                                                   << "startWith"
                                                   << "$x"
                                                   << "connectFromField"
                                                   << "id"
                                                   << "connectToField"
                                                   << "id"
                                                   << "as"
                                                   << "results"
                                                   << "from"
                                                   << "other_coll"));
    ASSERT_THROWS_CODE(DocumentSourceGraphLookUp::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       12735700);
}

TEST_F(DocumentSourceGraphLookUpTest,
       LiteParsedGraphLookupBindResolvedNamespacePopulatesPipelines) {
    auto liteParsed = parseLiteGraphLookup(getExpCtx(), kViewName);

    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    opts.shouldParseLpp = true;
    ResolvedNamespaceMap map;
    map.emplace(kViewNss,
                ResolvedNamespace(kViewNss,
                                  kBackingNss,
                                  std::vector<BSONObj>{BSON("$match" << BSON("x" << 1))},
                                  BSONObj{},
                                  opts));

    liteParsed->bindResolvedNamespace(ResolvedNamespace{}, map);

    ASSERT_EQ(1ul, liteParsed->getMutableSubPipelines()->size());
}

TEST_F(DocumentSourceGraphLookUpTest, LiteParsedGraphLookupBindResolvedNamespaceNoOpForNonView) {
    auto liteParsed = parseLiteGraphLookup(getExpCtx());

    // Map exists but marks the namespace as NOT a view.
    ResolvedNamespaceMap map;
    map.emplace(
        kForeignNs,
        ResolvedNamespace(
            kForeignNs, std::vector<BSONObj>{}, boost::none, false /*involvedNamespaceIsAView*/));

    liteParsed->bindResolvedNamespace(ResolvedNamespace{}, map);

    ASSERT_TRUE(liteParsed->getMutableSubPipelines()->empty());
}

TEST_F(DocumentSourceGraphLookUpTest,
       LiteParsedGraphLookupPopulatesSubPipelineFromInternalFromPipeline) {
    auto spec =
        BSON("$graphLookup" << BSON("from" << "foreign"
                                           << "startWith" << "$a"
                                           << "connectFromField" << "b"
                                           << "connectToField" << "c"
                                           << "as" << "d"
                                           << "$_internalFromPipeline"
                                           << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))));
    auto liteParsed = LiteParsedGraphLookUp::parse(
        getExpCtx()->getNamespaceString(), spec.firstElement(), LiteParserOptions{});
    ASSERT_EQ(1ul, liteParsed->getMutableSubPipelines()->size());
}

TEST_F(DocumentSourceGraphLookUpTest, LiteParsedGraphLookupRejectsNonArrayInternalFromPipeline) {
    auto spec = BSON("$graphLookup" << BSON("from" << "foreign"
                                                   << "startWith" << "$a"
                                                   << "connectFromField" << "b"
                                                   << "connectToField" << "c"
                                                   << "as" << "d"
                                                   << "$_internalFromPipeline" << "notAnArray"));
    ASSERT_THROWS_CODE(LiteParsedGraphLookUp::parse(getExpCtx()->getNamespaceString(),
                                                    spec.firstElement(),
                                                    LiteParserOptions{}),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceGraphLookUpTest, LiteParsedGraphLookupRejectsDuplicateInternalFromPipeline) {
    auto spec =
        BSON("$graphLookup" << BSON("from" << "foreign"
                                           << "startWith" << "$a"
                                           << "connectFromField" << "b"
                                           << "connectToField" << "c"
                                           << "as" << "d"
                                           << "$_internalFromPipeline"
                                           << BSON_ARRAY(BSON("$match" << BSON("x" << 1)))
                                           << "$_internalFromPipeline"
                                           << BSON_ARRAY(BSON("$match" << BSON("y" << 2)))));
    ASSERT_THROWS_CODE(LiteParsedGraphLookUp::parse(getExpCtx()->getNamespaceString(),
                                                    spec.firstElement(),
                                                    LiteParserOptions{}),
                       AssertionException,
                       ErrorCodes::IDLDuplicateField);
}

TEST_F(DocumentSourceGraphLookUpTest, GraphLookUpStageParamsCarriesPipelineFromSubpipeline) {
    auto liteParsed = parseLiteGraphLookup(getExpCtx(), kViewName);

    ResolvedNamespaceViewOptions opts;
    opts.involvedNamespaceIsAView = true;
    opts.shouldParseLpp = true;
    ResolvedNamespaceMap map;
    map.emplace(kViewNss,
                ResolvedNamespace(kViewNss,
                                  kBackingNss,
                                  std::vector<BSONObj>{BSON("$match" << BSON("x" << 1))},
                                  BSONObj{},
                                  opts));

    liteParsed->bindResolvedNamespace(ResolvedNamespace{}, map);
    ASSERT_EQ(1ul, liteParsed->getMutableSubPipelines()->size());

    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<GraphLookUpStageParams*>(stageParams.get());
    ASSERT_TRUE(typedParams != nullptr);
    ASSERT_EQ(1ul, typedParams->liteParsedPipeline.value()->getStages().size());
}

TEST_F(DocumentSourceGraphLookUpTest, CreateFromStageParamsUsesLiteParsedPipelineForFromPipeline) {
    // Keep stageSpec alive for the entire test — BSONElements in the stage params alias into
    // its buffer, and createFromStageParams reads them when parsing expressions.
    auto stageSpec = BSON("$graphLookup" << BSON("from" << "foreign"
                                                        << "startWith" << "$a"
                                                        << "connectFromField"
                                                        << "b"
                                                        << "connectToField"
                                                        << "c"
                                                        << "as"
                                                        << "d"));
    auto liteParsed = LiteParsedGraphLookUp::parse(
        getExpCtx()->getNamespaceString(), stageSpec.firstElement(), LiteParserOptions{});

    // Bind a view so that getStageParams() produces a liteParsedPipeline with [{$match:{x:1}}].
    ResolvedNamespaceViewOptions viewOpts;
    viewOpts.involvedNamespaceIsAView = true;
    viewOpts.shouldParseLpp = true;
    ResolvedNamespaceMap bindMap;
    bindMap.emplace(kForeignNs,
                    ResolvedNamespace(kForeignNs,
                                      kBackingNss,
                                      std::vector<BSONObj>{BSON("$match" << BSON("x" << 1))},
                                      BSONObj{},
                                      viewOpts));
    liteParsed->bindResolvedNamespace(ResolvedNamespace{}, bindMap);
    ASSERT_EQ(1ul, liteParsed->getMutableSubPipelines()->size());

    auto stageParams = liteParsed->getStageParams();
    auto* typedParams = dynamic_cast<GraphLookUpStageParams*>(stageParams.get());
    ASSERT_EQ(1ul, typedParams->liteParsedPipeline.value()->getStages().size());

    // Set up expCtx with kForeignNs resolved to a simple (non-view) backing namespace.
    // Its pipeline is empty so that any pipeline in the result must come from liteParsedPipeline.
    auto expCtx = getExpCtx();
    ResolvedNamespaceMap resolvedNss;
    resolvedNss.emplace(kForeignNs, ResolvedNamespace(kForeignNs, {}, boost::none, false));
    expCtx->setResolvedNamespaces(std::move(resolvedNss));

    auto ds = DocumentSourceGraphLookUp::createFromStageParams(*typedParams, expCtx);
    auto* glu = static_cast<DocumentSourceGraphLookUp*>(ds.get());

    query_shape::SerializationOptions serOpts;
    serOpts.isSerializingForRemoteDispatch = true;
    std::vector<Value> serialized;
    glu->serializeToArray(serialized, serOpts);
    ASSERT_EQ(1ul, serialized.size());

    // $_internalFromPipeline must carry exactly the one view stage.
    BSONObj glBson = serialized[0].getDocument().toBson();
    auto stages = glBson["$graphLookup"]["$_internalFromPipeline"].Array();
    ASSERT_EQ(1ul, stages.size());
    ASSERT_BSONOBJ_EQ(stages[0].Obj(), fromjson("{$match: {x: 1}}"));
}

// When featureFlagExtensionsInsideHybridSearch is disabled, serialize() must NOT emit
// $_internalFromPipeline.
TEST_F(DocumentSourceGraphLookUpTest,
       SerializeOmitsInternalFromPipelineWhenExtensionsFlagDisabled) {
    auto glBson = serializeGraphLookupWithResolvedView(getExpCtx(), {.flagEnabled = false});

    // $_internalFromPipeline must be absent because the flag is disabled.
    ASSERT_FALSE(glBson["$graphLookup"].Obj().hasField("$_internalFromPipeline"));

    // Because the pipeline is suppressed, the receiver must re-resolve the view itself. That
    // requires the view name ('foreignView'), not the backing collection ('actualColl'). Sending
    // the backing namespace here would leave the receiver with a plain collection and no pipeline,
    // silently dropping the view's [{$match:{x:1}}] stage. See SERVER-134439.
    ASSERT_EQ(glBson["$graphLookup"]["from"].String(), kViewName);
}

// Symmetric counterpart to the flag-disabled test: when featureFlagExtensionsInsideHybridSearch is
// enabled, serialize() for remote dispatch rewrites 'from' to the backing collection AND carries
// $_internalFromPipeline. The receiver then reconstructs the view pipeline from
// $_internalFromPipeline without re-resolving the view.
TEST_F(DocumentSourceGraphLookUpTest,
       SerializeSendsBackingNsAndInternalFromPipelineWhenExtensionsFlagEnabled) {
    auto glBson = serializeGraphLookupWithResolvedView(getExpCtx(), {.flagEnabled = true});

    // 'from' must be the backing collection so the receiver does not try to re-resolve the view.
    ASSERT_EQ(glBson["$graphLookup"]["from"].String(), kBackingCollName);

    // $_internalFromPipeline must carry exactly the one view stage.
    auto stages = glBson["$graphLookup"]["$_internalFromPipeline"].Array();
    ASSERT_EQ(1ul, stages.size());
    ASSERT_BSONOBJ_EQ(stages[0].Obj(), fromjson("{$match: {x: 1}}"));
}

// When serializing for explain (even if on router or serializing for remote dispatch),
// $_internalFromPipeline must be omitted and 'from' must remain the view namespace.
TEST_F(DocumentSourceGraphLookUpTest, SerializeOmitsInternalFieldsAndPreservesViewNameForExplain) {
    query_shape::SerializationOptions serOpts;
    serOpts.verbosity = ExplainOptions::Verbosity::kQueryPlanner;
    serOpts.isSerializingForRemoteDispatch = true;

    // First check when inRouter = false.
    auto glBson = serializeGraphLookupWithResolvedView(
        getExpCtx(), {.flagEnabled = true, .serOpts = serOpts, .inRouter = false});

    ASSERT_EQ(glBson["$graphLookup"]["from"].String(), kViewName);
    ASSERT_FALSE(glBson["$graphLookup"].Obj().hasField("$_internalFromPipeline"));

    // Then again with inRouter = true.
    glBson = serializeGraphLookupWithResolvedView(
        getExpCtx(), {.flagEnabled = true, .serOpts = serOpts, .inRouter = true});

    ASSERT_EQ(glBson["$graphLookup"]["from"].String(), kViewName);
    ASSERT_FALSE(glBson["$graphLookup"].Obj().hasField("$_internalFromPipeline"));
}

TEST_F(DocumentSourceGraphLookUpTest,
       SerializeOmitsInternalFieldsAndPreservesViewNameForRepresentativeQueryShape) {
    // When shapifying (e.g. for $queryStats or plan cache keys), internal fields such as
    // $_internalFromPipeline must not be serialized, and 'from' must remain the view namespace.
    auto glBson = serializeGraphLookupWithResolvedView(
        getExpCtx(),
        {.flagEnabled = true,
         .serOpts = query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
         .inRouter = true});

    ASSERT_EQ(glBson["$graphLookup"]["from"].String(), kViewName);
    ASSERT_FALSE(glBson["$graphLookup"].Obj().hasField("$_internalFromPipeline"));
}

TEST_F(DocumentSourceGraphLookUpTest, SerializeDefaultOmitsInternalFieldsAndPreservesViewName) {
    // Default local serialization (non-remote dispatch, not on router) should never emit
    // $_internalFromPipeline and should serialize 'from' as the view name.
    auto glBson = serializeGraphLookupWithResolvedView(
        getExpCtx(),
        {.flagEnabled = true, .serOpts = query_shape::SerializationOptions{}, .inRouter = false});

    ASSERT_EQ(glBson["$graphLookup"]["from"].String(), kViewName);
    ASSERT_FALSE(glBson["$graphLookup"].Obj().hasField("$_internalFromPipeline"));
}

}  // namespace
}  // namespace mongo
