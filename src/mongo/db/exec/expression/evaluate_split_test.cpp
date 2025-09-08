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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace expression_evaluation_test {

namespace {

auto parse(const std::string& expressionName, ImplicitValue operand) {
    auto pair =
        std::pair{std::make_unique<ExpressionContextForTest>(), boost::intrusive_ptr<Expression>{}};
    VariablesParseState vps = pair.first->variablesParseState;
    Value operandValue = operand;
    const BSONObj obj = BSON(expressionName << operandValue);
    pair.second = Expression::parseExpression(pair.first.get(), obj, vps);
    return pair;
}

auto split(ImplicitValue input, ImplicitValue separator) {
    auto [expCtx, expression] = parse("$split", std::vector<Value>{input, separator});
    return std::pair{std::move(expCtx),
                     expression->evaluate({}, &expression->getExpressionContext()->variables)};
}

}  // namespace

TEST(ExpressionEvaluateSplitTest, ExpectsStringsOrNullish) {
    // If any argument is non-string non-nullish, it's an error.
    ASSERT_THROWS(split(1, ""_sd).second, AssertionException);
    ASSERT_THROWS(split(1, BSONRegEx()).second, AssertionException);
    ASSERT_THROWS(split(""_sd, 1).second, AssertionException);
    ASSERT_THROWS(split(BSONRegEx(), ""_sd).second, AssertionException);
}

TEST(ExpressionEvaluateSplitTest, HandlesNullish) {
    // If any argument is nullish, the result is null.
    ASSERT_VALUE_EQ(split(BSONNULL, ""_sd).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(split(BSONNULL, BSONRegEx()).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(split(""_sd, BSONNULL).second, Value(BSONNULL));
}

TEST(ExpressionEvaluateSplitTest, SplitNothingWhenNoMatches) {
    // When there are no matches, the result is a list with only the input string.
    ASSERT_VALUE_EQ(split(""_sd, "x"_sd).second, Value(std::vector<Value>{Value(""_sd)}));
    ASSERT_VALUE_EQ(split("a"_sd, "x"_sd).second, Value(std::vector<Value>{Value("a"_sd)}));
    ASSERT_VALUE_EQ(split("abcd"_sd, "x"_sd).second, Value(std::vector<Value>{Value("abcd"_sd)}));
    ASSERT_VALUE_EQ(split("abcd"_sd, "xyz"_sd).second, Value(std::vector<Value>{Value("abcd"_sd)}));
    ASSERT_VALUE_EQ(split("xyyz"_sd, "xyz"_sd).second, Value(std::vector<Value>{Value("xyyz"_sd)}));

    ASSERT_VALUE_EQ(split(""_sd, BSONRegEx("x")).second, Value(std::vector<Value>{Value(""_sd)}));
    ASSERT_VALUE_EQ(split("a"_sd, BSONRegEx("x")).second, Value(std::vector<Value>{Value("a"_sd)}));
    ASSERT_VALUE_EQ(split("abcd"_sd, BSONRegEx("x")).second,
                    Value(std::vector<Value>{Value("abcd"_sd)}));
    ASSERT_VALUE_EQ(split("abcd"_sd, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value("abcd"_sd)}));
    ASSERT_VALUE_EQ(split("xyyz"_sd, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value("xyyz"_sd)}));
}

TEST(ExpressionEvaluateSplitTest, SplitUsingStringOrConstantRegex) {
    ASSERT_VALUE_EQ(split("x"_sd, "x"_sd).second,
                    Value(std::vector<Value>{Value(""_sd), Value(""_sd)}));
    ASSERT_VALUE_EQ(split("xyz"_sd, "xyz"_sd).second,
                    Value(std::vector<Value>{Value(""_sd), Value(""_sd)}));
    ASSERT_VALUE_EQ(split("..xyz.."_sd, "xyz"_sd).second,
                    Value(std::vector<Value>{Value(".."_sd), Value(".."_sd)}));
    ASSERT_VALUE_EQ(split("..xyz"_sd, "xyz"_sd).second,
                    Value(std::vector<Value>{Value(".."_sd), Value(""_sd)}));
    ASSERT_VALUE_EQ(split("xyz.."_sd, "xyz"_sd).second,
                    Value(std::vector<Value>{Value(""_sd), Value(".."_sd)}));
    ASSERT_VALUE_EQ(split("xyzyz"_sd, "y"_sd).second,
                    Value(std::vector<Value>{Value("x"_sd), Value("z"_sd), Value("z"_sd)}));
    ASSERT_VALUE_EQ(
        split("xyyyz"_sd, "y"_sd).second,
        Value(std::vector<Value>{Value("x"_sd), Value(""_sd), Value(""_sd), Value("z"_sd)}));

    ASSERT_VALUE_EQ(split("x"_sd, BSONRegEx("x")).second,
                    Value(std::vector<Value>{Value(""_sd), Value(""_sd)}));
    ASSERT_VALUE_EQ(split("xyz"_sd, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value(""_sd), Value(""_sd)}));
    ASSERT_VALUE_EQ(split("..xyz.."_sd, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value(".."_sd), Value(".."_sd)}));
    ASSERT_VALUE_EQ(split("..xyz"_sd, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value(".."_sd), Value(""_sd)}));
    ASSERT_VALUE_EQ(split("xyz.."_sd, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value(""_sd), Value(".."_sd)}));
    ASSERT_VALUE_EQ(split("xyzyz"_sd, BSONRegEx("y")).second,
                    Value(std::vector<Value>{Value("x"_sd), Value("z"_sd), Value("z"_sd)}));
    ASSERT_VALUE_EQ(
        split("xyyyz"_sd, BSONRegEx("y")).second,
        Value(std::vector<Value>{Value("x"_sd), Value(""_sd), Value(""_sd), Value("z"_sd)}));
}

