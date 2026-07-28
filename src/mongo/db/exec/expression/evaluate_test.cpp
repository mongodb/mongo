// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/exec/expression/evaluate.h"

#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"

namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

using boost::intrusive_ptr;

/* ------------------------ Constant -------------------- */

TEST(ExpressionConstantTest, Create) {
    /** Create an ExpressionConstant from a Value. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value(5));
    ASSERT_BSONOBJ_BINARY_EQ(BSON("" << 5), toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionConstantTest, CreateFromBsonElement) {
    /** Create an ExpressionConstant from a BsonElement. */
    BSONObj spec = BSON("IGNORED_FIELD_NAME" << "foo");
    auto expCtx = ExpressionContextForTest{};
    BSONElement specElement = spec.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    intrusive_ptr<Expression> expression = ExpressionConstant::parse(&expCtx, specElement, vps);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("" << "foo"),
                             toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionConstantTest, ConstantOfValueMissingRemovesField) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value());
    ASSERT_BSONOBJ_BINARY_EQ(
        BSONObj(),
        toBson(expression->evaluate(Document{{"foo", Value("bar"sv)}}, &expCtx.variables, {})));
}

namespace BuiltinRemoveVariable {

TEST(BuiltinRemoveVariableTest, TypeOfRemoveIsMissing) {
    assertExpectedResults("$type", {{{Value("$$REMOVE"sv)}, Value("missing"sv)}});
}

TEST(BuiltinRemoveVariableTest, LiteralEscapesRemoveVar) {
    assertExpectedResults(
        "$literal", {{{Value("$$REMOVE"sv)}, Value(std::vector<Value>{Value("$$REMOVE"sv)})}});
}

}  // namespace BuiltinRemoveVariable

namespace NowAndClusterTime {
TEST(NowAndClusterTime, BasicTest) {
    auto expCtx = ExpressionContextForTest{};

    // $$NOW is the Date type.
    {
        auto expression = ExpressionFieldPath::parse(&expCtx, "$$NOW", expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables, {});
        ASSERT_EQ(result.getType(), BSONType::date);
    }
    // $$CLUSTER_TIME is the timestamp type.
    {
        auto expression =
            ExpressionFieldPath::parse(&expCtx, "$$CLUSTER_TIME", expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables, {});
        ASSERT_EQ(result.getType(), BSONType::timestamp);
    }

    // Multiple references to $$NOW must return the same value.
    {
        auto expression = Expression::parseExpression(
            &expCtx, fromjson("{$eq: [\"$$NOW\", \"$$NOW\"]}"), expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables, {});

        ASSERT_VALUE_EQ(result, Value{true});
    }
    // Same is true for the $$CLUSTER_TIME.
    {
        auto expression =
            Expression::parseExpression(&expCtx,
                                        fromjson("{$eq: [\"$$CLUSTER_TIME\", \"$$CLUSTER_TIME\"]}"),
                                        expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables, {});

        ASSERT_VALUE_EQ(result, Value{true});
    }
}
}  // namespace NowAndClusterTime

/* ------------------------ expressionMemoryFlushThresholdBytes --------------- */

TEST(ExpressionMemoryFlushThresholdBytesTest, ClampsToMaxWhenFarFromLimit) {
    // Remaining budget is enormous, so the threshold should clamp to the 1MB cap rather than
    // scale up with it.
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{10LL * 1024 * 1024 * 1024}};
    ASSERT_EQ(exec::expression::expressionMemoryFlushThresholdBytes(tracker, nullptr), 1024 * 1024);
}

TEST(ExpressionMemoryFlushThresholdBytesTest, ClampsToMinWhenLimitIsSmall) {
    // remaining / 256 would be well under 1KB here, so the floor kicks in.
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{1000}};
    ASSERT_EQ(exec::expression::expressionMemoryFlushThresholdBytes(tracker, nullptr), 1024);
}

