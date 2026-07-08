/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

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

    SimpleMemoryUsageTracker tracker{4096};
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

    SimpleMemoryUsageTracker tracker{8};
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

    SimpleMemoryUsageTracker operationTracker{8};
    SimpleMemoryUsageTracker stageTracker{&operationTracker, 100 * 1024 * 1024};
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

}  // namespace expression_evaluation_test
}  // namespace mongo
