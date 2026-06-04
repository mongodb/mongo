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

#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>


namespace mongo {
namespace expression_evaluation_test {

//
// Evaluation.
//

TEST(ExpressionObjectEvaluate, EmptyObjectShouldEvaluateToEmptyDocument) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(&expCtx, {});
    ASSERT_VALUE_EQ(Value(Document()), object->evaluate(Document(), &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document()), object->evaluate(Document{{"a", 1}}, &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document()),
                    object->evaluate(Document{{"_id", "ID"_sd}}, &(expCtx.variables)));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateEachField) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(&expCtx,
                                           {{"a", ExpressionConstant::create(&expCtx, Value{1})},
                                            {"b", ExpressionConstant::create(&expCtx, Value{5})}});


    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}),
                    object->evaluate(Document(), &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}),
                    object->evaluate(Document{{"a", 1}}, &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document{{"a", 1}, {"b", 5}}),
                    object->evaluate(Document{{"_id", "ID"_sd}}, &(expCtx.variables)));
}

TEST(ExpressionObjectEvaluate, OrderOfFieldsInOutputShouldMatchOrderInSpecification) {
    auto expCtx = ExpressionContextForTest{};
    auto object =
        ExpressionObject::create(&expCtx,
                                 {{"a", ExpressionFieldPath::deprecatedCreate(&expCtx, "a")},
                                  {"b", ExpressionFieldPath::deprecatedCreate(&expCtx, "b")},
                                  {"c", ExpressionFieldPath::deprecatedCreate(&expCtx, "c")}});
    ASSERT_VALUE_EQ(
        Value(Document{{"a", "A"_sd}, {"b", "B"_sd}, {"c", "C"_sd}}),
        object->evaluate(Document{{"c", "C"_sd}, {"a", "A"_sd}, {"b", "B"_sd}, {"_id", "ID"_sd}},
                         &(expCtx.variables)));
}

TEST(ExpressionObjectEvaluate, ShouldRemoveFieldsThatHaveMissingValues) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx,
        {{"a", ExpressionFieldPath::deprecatedCreate(&expCtx, "a.b")},
         {"b", ExpressionFieldPath::deprecatedCreate(&expCtx, "missing")}});
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document(), &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document{{"a", 1}}, &(expCtx.variables)));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateFieldsWithinNestedObject) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx,
        {{"a",
          ExpressionObject::create(
              &expCtx,
              {{"b", ExpressionConstant::create(&expCtx, Value{1})},
               {"c", ExpressionFieldPath::deprecatedCreate(&expCtx, "_id")}})}});
    ASSERT_VALUE_EQ(Value(Document{{"a", Document{{"b", 1}}}}),
                    object->evaluate(Document(), &(expCtx.variables)));
    ASSERT_VALUE_EQ(Value(Document{{"a", Document{{"b", 1}, {"c", "ID"_sd}}}}),
                    object->evaluate(Document{{"_id", "ID"_sd}}, &(expCtx.variables)));
}

TEST(ExpressionObjectEvaluate, ShouldEvaluateToEmptyDocumentIfAllFieldsAreMissing) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(
        &expCtx, {{"a", ExpressionFieldPath::deprecatedCreate(&expCtx, "missing")}});
    ASSERT_VALUE_EQ(Value(Document{}), object->evaluate(Document(), &(expCtx.variables)));

    auto objectWithNestedObject = ExpressionObject::create(&expCtx, {{"nested", object}});
    ASSERT_VALUE_EQ(Value(Document{{"nested", Document{}}}),
                    objectWithNestedObject->evaluate(Document(), &(expCtx.variables)));
}

namespace {
std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> makeLargeValueFields(
    ExpressionContextForTest* expCtx, int numFields, size_t valueSizeBytes) {
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> fields;
    fields.reserve(numFields);
    for (int i = 0; i < numFields; ++i) {
        fields.emplace_back(
            "f" + std::to_string(i),
            ExpressionConstant::create(expCtx, Value(std::string(valueSizeBytes, 'a'))));
    }
    return fields;
}
}  // namespace

TEST(ExpressionObjectEvaluate, LargeObjectExceedsMemoryLimit) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(&expCtx, makeLargeValueFields(&expCtx, 10, 1024));

    RAIIServerParameterControllerForTest limit("internalQueryMaxExpressionOutputBytes", 100);
    ASSERT_THROWS_CODE(object->evaluate(Document(), &(expCtx.variables)),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

TEST(ExpressionObjectEvaluate, LargeObjectWithinMemoryLimitSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    auto object = ExpressionObject::create(&expCtx, makeLargeValueFields(&expCtx, 10, 1024));

    Value result = object->evaluate(Document(), &(expCtx.variables));
    ASSERT_TRUE(result.getType() == BSONType::object);
    ASSERT_EQ(result.getDocument().computeSize(), 10u);
}

TEST(ExpressionObjectEvaluate, LongFieldNamesCountTowardMemoryLimit) {
    auto expCtx = ExpressionContextForTest{};
    const std::string longKey(200, 'k');
    std::vector<std::pair<std::string, boost::intrusive_ptr<Expression>>> fields;
    for (int i = 0; i < 10; ++i) {
        fields.emplace_back(longKey + std::to_string(i),
                            ExpressionConstant::create(&expCtx, Value(1)));
    }
    auto object = ExpressionObject::create(&expCtx, std::move(fields));

    RAIIServerParameterControllerForTest limit("internalQueryMaxExpressionOutputBytes", 500);
    ASSERT_THROWS_CODE(object->evaluate(Document(), &(expCtx.variables)),
                       AssertionException,
                       ErrorCodes::ExceededMemoryLimit);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