TEST(ExpressionMemoryFlushThresholdBytesTest, ScalesDownAsUsageApproachesLimit) {
    // 2MB limit: remaining / 256 stays comfortably between the 1KB floor and 1MB cap across the
    // range exercised below, so the threshold tracks 'remaining' directly and we can observe it
    // shrink as usage grows.
    const int64_t limit = 2 * 1024 * 1024;
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{limit}};

    ASSERT_EQ(exec::expression::expressionMemoryFlushThresholdBytes(tracker, nullptr), 8192);

    tracker.add(limit - 512 * 1024);  // 512KB remaining.
    ASSERT_EQ(exec::expression::expressionMemoryFlushThresholdBytes(tracker, nullptr), 2048);

    // 1000 bytes remaining: below the floor's break-even point, so the 1KB floor clamps it back
    // up rather than shrinking further.
    tracker.add(512 * 1024 - 1000);
    ASSERT_EQ(exec::expression::expressionMemoryFlushThresholdBytes(tracker, nullptr), 1024);
}

TEST(ExpressionMemoryFlushThresholdBytesTest, ReturnsZeroWhenAlreadyAtLimit) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(100);
    ASSERT_EQ(exec::expression::expressionMemoryFlushThresholdBytes(tracker, nullptr), 0);
}

TEST(ExpressionMemoryFlushThresholdBytesTest, ReturnsZeroWhenOverLimit) {
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(150);
    ASSERT_EQ(exec::expression::expressionMemoryFlushThresholdBytes(tracker, nullptr), 0);
}

TEST(ExpressionMemoryFlushThresholdBytesTest, ReturnsZeroWhenAncestorIsAtLimitEvenIfLocalIsNot) {
    // Local tracker has plenty of headroom, but the ancestor it is chained to does not. The
    // threshold must reflect the binding (ancestor) constraint, not the local tracker's own.
    SimpleMemoryUsageTracker opTracker{MemoryUsageLimit{100}};
    opTracker.add(100);
    SimpleMemoryUsageTracker stageTracker{&opTracker, MemoryUsageLimit{10 * 1024 * 1024}};
    ASSERT_EQ(exec::expression::expressionMemoryFlushThresholdBytes(stageTracker, nullptr), 0);
}

/* ------------------------ BatchedExpressionMemoryCharger -------------------- */

namespace {
// A 2MB limit puts the flush threshold at 8192 bytes (limit / 256, between the 1KB floor and the
// 1MB cap), which the tests below use to stay deliberately under or over one batch.
constexpr int64_t kTestLimit = 2 * 1024 * 1024;
constexpr int64_t kTestThreshold = 8192;
}  // namespace

TEST(BatchedExpressionMemoryChargerTest, AccumulatesBelowThresholdAndChargesOnFlush) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionConstant::create(&expCtx, Value(5));
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{kTestLimit}};
    EvaluationContext ctx{.tracker = &tracker};

    exec::expression::BatchedExpressionMemoryCharger memCharger(*expr, ctx);
    memCharger.add(100);
    memCharger.add(100);
    memCharger.add(100);
    // Still under one batch, so nothing has reached the tracker yet.
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);

    memCharger.flush();
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 300);
}

TEST(BatchedExpressionMemoryChargerTest, AutoFlushesWhenThresholdCrossedAndDoesNotDoubleCharge) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionConstant::create(&expCtx, Value(5));
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{kTestLimit}};
    EvaluationContext ctx{.tracker = &tracker};

    exec::expression::BatchedExpressionMemoryCharger memCharger(*expr, ctx);
    memCharger.add(kTestThreshold);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), kTestThreshold);

    // The auto-flush reset the pending batch, so an explicit flush must not charge it again.
    memCharger.flush();
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), kTestThreshold);
}

