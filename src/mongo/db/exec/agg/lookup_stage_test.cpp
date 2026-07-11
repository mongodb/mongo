// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/lookup_stage.h"

#include "mongo/bson/json.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_lookup_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <deque>
#include <limits>
#include <list>
#include <string_view>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using namespace test;

class LookupStageTest : public AggregationContextFixture {
protected:
    LookupStageTest() {
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

TEST_F(LookupStageTest, ShouldPropagatePauses) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // Mock the input of a foreign namespace, pausing every other result.
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"foreignId", 0}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"foreignId", 1}},
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            expCtx);

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                                  Document{{"_id", 1}}};
    expCtx->setMongoProcessInterface(
        std::make_shared<DocumentSourceLookupMockMongoInterface>(std::move(mockForeignContents)));

    // Set up the $lookup stage.
    auto lookupSpec = Document{{"$lookup",
                                Document{{"from", fromNs.coll()},
                                         {"localField", "foreignId"sv},
                                         {"foreignField", "_id"sv},
                                         {"as", "foreignDocs"sv}}}}
                          .toBson();
    auto lookup = makeLookUpFromBson(lookupSpec.firstElement(), expCtx);
    auto lookupStage = exec::agg::buildStageAndStitch(lookup, mockLocalStage);

    auto next = lookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 0}, {"foreignDocs", {Document{{"_id", 0}}}}}));

    ASSERT_TRUE(lookupStage->getNext().isPaused());

    next = lookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 1}, {"foreignDocs", {Document{{"_id", 1}}}}}));

    ASSERT_TRUE(lookupStage->getNext().isPaused());

    ASSERT_TRUE(lookupStage->getNext().isEOF());
    ASSERT_TRUE(lookupStage->getNext().isEOF());
}

TEST_F(LookupStageTest, ShouldPropagatePausesWhileUnwinding) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "foreign");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // Mock out the foreign collection.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"_id", 0}},
                                                                  Document{{"_id", 1}}};
    expCtx->setMongoProcessInterface(
        std::make_shared<DocumentSourceLookupMockMongoInterface>(std::move(mockForeignContents)));

    // Mock its input, pausing every other result.
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"foreignId", 0}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"foreignId", 1}},
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            expCtx);

    // Set up the $lookup stage.
    auto lookupSpec = Document{{"$lookup",
                                Document{{"from", fromNs.coll()},
                                         {"localField", "foreignId"sv},
                                         {"foreignField", "_id"sv},
                                         {"as", "foreignDoc"sv}}}}
                          .toBson();
    auto lookup = makeLookUpFromBson(lookupSpec.firstElement(), expCtx);

    const bool preserveNullAndEmptyArrays = false;
    const boost::optional<std::string> includeArrayIndex = boost::none;
    lookup->setUnwindStage_forTest(DocumentSourceUnwind::create(
        expCtx, "foreignDoc", preserveNullAndEmptyArrays, includeArrayIndex));

    auto lookupStage = exec::agg::buildStageAndStitch(lookup.get(), mockLocalStage);

    auto next = lookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 0}, {"foreignDoc", Document{{"_id", 0}}}}));

    ASSERT_TRUE(lookupStage->getNext().isPaused());

    next = lookupStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(),
                       (Document{{"foreignId", 1}, {"foreignDoc", Document{{"_id", 1}}}}));

    ASSERT_TRUE(lookupStage->getNext().isPaused());

    ASSERT_TRUE(lookupStage->getNext().isEOF());
    ASSERT_TRUE(lookupStage->getNext().isEOF());
}

