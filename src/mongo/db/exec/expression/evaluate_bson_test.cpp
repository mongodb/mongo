/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

#include <climits>
#include <cmath>
#include <limits>

namespace mongo {
namespace expression_evaluation_test {

TEST(ExpressionInternalFindAllValuesAtPath, PreservesSimpleArray) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a"_sd));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    auto result =
        expression->evaluate(Document{{"a", Value({Value(1), Value(2)})}}, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, PreservesSimpleNestedArray) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"_sd));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    auto doc = Document{{"a", Value(Document{{"b", Value({Value(1), Value(2)})}})}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, DescendsThroughSingleArrayAndObject) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"_sd));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Document doc = Document{
        {"a",
         Value({Document{{"b", Value(1)}}, Document{{"b", Value(2)}}, Document{{"b", Value(3)}}})}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2 << 3)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, DescendsThroughMultipleObjectArrayPairs) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"_sd));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Document doc = Document{{"a",
                             Value({Document{{"b", Value({Value(1), Value(2)})}},
                                    Document{{"b", Value({Value(3), Value(4)})}},
                                    Document{{"b", Value({Value(5), Value(6)})}}})}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, DoesNotDescendThroughDoubleArray) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"_sd));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Document seenDoc1 = Document{{"b", Value({Value(5), Value(6)})}};
    Document seenDoc2 = Document{{"b", Value({Value(3), Value(4)})}};
    Document unseenDoc1 = Document{{"b", Value({Value(1), Value(2)})}};
    Document unseenDoc2 = Document{{"b", Value({Value(7), Value(8)})}};

    Document doc = Document{{"a",
                             Value({
                                 Value({unseenDoc1, unseenDoc2}),
                                 Value(seenDoc1),
                                 Value(seenDoc2),
                             })}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(3 << 4 << 5 << 6)), result);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