TEST(BatchedExpressionMemoryChargerTest, SetTotalChargesDeltaRatherThanAccumulating) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionConstant::create(&expCtx, Value(5));
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{kTestLimit}};
    EvaluationContext ctx{.tracker = &tracker};

    exec::expression::BatchedExpressionMemoryCharger memCharger(*expr, ctx);
    memCharger.setTotal(1000);
    memCharger.flush();
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 1000);

    // Usage is the last total reported, not the sum of the totals.
    memCharger.setTotal(1500);
    memCharger.flush();
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 1500);
}

TEST(BatchedExpressionMemoryChargerTest, SetTotalAccountsForPendingBytesNotYetCharged) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionConstant::create(&expCtx, Value(5));
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{kTestLimit}};
    EvaluationContext ctx{.tracker = &tracker};

    exec::expression::BatchedExpressionMemoryCharger memCharger(*expr, ctx);
    // Both stay under the threshold, so the second call has to net against the first while it is
    // still pending rather than against what the tracker has been told.
    memCharger.setTotal(400);
    memCharger.setTotal(900);
    memCharger.flush();
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 900);
}

TEST(BatchedExpressionMemoryChargerTest, SetTotalCanShrinkUsage) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionConstant::create(&expCtx, Value(5));
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{kTestLimit}};
    EvaluationContext ctx{.tracker = &tracker};

    exec::expression::BatchedExpressionMemoryCharger memCharger(*expr, ctx);
    memCharger.setTotal(5000);
    memCharger.flush();
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 5000);

    memCharger.setTotal(1000);
    memCharger.flush();
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 1000);
}

TEST(BatchedExpressionMemoryChargerTest, FlushWithNothingPendingDoesNotThrowWhenOverLimit) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionConstant::create(&expCtx, Value(5));
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{100}};
    tracker.add(150);
    EvaluationContext ctx{.tracker = &tracker};

    // The tracker is already over its limit, but this charger has not charged anything, so there
    // is nothing to check and flush() must stay a no-op.
    exec::expression::BatchedExpressionMemoryCharger memCharger(*expr, ctx);
    memCharger.flush();
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 150);
}

TEST(BatchedExpressionMemoryChargerTest, AddThrowsWhenAutoFlushedChargeExceedsLimit) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionConstant::create(&expCtx, Value(5));
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{4096}};
    EvaluationContext ctx{.tracker = &tracker};

    exec::expression::BatchedExpressionMemoryCharger memCharger(*expr, ctx);
    ASSERT_THROWS_CODE(memCharger.add(5000), AssertionException, ErrorCodes::ExceededMemoryLimit);
}

TEST(BatchedExpressionMemoryChargerTest, FlushThrowsWhenSubThresholdChargeExceedsLimit) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionConstant::create(&expCtx, Value(5));
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{500}};
    EvaluationContext ctx{.tracker = &tracker};

    exec::expression::BatchedExpressionMemoryCharger memCharger(*expr, ctx);
    memCharger.add(600);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_THROWS_CODE(memCharger.flush(), AssertionException, ErrorCodes::ExceededMemoryLimit);
}

TEST(BatchedExpressionMemoryChargerTest, ReleasesChargedMemoryOnDestruction) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionConstant::create(&expCtx, Value(5));
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{kTestLimit}};
    EvaluationContext ctx{.tracker = &tracker};

    {
        exec::expression::BatchedExpressionMemoryCharger memCharger(*expr, ctx);
        memCharger.add(2000);
        memCharger.flush();
        ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 2000);
    }
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_EQ(tracker.peakTrackedMemoryBytes(), 2000);
}

TEST(BatchedExpressionMemoryChargerTest, PendingBytesAreNotChargedWithoutFlush) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = ExpressionConstant::create(&expCtx, Value(5));
    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{kTestLimit}};
    EvaluationContext ctx{.tracker = &tracker};

    {
        // Bytes still pending when the charger dies are never charged at all. This is why every
        // evaluator must flush before returning.
        exec::expression::BatchedExpressionMemoryCharger memCharger(*expr, ctx);
        memCharger.add(2000);
    }
    ASSERT_EQ(tracker.peakTrackedMemoryBytes(), 0);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
