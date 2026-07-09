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

#include "mongo/db/pipeline/document_source_redact.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <memory>


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceRedactTest = AggregationContextFixture;

TEST_F(DocumentSourceRedactTest, ShouldCopyRedactSafePartOfMatchBeforeItself) {
    BSONObj redactSpec = BSON("$redact" << "$$PRUNE");
    auto redact = DocumentSourceRedact::createFromBson(redactSpec.firstElement(), getExpCtx());
    auto match = DocumentSourceMatch::create(BSON("a" << 1), getExpCtx());

    DocumentSourceContainer pipeline;
    pipeline.push_back(redact);
    pipeline.push_back(match);

    checked_cast<DocumentSourceRedact&>(*pipeline.front().get())
        .optimizeAt(pipeline.begin(), &pipeline);

    ASSERT_EQUALS(pipeline.size(), 3U);
    ASSERT(dynamic_cast<DocumentSourceMatch*>(pipeline.front().get()));
}

TEST_F(DocumentSourceRedactTest, ShouldPropagatePauses) {
    auto redactSpec = BSON("$redact" << "$$KEEP");
    auto redact = DocumentSourceRedact::createFromBson(redactSpec.firstElement(), getExpCtx());
    auto mock =
        exec::agg::MockStage::createForTest({Document{{"_id", 0}},
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document{{"_id", 1}},
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            getExpCtx());
    auto redactStage = exec::agg::buildStageAndStitch(redact, mock);

    // The $redact is keeping everything, so we should see everything from the mock, then EOF.
    ASSERT_TRUE(redactStage->getNext().isAdvanced());
    ASSERT_TRUE(redactStage->getNext().isPaused());
    ASSERT_TRUE(redactStage->getNext().isAdvanced());
    ASSERT_TRUE(redactStage->getNext().isPaused());
    ASSERT_TRUE(redactStage->getNext().isEOF());
    ASSERT_TRUE(redactStage->getNext().isEOF());
}

// Test-only expression, registered as $_testRedactMemoryTrackerObserver, used as a $redact
// expression. It records whether the evaluation context carried a memory tracker and always
// returns the "$$KEEP" sentinel so the document is passed through unchanged.
class RedactMemoryTrackerObservingExpression final : public Expression {
public:
    static inline int gEvaluations = 0;
    static inline int gEvaluationsWithTracker = 0;
    static inline int64_t gLastTrackerMaxBytes = -1;
    static void resetObservations() {
        gEvaluations = 0;
        gEvaluationsWithTracker = 0;
        gLastTrackerMaxBytes = -1;
    }

    explicit RedactMemoryTrackerObservingExpression(ExpressionContext* expCtx)
        : Expression(expCtx) {}

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState&) {
        return make_intrusive<RedactMemoryTrackerObservingExpression>(expCtx);
    }

    Value evaluate(const Document&, Variables*, const EvaluationContext& ctx) const final {
        ++gEvaluations;
        if (ctx.tracker != nullptr) {
            ++gEvaluationsWithTracker;
            gLastTrackerMaxBytes = ctx.tracker->maxAllowedMemoryUsageBytes();
        }
        return Value("keep"sv);
    }

    Value serialize(const query_shape::SerializationOptions&) const final {
        return Value(Document{});
    }
    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<RedactMemoryTrackerObservingExpression>(&expCtx);
    }
    void acceptVisitor(ExpressionMutableVisitor*) final {}
    void acceptVisitor(ExpressionConstVisitor*) const final {}
};

REGISTER_TEST_EXPRESSION(_testRedactMemoryTrackerObserver,
                         RedactMemoryTrackerObservingExpression::parse,
                         AllowedWithApiStrict::kAlways,
                         AllowedWithClientType::kAny,
                         nullptr /* featureFlag */);

// Runs a $redact whose expression is the tracker-observing expression over a single document,
// resetting the observation counters first.
void runRedactWithObservingExpression(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    RedactMemoryTrackerObservingExpression::resetObservations();

    auto redactSpec = BSON("$redact" << BSON("$_testRedactMemoryTrackerObserver" << BSONObj()));
    auto redact = DocumentSourceRedact::createFromBson(redactSpec.firstElement(), expCtx);

    auto mock = exec::agg::MockStage::createForTest({Document{{"_id", 0}, {"a", 1}}}, expCtx);
    auto redactStage = exec::agg::buildStageAndStitch(redact, mock);
    while (redactStage->getNext().isAdvanced()) {
    }
}

TEST_F(DocumentSourceRedactTest, ThreadsMemoryTrackerWhenEvaluatingExpression) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runRedactWithObservingExpression(getExpCtx());

    ASSERT_EQ(RedactMemoryTrackerObservingExpression::gEvaluations, 1);
    ASSERT_EQ(RedactMemoryTrackerObservingExpression::gEvaluationsWithTracker, 1);
}

TEST_F(DocumentSourceRedactTest, DoesNotThreadMemoryTrackerWhenExpressionMemoryTrackingDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", false);

    runRedactWithObservingExpression(getExpCtx());

    ASSERT_EQ(RedactMemoryTrackerObservingExpression::gEvaluations, 1);
    ASSERT_EQ(RedactMemoryTrackerObservingExpression::gEvaluationsWithTracker, 0);
}

TEST_F(DocumentSourceRedactTest, DoesNotThreadMemoryTrackerWhenQueryMemoryTrackingDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", false);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runRedactWithObservingExpression(getExpCtx());

    ASSERT_EQ(RedactMemoryTrackerObservingExpression::gEvaluations, 1);
    ASSERT_EQ(RedactMemoryTrackerObservingExpression::gEvaluationsWithTracker, 0);
}

TEST_F(DocumentSourceRedactTest, MemoryTrackerHasNoPerStageLimit) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runRedactWithObservingExpression(getExpCtx());

    ASSERT_EQ(RedactMemoryTrackerObservingExpression::gEvaluationsWithTracker, 1);
    ASSERT_EQ(RedactMemoryTrackerObservingExpression::gLastTrackerMaxBytes,
              std::numeric_limits<int64_t>::max());
}
}  // namespace
}  // namespace mongo
