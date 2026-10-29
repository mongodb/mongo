// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

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

    Value evaluate(const Document&, Variables*, const EvaluationContext& ctx) const final {
        ++gEvaluations;
        if (ctx.tracker != nullptr) {
            ++gEvaluationsWithTracker;
            gLastTrackerMaxBytes = ctx.tracker->maxAllowedMemoryUsageBytes(
                getExpressionContext()->getOperationContext());
        }
        gLastStageName = ctx.stageName;
        return Value(true);
    }

    Value serialize(const query_shape::SerializationOptions&) const final {
        return Value(Document{});
    }
    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<MemoryTrackerObservingExpression>(&expCtx);
    }
    void acceptVisitor(ExpressionMutableVisitor*) final {}
    void acceptVisitor(ExpressionConstVisitor*) const final {}
};

REGISTER_TEST_EXPRESSION(_testMatchTrackerObserver,
                         MemoryTrackerObservingExpression::parse,
                         AllowedWithApiStrict::kAlways,
                         AllowedWithClientType::kAny,
                         nullptr /* featureFlag */);

struct MatchTestResult {
    boost::intrusive_ptr<exec::agg::Stage> stage;
    boost::intrusive_ptr<exec::agg::MockStage> source;  // must outlive stage
};

MatchTestResult runMatchWithObservingExpression(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    MemoryTrackerObservingExpression::resetObservations();

    auto matchDS = DocumentSourceMatch::create(
        BSON("$expr" << BSON("$_testMatchTrackerObserver" << BSONObj())), expCtx);
    auto stage = exec::agg::buildStage(matchDS);
    auto mockSource = exec::agg::MockStage::createForTest({Document{{"_id", 0}}}, expCtx);
    exec::agg::MockStage::setSource_forTest(stage, mockSource.get());
    while (stage->getNext().isAdvanced()) {
    }
    return {stage, mockSource};
}

MatchTestResult makeMatchOverDocs(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  const std::vector<Document>& docs) {
    auto matchDS = DocumentSourceMatch::create(BSON("_id" << BSON("$gte" << 0)), expCtx);
    auto stage = exec::agg::buildStage(matchDS);
    auto mockSource = exec::agg::MockStage::createForTest(docs, expCtx);
    exec::agg::MockStage::setSource_forTest(stage, mockSource.get());
    return {stage, mockSource};
}

int64_t operationInUseBytes(OperationContext* opCtx) {
    auto* tracker = OperationMemoryUsageTracker::getIfExists(opCtx);
    return tracker ? tracker->inUseTrackedMemoryBytes() : 0;
}

using MatchStageTest = AggregationContextFixture;

TEST_F(MatchStageTest, ThreadsMemoryTrackerWhenEvaluatingExpressions) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runMatchWithObservingExpression(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluations, 1);
    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 1);
}

TEST_F(MatchStageTest, DoesNotThreadMemoryTrackerWhenExpressionMemoryTrackingDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", false);

    runMatchWithObservingExpression(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluations, 1);
    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 0);
}

TEST_F(MatchStageTest, MemoryTrackerHasNoPerStageLimit) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runMatchWithObservingExpression(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 1);
    ASSERT_EQ(MemoryTrackerObservingExpression::gLastTrackerMaxBytes,
              std::numeric_limits<int64_t>::max());
}

TEST_F(MatchStageTest, StageNameIsSetInEvaluationContext) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runMatchWithObservingExpression(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gLastStageName, "$match");
}

TEST_F(MatchStageTest, ExplainOutputIncludesExpressionEvaluationPeakMemoryBytesWhenEnabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    auto [stage, source] = runMatchWithObservingExpression(getExpCtx());

    auto explain = stage->getExplainOutput();
    ASSERT(!explain.getNestedField("expressionEvaluationPeakMemoryBytes").missing());
}

TEST_F(MatchStageTest, ExplainOutputOmitsExpressionEvaluationPeakMemoryBytesWhenDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", false);

    auto [stage, source] = runMatchWithObservingExpression(getExpCtx());

    auto explain = stage->getExplainOutput();
    ASSERT(explain.getNestedField("expressionEvaluationPeakMemoryBytes").missing());
}

TEST_F(MatchStageTest, DetachReleasesBufferMemoryAndNextGetNextChargesItAgain) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    auto [stage, source] =
        makeMatchOverDocs(getExpCtx(), {Document{{"_id", 0}}, Document{{"_id", 1}}});

    ASSERT(stage->getNext().isAdvanced());
    const int64_t chargedBytes = operationInUseBytes(getOpCtx());
    ASSERT_GT(chargedBytes, 0);

    stage->detachFromOperationContext();
    ASSERT_EQ(operationInUseBytes(getOpCtx()), 0);

    stage->reattachToOperationContext(getOpCtx());
    ASSERT(stage->getNext().isAdvanced());
    ASSERT_EQ(operationInUseBytes(getOpCtx()), chargedBytes);
}

TEST_F(MatchStageTest, DisposeReleasesBufferMemory) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    auto [stage, source] = makeMatchOverDocs(getExpCtx(), {Document{{"_id", 0}}});

    ASSERT(stage->getNext().isAdvanced());
    ASSERT_GT(operationInUseBytes(getOpCtx()), 0);

    stage->dispose();
    ASSERT_EQ(operationInUseBytes(getOpCtx()), 0);
}

TEST_F(MatchStageTest, ExprOperatorCanEvaluateMetaExpression) {
    auto docWithStreamMeta = [](auto docarg, auto sortKey) {
        MutableDocument doc{Document(docarg)};
        doc.metadata().setSortKey(Value(sortKey), true);
        return doc.freeze();
    };

    const BSONObj expr =
        fromjson(R"({ $expr: { $eq: [{ $meta: "sortKey" }, [{"source": "foo"}] ] } })");
    auto matcher = DocumentSourceMatch::create(expr, getExpCtx());
    auto matchStage = exec::agg::buildStage(matcher);
    const auto docs = std::vector<Document>{
        docWithStreamMeta(fromjson(R"({"_id": 0})"), fromjson(R"({"source": "foo"})")),
        docWithStreamMeta(fromjson(R"({"_id": 1})"), fromjson(R"({"source": "bar"})")),
        docWithStreamMeta(fromjson(R"({"_id": 2})"), fromjson(R"({"source": "foo"})"))};

    auto mockSource = exec::agg::MockStage::createForTest(docs, getExpCtx());
    exec::agg::MockStage::setSource_forTest(matchStage, mockSource.get());

    std::vector<Document> outDocs;
    for (auto r = matchStage->getNext(); !r.isEOF(); r = matchStage->getNext()) {
        if (r.isAdvanced()) {
            outDocs.push_back(r.releaseDocument());
        }
    }

    const std::vector<Document> expectDocs = {docs[0], docs[2]};
    ASSERT_DOCUMENTS_EQ(expectDocs, outDocs);
}

}  // namespace
}  // namespace mongo