TEST_F(LookupStageTest, ShouldReplaceNonCorrelatedPrefixWithCacheAfterFirstSubPipelineIteration) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    std::deque<DocumentSource::GetNextResult> mockForeignContents{
        Document{{"x", 0}}, Document{{"x", 1}}, Document{{"x", 2}}};
    expCtx->setMongoProcessInterface(
        std::make_shared<DocumentSourceLookupMockMongoInterface>(mockForeignContents));

    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x: {$gte: 0}}}, {$sort: {x: "
        "1}}, {$addFields: {varField: {$sum: ['$x', '$$var1']}}}], from: 'coll', as: "
        "'as'}}",
        expCtx);

    // Prepare the mocked local stage.
    auto mockLocalStage = exec::agg::MockStage::createForTest(
        {Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}}, expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);
    exec::agg::MockStage::setSource_forTest(lookupStage, mockLocalStage.get());

    // Confirm that the empty 'kBuilding' cache is placed just before the correlated $addFields.
    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 0));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{$mock: {}}, {$match: {x: {$gte: 0}}}, {$sort: {sortKey: {x: 1}}}, "
                      << sequentialCacheStageObj(getExpCtx()->getOperationContext(), "kBuilding")
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
    subPipeline = lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 1));
    ASSERT(subPipeline);

    expectedPipe = fromjson(
        str::stream() << "["
                      << sequentialCacheStageObj(getExpCtx()->getOperationContext(), "kServing")
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

TEST_F(LookupStageTest, ShouldAbandonCacheIfMaxSizeIsExceededAfterFirstSubPipelineIteration) {
    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    std::deque<DocumentSource::GetNextResult> mockForeignContents{Document{{"x", 0}},
                                                                  Document{{"x", 1}}};
    expCtx->setMongoProcessInterface(
        std::make_shared<DocumentSourceLookupMockMongoInterface>(mockForeignContents));

    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: '$_id'}, pipeline: [{$match: {x: {$gte: 0}}}, {$sort: {x: "
        "1}}, {$addFields: {varField: {$sum: ['$x', '$$var1']}}}], from: 'coll', as: "
        "'as'}}",
        expCtx);

    // Prepare the mocked local and foreign sources.
    auto mockLocalStage =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}}, Document{{"_id", 1}}}, expCtx);

    auto lookupStage = buildLookUpStage(lookupDS);
    exec::agg::MockStage::setSource_forTest(lookupStage, mockLocalStage.get());

    // Ensure the cache is abandoned after the first iteration by setting its max size to 0.
    size_t maxCacheSizeBytes = 0;
    lookupStage->reInitializeCache_forTest(maxCacheSizeBytes);

    // Confirm that the empty 'kBuilding' cache is placed just before the correlated $addFields.
    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 0));
    ASSERT(subPipeline);

    auto expectedPipe = fromjson(
        str::stream() << "[{$mock: {}}, {$match: {x: {$gte: 0}}}, {$sort: {sortKey: {x: 1}}}, "
                      << sequentialCacheStageObj(
                             getExpCtx()->getOperationContext(), "kBuilding", 0ll)
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
    subPipeline = lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 1));
    ASSERT(subPipeline);

    expectedPipe = fromjson(
        str::stream() << "[{$mock: {}}, {$match: {x: {$gte: 0}}}, {$sort: {sortKey: {x: 1}}}, "
                         "{$addFields: {varField: {$sum: ['$x', {$const: 1}]}}}]");

    ASSERT_VALUE_EQ(Value(subPipeline->writeExplainOps(kExplain)), Value(BSONArray(expectedPipe)));

    // Verify that the second document is constructed correctly without the cache.
    auto secondResult = lookupStage->getNext();

    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{_id: 1, as: [{x: 0, varField: 1}, {x: 1, varField: 2}]}")},
        secondResult.getDocument());
}

TEST_F(LookupStageTest, AddingCacheStageWorksWithDisablePipelineRewrites) {
    // Disable pipeline rewrites.
    unittest::ServerParameterGuard controller("internalQueryMaxPipelineRewrites", 0);

    auto expCtx = getExpCtx();
    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    // Build lookup stage.
    constexpr auto json = R"(
    {
        $lookup: {
            from: "coll",
            let: {var1: "$_id"},
            pipeline: [
                {$match: {$expr: {$eq: ["$_id", "$$var1"]}}},
                {$project: {y: 1, computed: {$add: ["$y", 1]}}},
                {$sort: {y: 1}}
            ],
            as: "joined"
        }
    }
    )";
    auto lookupDS = makeLookUpFromJson(json, expCtx);
    auto lookupStage = buildLookUpStage(lookupDS);

    // Prepare the mocked local and foreign sources.
    std::deque<DocumentSource::GetNextResult> mockForeignContents{
        Document{{"_id", 1}, {"y", 10}},
        Document{{"_id", 2}, {"y", 20}},
        Document{{"_id", 3}, {"y", 30}},
    };

    expCtx->setMongoProcessInterface(
        std::make_shared<DocumentSourceLookupMockMongoInterface>(mockForeignContents));

    auto mockLocalStage = exec::agg::MockStage::createForTest({Document{{"_id", 1}, {"x", 1}},
                                                               Document{{"_id", 2}, {"x", 2}},
                                                               Document{{"_id", 3}, {"x", 3}}},
                                                              expCtx);

    exec::agg::MockStage::setSource_forTest(lookupStage, mockLocalStage.get());

    // buildPipeline adds the cache stage.
    auto subPipeline =
        lookupStage->buildPipeline(lookupDS->getSubpipelineExpCtx(), DOC("_id" << 1));
    ASSERT(subPipeline);
}

