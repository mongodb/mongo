/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class MemoryTrackerObservingExpression final : public Expression {
public:
    static inline int gEvaluations = 0;
    static inline int gEvaluationsWithTracker = 0;
    static inline int64_t gLastTrackerMaxBytes = -1;
    static inline StringData gLastStageName;
    static void resetObservations() {
        gEvaluations = 0;
        gEvaluationsWithTracker = 0;
        gLastTrackerMaxBytes = -1;
        gLastStageName = StringData{};
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
