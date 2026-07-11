// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
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
            gLastTrackerMaxBytes = ctx.tracker->maxAllowedMemoryUsageBytes();
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

}  // namespace
}  // namespace mongo