// Test-only expression, registered as $_testMemoryTrackerObserver, used as a $lookup 'let' value.
class MemoryTrackerObservingExpression final : public Expression {
public:
    static inline int gEvaluations = 0;
    static inline int gEvaluationsWithTracker = 0;
    static inline int64_t gLastTrackerMaxBytes = -1;
    static inline std::string_view gLastStageName;
    static void resetObservations() {
        gEvaluations = 0;
        gEvaluationsWithTracker = 0;
        gLastTrackerMaxBytes = -1;
        gLastStageName = std::string_view{};
    }

    explicit MemoryTrackerObservingExpression(ExpressionContext* expCtx) : Expression(expCtx) {}

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState&) {
        return make_intrusive<MemoryTrackerObservingExpression>(expCtx);
    }

    Value evaluate(const Document& root,
                   Variables* variables,
                   const EvaluationContext& ctx) const final {
        ++gEvaluations;
        if (ctx.tracker != nullptr) {
            ++gEvaluationsWithTracker;
            gLastTrackerMaxBytes = ctx.tracker->maxAllowedMemoryUsageBytes(
                getExpressionContext()->getOperationContext());
        }
        gLastStageName = ctx.stageName;
        return Value(1);
    }

    Value serialize(const query_shape::SerializationOptions& options = {}) const final {
        return Value(Document{});
    }
    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<MemoryTrackerObservingExpression>(&expCtx);
    }
    void acceptVisitor(ExpressionMutableVisitor*) final {
        MONGO_UNREACHABLE;
    }
    void acceptVisitor(ExpressionConstVisitor*) const final {
        MONGO_UNREACHABLE;
    }
};

REGISTER_TEST_EXPRESSION(_testMemoryTrackerObserver,
                         MemoryTrackerObservingExpression::parse,
                         AllowedWithApiStrict::kAlways,
                         AllowedWithClientType::kAny,
                         nullptr /* featureFlag */);

struct LookupTestResult {
    boost::intrusive_ptr<exec::agg::Stage> stage;
    boost::intrusive_ptr<exec::agg::MockStage> source;  // must outlive stage
};

// Runs a $lookup over a single local document whose 'let' variable is the tracker-observing
// expression, resetting the observation counters first.
LookupTestResult runLookupWithObservingLetVariable(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    MemoryTrackerObservingExpression::resetObservations();

    NamespaceString fromNs =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    expCtx->setMongoProcessInterface(std::make_shared<DocumentSourceLookupMockMongoInterface>(
        std::deque<DocumentSource::GetNextResult>{Document{{"x", 0}}}));

    auto lookupDS = makeLookUpFromJson(
        "{$lookup: {let: {var1: {$_testMemoryTrackerObserver: {}}}, pipeline: [{$match: {x: {$gte: "
        "0}}}], from: 'coll', as: 'as'}}",
        expCtx);
    auto mockLocalStage = exec::agg::MockStage::createForTest({Document{{"_id", 0}}}, expCtx);
    auto lookupStage = buildLookUpStage(lookupDS);
    exec::agg::MockStage::setSource_forTest(lookupStage, mockLocalStage.get());
    while (lookupStage->getNext().isAdvanced()) {
    }
    return {lookupStage, mockLocalStage};
}

TEST_F(LookupStageTest, ThreadsMemoryTrackerWhenEvaluatingLetVariables) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runLookupWithObservingLetVariable(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluations, 1);
    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 1);
}

TEST_F(LookupStageTest, DoesNotThreadMemoryTrackerWhenExpressionMemoryTrackingDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", false);

    runLookupWithObservingLetVariable(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluations, 1);
    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 0);
}

TEST_F(LookupStageTest, MemoryTrackerHasNoPerStageLimit) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runLookupWithObservingLetVariable(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 1);
    ASSERT_EQ(MemoryTrackerObservingExpression::gLastTrackerMaxBytes,
              std::numeric_limits<int64_t>::max());
}

TEST_F(LookupStageTest, StageNameIsSetInEvaluationContext) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    auto [stage, source] = runLookupWithObservingLetVariable(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gLastStageName, "$lookup");
}

TEST_F(LookupStageTest, ExplainOutputIncludesExpressionEvaluationPeakMemoryBytesWhenEnabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    auto [stage, source] = runLookupWithObservingLetVariable(getExpCtx());

    auto explain = stage->getExplainOutput();
    ASSERT(!explain.getNestedField("expressionEvaluationPeakMemoryBytes").missing());
}

TEST_F(LookupStageTest, ExplainOutputOmitsExpressionEvaluationPeakMemoryBytesWhenDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", false);

    auto [stage, source] = runLookupWithObservingLetVariable(getExpCtx());

    auto explain = stage->getExplainOutput();
    ASSERT(explain.getNestedField("expressionEvaluationPeakMemoryBytes").missing());
}
}  // namespace
}  // namespace mongo
