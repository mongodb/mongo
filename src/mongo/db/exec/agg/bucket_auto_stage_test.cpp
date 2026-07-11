// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/bucket_auto_stage.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

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
                                                  BSONElement,
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
        return Value(0);
    }

    Value serialize(const query_shape::SerializationOptions&) const final {
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

struct BucketAutoTestResult {
    boost::intrusive_ptr<exec::agg::BucketAutoStage> stage;
    boost::intrusive_ptr<exec::agg::MockStage> source;  // must outlive stage
};

BucketAutoTestResult runBucketAutoWithObservingGroupBy(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    MemoryTrackerObservingExpression::resetObservations();

    auto observerExpr = make_intrusive<MemoryTrackerObservingExpression>(expCtx.get());
    auto accumulatedFields = std::make_shared<std::vector<AccumulationStatement>>();
    auto populated = std::make_shared<bool>(false);
    auto stage = make_intrusive<exec::agg::BucketAutoStage>("$bucketAuto",
                                                            expCtx,
                                                            accumulatedFields,
                                                            populated,
                                                            std::move(observerExpr),
                                                            nullptr /* granularityRounder */,
                                                            2 /* nBuckets */);

    auto source = exec::agg::MockStage::createForTest(
        {Document{{"_id", 0}}, Document{{"_id", 1}}, Document{{"_id", 2}}}, expCtx);
    exec::agg::MockStage::setSource_forTest(stage, source.get());
    while (stage->getNext().isAdvanced()) {
    }
    return {stage, source};
}

using BucketAutoStageTest = AggregationContextFixture;

TEST_F(BucketAutoStageTest, ThreadsMemoryTrackerWhenEvaluatingGroupByExpression) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runBucketAutoWithObservingGroupBy(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluations, 3);
    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 3);
}

TEST_F(BucketAutoStageTest, DoesNotThreadMemoryTrackerWhenExpressionMemoryTrackingDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", false);

    runBucketAutoWithObservingGroupBy(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluations, 3);
    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 0);
}

TEST_F(BucketAutoStageTest, MemoryTrackerLimitReflectsKnob) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);
    const long long customLimit = 2 * 1024 * 1024;
    unittest::ServerParameterGuard knobGuard("internalDocumentSourceBucketAutoMaxMemoryBytes",
                                             customLimit);

    runBucketAutoWithObservingGroupBy(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 3);
    ASSERT_EQ(MemoryTrackerObservingExpression::gLastTrackerMaxBytes, customLimit);
}

TEST_F(BucketAutoStageTest, StageNameIsSetInEvaluationContext) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    auto [stage, source] = runBucketAutoWithObservingGroupBy(getExpCtx());

    ASSERT_EQ(MemoryTrackerObservingExpression::gLastStageName, "$bucketAuto");
}

TEST_F(BucketAutoStageTest, ExplainOutputIncludesExpressionEvaluationPeakMemoryBytesWhenEnabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    auto [stage, source] = runBucketAutoWithObservingGroupBy(getExpCtx());

    auto explain = stage->getExplainOutput();
    ASSERT(!explain.getNestedField("expressionEvaluationPeakMemoryBytes").missing());
}

TEST_F(BucketAutoStageTest, ExplainOutputOmitsExpressionEvaluationPeakMemoryBytesWhenDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", false);

    auto [stage, source] = runBucketAutoWithObservingGroupBy(getExpCtx());

    auto explain = stage->getExplainOutput();
    ASSERT(explain.getNestedField("expressionEvaluationPeakMemoryBytes").missing());
}

}  // namespace
}  // namespace mongo
