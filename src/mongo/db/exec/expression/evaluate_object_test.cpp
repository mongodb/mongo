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
#include "mongo/db/query/query_knobs/query_knob_configuration_test_util.h"
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

TEST(ExpressionObjectEvaluate, WithinPerExpressionCapDoesNotThrow) {
    auto expCtx = ExpressionContextForTest{};
    auto object = makeFieldPathObject(&expCtx);

    // Default cap is large; a small object stays well under it.
    Document doc{{"a", "hello"sv}, {"b", "world"sv}};
    EvaluationContext ctx{};
    ASSERT_VALUE_EQ(object->evaluate(doc, &expCtx.variables, ctx),
                    Value(Document{{"a", "hello"sv}, {"b", "world"sv}}));
}

TEST(ExpressionObjectEvaluate, ThrowsExceededMemoryLimitWhenOverPerExpressionCap) {
    auto expCtx = ExpressionContextForTest{};
    auto object = makeFieldPathObject(&expCtx);

    // Sets the knob so the lowered cap is observed by ExpressionObject::evaluate.
    QueryKnobGuardForTest limitGuard{
        expCtx.getOperationContext(), "internalQueryMaxSingleExpressionMemoryUsageBytes", 8};

    Document doc{{"a", std::string(100, 'x')}, {"b", std::string(100, 'y')}};
    EvaluationContext ctx{};
    try {
        object->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$object");
    }
}

TEST(ExpressionObjectEvaluate, OutputAccountingDoesNotChargeCtxTracker) {
    // $object accounts its output locally against the per-expression cap. The tracker wired into
    // the EvaluationContext is therefore not charged by $object itself (child expressions may still
    // consult it).
    auto expCtx = ExpressionContextForTest{};
    auto object = makeFieldPathObject(&expCtx);

    SimpleMemoryUsageTracker tracker{MemoryUsageLimit{8}};
    EvaluationContext ctx{.tracker = &tracker};

    Document doc{{"a", std::string(100, 'x')}, {"b", std::string(100, 'y')}};
    ASSERT_VALUE_EQ(object->evaluate(doc, &expCtx.variables, ctx),
                    Value(Document{{"a", std::string(100, 'x')}, {"b", std::string(100, 'y')}}));
    ASSERT_EQ(tracker.peakTrackedMemoryBytes(), 0);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
