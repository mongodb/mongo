// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/memory_usage_limit.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <string>
#include <string_view>
#include <vector>


namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

//
// Evaluation.
//

TEST(ExpressionObjectEvaluate, EmptyObjectShouldEvaluateToEmptyDocument) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(&expCtx, {});
    ASSERT_VALUE_EQ(Value(Document()), object->evaluate(Document(), &(expCtx.variables), {}));
    ASSERT_VALUE_EQ(Value(Document()),
                    object->evaluate(Document{{"a", 1}}, &(expCtx.variables), {}));
    ASSERT_VALUE_EQ(Value(Document()),
                    object->evaluate(Document{{"_id", "ID"sv}}, &(expCtx.variables), {}));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateEachField) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(&expCtx,
                                           {{"a", ExpressionConstant::create(&expCtx, Value{1})},
                                            {"b", ExpressionConstant::create(&expCtx, Value{5})}});


    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}),
                    object->evaluate(Document(), &(expCtx.variables), {}));
    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}),
                    object->evaluate(Document{{"a", 1}}, &(expCtx.variables), {}));
    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}),
                    object->evaluate(Document{{"_id", "ID"sv}}, &(expCtx.variables), {}));
}

TEST(ExpressionObjectEvaluate, OrderOfFieldsInOutputShouldMatchOrderInSpecification) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx,
        {{"a", ExpressionFieldPath::createPathFromString(&expCtx, "a", expCtx.variablesParseState)},
         {"b", ExpressionFieldPath::createPathFromString(&expCtx, "b", expCtx.variablesParseState)},
         {"c",
          ExpressionFieldPath::createPathFromString(&expCtx, "c", expCtx.variablesParseState)}});
    ASSERT_VALUE_EQ(
        Value(Document{{"a", "A"sv}, {"b", "B"sv}, {"c", "C"sv}}),
        object->evaluate(Document{{"c", "C"sv}, {"a", "A"sv}, {"b", "B"sv}, {"_id", "ID"sv}},
                         &(expCtx.variables),
                         {}));
}

TEST(ExpressionObjectEvaluate, ShouldRemoveFieldsThatHaveMissingValues) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx,
        {{"a",
          ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState)},
         {"b",
          ExpressionFieldPath::createPathFromString(
              &expCtx, "missing", expCtx.variablesParseState)}});
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document(), &(expCtx.variables), {}));
    ASSERT_VALUE_EQ(Value(Document{}),
                    object->evaluate(Document{{"a", 1}}, &(expCtx.variables), {}));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateFieldsWithinNestedObject) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx,
        {{"a",
          ExpressionObject::create(&expCtx,
                                   {{"b", ExpressionConstant::create(&expCtx, Value{1})},
                                    {"c",
                                     ExpressionFieldPath::createPathFromString(
                                         &expCtx, "_id", expCtx.variablesParseState)}})}});
    ASSERT_VALUE_EQ(Value(Document{{"a", Document{{"b", 1}}}}),
                    object->evaluate(Document(), &(expCtx.variables), {}));
    ASSERT_VALUE_EQ(Value(Document{{"a", Document{{"b", 1}, {"c", "ID"sv}}}}),
                    object->evaluate(Document{{"_id", "ID"sv}}, &(expCtx.variables), {}));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateToEmptyDocumentIfAllFieldsAreMissing) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(&expCtx,
                                           {{"a",
                                             ExpressionFieldPath::createPathFromString(
                                                 &expCtx, "missing", expCtx.variablesParseState)}});
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document(), &(expCtx.variables), {}));

    auto objectWithNestedObject = ExpressionObject::create(&expCtx, {{"nested", object}});
    ASSERT_VALUE_EQ(Value(Document{{"nested", Document{}}}),
                    objectWithNestedObject->evaluate(Document(), &(expCtx.variables), {}));
}

namespace {
boost::intrusive_ptr<Expression> makeFieldPathObject(ExpressionContextForTest* expCtx) {
    return ExpressionObject::create(
        expCtx,
        {{"a", ExpressionFieldPath::createPathFromString(expCtx, "a", expCtx->variablesParseState)},
         {"b",
          ExpressionFieldPath::createPathFromString(expCtx, "b", expCtx->variablesParseState)}});
}
}  // namespace

TEST(ExpressionObjectEvaluate, TracksOutputMemoryAndReleasesAfterEvaluation) {
    auto expCtx = ExpressionContextForTest{};
    auto object = makeFieldPathObject(&expCtx);

    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{4096}};
    EvaluationContext ctx{.tracker = &tracker};

    Document doc{{"a", "hello"sv}, {"b", "world"sv}};
    ASSERT_VALUE_EQ(object->evaluate(doc, &expCtx.variables, ctx),
                    Value(Document{{"a", "hello"sv}, {"b", "world"sv}}));

    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(tracker.peakTrackedMemoryBytes(), 0);
}

