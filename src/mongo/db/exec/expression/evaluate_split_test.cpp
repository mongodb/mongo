// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

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
    ASSERT_THROWS(split(1, ""sv).second, AssertionException);
    ASSERT_THROWS(split(1, BSONRegEx()).second, AssertionException);
    ASSERT_THROWS(split(""sv, 1).second, AssertionException);
    ASSERT_THROWS(split(BSONRegEx(), ""sv).second, AssertionException);
}

TEST(ExpressionEvaluateSplitTest, HandlesNullish) {
    // If any argument is nullish, the result is null.
    ASSERT_VALUE_EQ(split(BSONNULL, ""sv).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(split(BSONNULL, BSONRegEx()).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(split(""sv, BSONNULL).second, Value(BSONNULL));
}

TEST(ExpressionEvaluateSplitTest, SplitNothingWhenNoMatches) {
    // When there are no matches, the result is a list with only the input string.
    ASSERT_VALUE_EQ(split(""sv, "x"sv).second, Value(std::vector<Value>{Value(""sv)}));
    ASSERT_VALUE_EQ(split("a"sv, "x"sv).second, Value(std::vector<Value>{Value("a"sv)}));
    ASSERT_VALUE_EQ(split("abcd"sv, "x"sv).second, Value(std::vector<Value>{Value("abcd"sv)}));
    ASSERT_VALUE_EQ(split("abcd"sv, "xyz"sv).second, Value(std::vector<Value>{Value("abcd"sv)}));
    ASSERT_VALUE_EQ(split("xyyz"sv, "xyz"sv).second, Value(std::vector<Value>{Value("xyyz"sv)}));

    ASSERT_VALUE_EQ(split(""sv, BSONRegEx("x")).second, Value(std::vector<Value>{Value(""sv)}));
    ASSERT_VALUE_EQ(split("a"sv, BSONRegEx("x")).second, Value(std::vector<Value>{Value("a"sv)}));
    ASSERT_VALUE_EQ(split("abcd"sv, BSONRegEx("x")).second,
                    Value(std::vector<Value>{Value("abcd"sv)}));
    ASSERT_VALUE_EQ(split("abcd"sv, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value("abcd"sv)}));
    ASSERT_VALUE_EQ(split("xyyz"sv, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value("xyyz"sv)}));
}

TEST(ExpressionEvaluateSplitTest, SplitUsingStringOrConstantRegex) {
    ASSERT_VALUE_EQ(split("x"sv, "x"sv).second,
                    Value(std::vector<Value>{Value(""sv), Value(""sv)}));
    ASSERT_VALUE_EQ(split("xyz"sv, "xyz"sv).second,
                    Value(std::vector<Value>{Value(""sv), Value(""sv)}));
    ASSERT_VALUE_EQ(split("..xyz.."sv, "xyz"sv).second,
                    Value(std::vector<Value>{Value(".."sv), Value(".."sv)}));
    ASSERT_VALUE_EQ(split("..xyz"sv, "xyz"sv).second,
                    Value(std::vector<Value>{Value(".."sv), Value(""sv)}));
    ASSERT_VALUE_EQ(split("xyz.."sv, "xyz"sv).second,
                    Value(std::vector<Value>{Value(""sv), Value(".."sv)}));
    ASSERT_VALUE_EQ(split("xyzyz"sv, "y"sv).second,
                    Value(std::vector<Value>{Value("x"sv), Value("z"sv), Value("z"sv)}));
    ASSERT_VALUE_EQ(
        split("xyyyz"sv, "y"sv).second,
        Value(std::vector<Value>{Value("x"sv), Value(""sv), Value(""sv), Value("z"sv)}));

    ASSERT_VALUE_EQ(split("x"sv, BSONRegEx("x")).second,
                    Value(std::vector<Value>{Value(""sv), Value(""sv)}));
    ASSERT_VALUE_EQ(split("xyz"sv, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value(""sv), Value(""sv)}));
    ASSERT_VALUE_EQ(split("..xyz.."sv, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value(".."sv), Value(".."sv)}));
    ASSERT_VALUE_EQ(split("..xyz"sv, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value(".."sv), Value(""sv)}));
    ASSERT_VALUE_EQ(split("xyz.."sv, BSONRegEx("xyz")).second,
                    Value(std::vector<Value>{Value(""sv), Value(".."sv)}));
    ASSERT_VALUE_EQ(split("xyzyz"sv, BSONRegEx("y")).second,
                    Value(std::vector<Value>{Value("x"sv), Value("z"sv), Value("z"sv)}));
    ASSERT_VALUE_EQ(
        split("xyyyz"sv, BSONRegEx("y")).second,
        Value(std::vector<Value>{Value("x"sv), Value(""sv), Value(""sv), Value("z"sv)}));
}