TEST(ExpressionEvaluateSplitTest, SplitWithEmptyString) {
    ASSERT_THROWS(split(""_sd, ""_sd).second, AssertionException);
    ASSERT_THROWS(split(".."_sd, ""_sd).second, AssertionException);
    ASSERT_VALUE_EQ(split(""_sd, "."_sd).second, Value(std::vector<Value>{Value(""_sd)}));
}

TEST(ExpressionEvaluateSplitTest, MatchPossiblyEmpty) {
    ASSERT_VALUE_EQ(split("x"_sd, BSONRegEx("x*")).second,
                    Value(std::vector<Value>{Value(""_sd), Value(""_sd), Value(""_sd)}));
    ASSERT_VALUE_EQ(split("..xyz.."_sd, BSONRegEx("[.y]*")).second,
                    Value(std::vector<Value>{Value(""_sd),
                                             Value(""_sd),
                                             Value("x"_sd),
                                             Value(""_sd),
                                             Value("z"_sd),
                                             Value(""_sd),
                                             Value(""_sd)}));
    ASSERT_VALUE_EQ(split("..xyz"_sd, BSONRegEx("[zxy]*")).second,
                    Value(std::vector<Value>{
                        Value(""_sd), Value("."_sd), Value("."_sd), Value(""_sd), Value(""_sd)}));
    ASSERT_VALUE_EQ(split("xyzyz"_sd, BSONRegEx("y*")).second,
                    Value(std::vector<Value>{Value(""_sd),
                                             Value("x"_sd),
                                             Value(""_sd),
                                             Value("z"_sd),
                                             Value(""_sd),
                                             Value("z"_sd),
                                             Value(""_sd)}));
    ASSERT_VALUE_EQ(split("xyyyz"_sd, BSONRegEx("y*")).second,
                    Value(std::vector<Value>{
                        Value(""_sd), Value("x"_sd), Value(""_sd), Value("z"_sd), Value(""_sd)}));
}

TEST(ExpressionEvaluateSplitTest, MatchWithSubGroups) {
    ASSERT_VALUE_EQ(split("x-y"_sd, BSONRegEx("(-)")).second,
                    Value(std::vector<Value>{Value("x"_sd), Value("-"_sd), Value("y"_sd)}));
    ASSERT_VALUE_EQ(split("xyz"_sd, BSONRegEx("(x*)")).second,
                    Value(std::vector<Value>{Value(""_sd),
                                             Value("x"_sd),
                                             Value(""_sd),
                                             Value(""_sd),
                                             Value("y"_sd),
                                             Value(""_sd),
                                             Value("z"_sd),
                                             Value(""_sd),
                                             Value(""_sd)}));
    ASSERT_VALUE_EQ(split("xyz"_sd, BSONRegEx("((x*))")).second,
                    Value(std::vector<Value>{Value(""_sd),
                                             Value("x"_sd),
                                             Value("x"_sd),
                                             Value(""_sd),
                                             Value(""_sd),
                                             Value(""_sd),
                                             Value("y"_sd),
                                             Value(""_sd),
                                             Value(""_sd),
                                             Value("z"_sd),
                                             Value(""_sd),
                                             Value(""_sd),
                                             Value(""_sd)}));
    ASSERT_VALUE_EQ(split("xyz"_sd, BSONRegEx("((x+)*)")).second,
                    Value(std::vector<Value>{Value(""_sd),
                                             Value("x"_sd),
                                             Value("x"_sd),
                                             Value(""_sd),
                                             Value(""_sd),
                                             Value(""_sd),
                                             Value("y"_sd),
                                             Value(""_sd),
                                             Value(""_sd),
                                             Value("z"_sd),
                                             Value(""_sd),
                                             Value(""_sd),
                                             Value(""_sd)}));
}

TEST(ExpressionEvaluateSplitTest, SplitWithUnicode) {
    StringData precomposedAcuteE = "é";

    ASSERT_VALUE_EQ(split("é"_sd, "e"_sd).second,
                    Value(std::vector<Value>{Value(""_sd), Value("́"_sd)}));
    ASSERT_VALUE_EQ(split(precomposedAcuteE, "e"_sd).second,
                    Value(std::vector<Value>{Value(precomposedAcuteE)}));
    ASSERT_VALUE_EQ(split(precomposedAcuteE, precomposedAcuteE).second,
                    Value(std::vector<Value>{Value(""_sd), Value(""_sd)}));

    ASSERT_VALUE_EQ(split("é"_sd, BSONRegEx("e")).second,
                    Value(std::vector<Value>{Value(""_sd), Value("́"_sd)}));
    ASSERT_VALUE_EQ(split(precomposedAcuteE, BSONRegEx("e")).second,
                    Value(std::vector<Value>{Value(precomposedAcuteE)}));
    ASSERT_VALUE_EQ(split(precomposedAcuteE, BSONRegEx(precomposedAcuteE)).second,
                    Value(std::vector<Value>{Value(""_sd), Value(""_sd)}));
}

}  // namespace expression_evaluation_test
}  // namespace mongo