TEST(ExpressionObjectEvaluate, ThrowsExceededMemoryLimitWhenOverLimit) {
    auto expCtx = ExpressionContextForTest{};
    auto object = makeFieldPathObject(&expCtx);

    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{8}};
    EvaluationContext ctx{.tracker = &tracker};

    Document doc{{"a", std::string(100, 'x')}, {"b", std::string(100, 'y')}};
    try {
        object->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$object");
    }

    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
}

TEST(ExpressionObjectEvaluate, ThrowsExceededMemoryLimitWhenQueryLimitExceeded) {
    auto expCtx = ExpressionContextForTest{};
    auto object = makeFieldPathObject(&expCtx);

    SimpleMemoryUsageTracker operationTracker{MemoryUsageLimit{8}};
    SimpleMemoryUsageTracker stageTracker{&operationTracker, MemoryUsageLimit{100 * 1024 * 1024}};
    EvaluationContext ctx{.tracker = &stageTracker};

    Document doc{{"a", std::string(100, 'x')}, {"b", std::string(100, 'y')}};
    try {
        object->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$object");
    }

    ASSERT_EQ(stageTracker.inUseTrackedMemoryBytes(), 0);
}

TEST(ExpressionObjectEvaluate, FallbackTrackerWithinLimitDoesNotThrow) {
    auto expCtx = ExpressionContextForTest{};
    auto object = makeFieldPathObject(&expCtx);

    Document doc{{"a", "hello"sv}, {"b", "world"sv}};

    const int64_t limit = 10 * 1024 * 1024;
    // Disable expression tracking so the fallback is standalone and enforces the per-expression cap
    unittest::ServerParameterGuard exprFlag{"featureFlagExpressionMemoryTracking", false};
    unittest::ServerParameterGuard limitGuard{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                              limit};

    // Within the configured limit, evaluation succeeds and charges the fallback tracker.
    EvaluationContext ctx{};
    ASSERT_VALUE_EQ(object->evaluate(doc, &expCtx.variables, ctx),
                    Value(Document{{"a", "hello"sv}, {"b", "world"sv}}));

    auto& tracker = expCtx.getExpressionFallbackTracker();
    ASSERT_GT(tracker.peakTrackedMemoryBytes(), 0);
    ASSERT_LT(tracker.peakTrackedMemoryBytes(), limit);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
}

TEST(ExpressionObjectEvaluate, FallbackTrackerEnforcesLimit) {
    auto expCtx = ExpressionContextForTest{};
    auto object = makeFieldPathObject(&expCtx);

    Document doc{{"a", std::string(100, 'x')}, {"b", std::string(100, 'y')}};

    const int64_t limit = 8;
    // Disable expression tracking so the fallback is standalone and enforces the per-expression cap
    unittest::ServerParameterGuard exprFlag{"featureFlagExpressionMemoryTracking", false};
    unittest::ServerParameterGuard limitGuard{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                              limit};

    EvaluationContext ctx{};
    try {
        object->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$object");
    }
    ASSERT_EQ(expCtx.getExpressionFallbackTracker().inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(expCtx.getExpressionFallbackTracker().peakTrackedMemoryBytes(), limit);
}

TEST(ExpressionObjectEvaluate, ExcludedContextUsesFallbackTrackerEvenWithStageTracker) {
    auto expCtx = ExpressionContextForTest{};
    expCtx.setExcludeOperationMemoryTracking(true);
    auto object = makeFieldPathObject(&expCtx);

    Document doc{{"a", std::string(100, 'x')}, {"b", std::string(100, 'y')}};

    const int64_t limit = 8;
    unittest::ServerParameterGuard limitGuard{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                              limit};

    // Wire an unlimited stage tracker into the EvaluationContext: if evaluation charged it, no
    // limit would trip. A context excluded from operation-wide memory tracking (e.g. stream
    // processing) must route expression memory to the fallback tracker instead, which is
    // standalone and enforces the per-expression safety cap.
    SimpleMemoryUsageTracker stageTracker{MemoryUsageLimit{std::numeric_limits<int64_t>::max()}};
    EvaluationContext ctx{};
    ctx.tracker = &stageTracker;

    ASSERT_THROWS_CODE(object->evaluate(doc, &expCtx.variables, ctx),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
    ASSERT_EQ(stageTracker.peakTrackedMemoryBytes(), 0);
    ASSERT_EQ(expCtx.getExpressionFallbackTracker().inUseTrackedMemoryBytes(), 0);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