TEST(ExpressionEvaluateSplitTest, SplitWithEmptyString) {
    ASSERT_THROWS(split(""sv, ""sv).second, AssertionException);
    ASSERT_THROWS(split(".."sv, ""sv).second, AssertionException);
    ASSERT_VALUE_EQ(split(""sv, "."sv).second, Value(std::vector<Value>{Value(""sv)}));
}

TEST(ExpressionEvaluateSplitTest, MatchPossiblyEmpty) {
    ASSERT_VALUE_EQ(split("x"sv, BSONRegEx("x*")).second,
                    Value(std::vector<Value>{Value(""sv), Value(""sv), Value(""sv)}));
    ASSERT_VALUE_EQ(split("..xyz.."sv, BSONRegEx("[.y]*")).second,
                    Value(std::vector<Value>{Value(""sv),
                                             Value(""sv),
                                             Value("x"sv),
                                             Value(""sv),
                                             Value("z"sv),
                                             Value(""sv),
                                             Value(""sv)}));
    ASSERT_VALUE_EQ(split("..xyz"sv, BSONRegEx("[zxy]*")).second,
                    Value(std::vector<Value>{
                        Value(""sv), Value("."sv), Value("."sv), Value(""sv), Value(""sv)}));
    ASSERT_VALUE_EQ(split("xyzyz"sv, BSONRegEx("y*")).second,
                    Value(std::vector<Value>{Value(""sv),
                                             Value("x"sv),
                                             Value(""sv),
                                             Value("z"sv),
                                             Value(""sv),
                                             Value("z"sv),
                                             Value(""sv)}));
    ASSERT_VALUE_EQ(split("xyyyz"sv, BSONRegEx("y*")).second,
                    Value(std::vector<Value>{
                        Value(""sv), Value("x"sv), Value(""sv), Value("z"sv), Value(""sv)}));
}

TEST(ExpressionEvaluateSplitTest, MatchWithSubGroups) {
    ASSERT_VALUE_EQ(split("x-y"sv, BSONRegEx("(-)")).second,
                    Value(std::vector<Value>{Value("x"sv), Value("-"sv), Value("y"sv)}));
    ASSERT_VALUE_EQ(split("xyz"sv, BSONRegEx("(x*)")).second,
                    Value(std::vector<Value>{Value(""sv),
                                             Value("x"sv),
                                             Value(""sv),
                                             Value(""sv),
                                             Value("y"sv),
                                             Value(""sv),
                                             Value("z"sv),
                                             Value(""sv),
                                             Value(""sv)}));
    ASSERT_VALUE_EQ(split("xyz"sv, BSONRegEx("((x*))")).second,
                    Value(std::vector<Value>{Value(""sv),
                                             Value("x"sv),
                                             Value("x"sv),
                                             Value(""sv),
                                             Value(""sv),
                                             Value(""sv),
                                             Value("y"sv),
                                             Value(""sv),
                                             Value(""sv),
                                             Value("z"sv),
                                             Value(""sv),
                                             Value(""sv),
                                             Value(""sv)}));
    ASSERT_VALUE_EQ(split("xyz"sv, BSONRegEx("((x+)*)")).second,
                    Value(std::vector<Value>{Value(""sv),
                                             Value("x"sv),
                                             Value("x"sv),
                                             Value(""sv),
                                             Value(""sv),
                                             Value(""sv),
                                             Value("y"sv),
                                             Value(""sv),
                                             Value(""sv),
                                             Value("z"sv),
                                             Value(""sv),
                                             Value(""sv),
                                             Value(""sv)}));
}

TEST(ExpressionEvaluateSplitTest, SplitWithUnicode) {
    std::string_view precomposedAcuteE = "é";

    ASSERT_VALUE_EQ(split("é"sv, "e"sv).second,
                    Value(std::vector<Value>{Value(""sv), Value("́"sv)}));
    ASSERT_VALUE_EQ(split(precomposedAcuteE, "e"sv).second,
                    Value(std::vector<Value>{Value(precomposedAcuteE)}));
    ASSERT_VALUE_EQ(split(precomposedAcuteE, precomposedAcuteE).second,
                    Value(std::vector<Value>{Value(""sv), Value(""sv)}));

    ASSERT_VALUE_EQ(split("é"sv, BSONRegEx("e")).second,
                    Value(std::vector<Value>{Value(""sv), Value("́"sv)}));
    ASSERT_VALUE_EQ(split(precomposedAcuteE, BSONRegEx("e")).second,
                    Value(std::vector<Value>{Value(precomposedAcuteE)}));
    ASSERT_VALUE_EQ(split(precomposedAcuteE, BSONRegEx(precomposedAcuteE)).second,
                    Value(std::vector<Value>{Value(""sv), Value(""sv)}));
}

}  // namespace expression_evaluation_test
}  // namespace mongo
