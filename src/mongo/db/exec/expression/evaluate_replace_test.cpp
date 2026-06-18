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

auto eval(const std::string& expressionName,
          ImplicitValue input,
          ImplicitValue find,
          ImplicitValue replacement) {
    auto [expCtx, expression] = parse(
        expressionName, Document{{"input", input}, {"find", find}, {"replacement", replacement}});
    return std::pair{std::move(expCtx),
                     expression->evaluate({}, &expression->getExpressionContext()->variables)};
}

auto replaceOne(ImplicitValue input, ImplicitValue find, ImplicitValue replacement) {
    return eval("$replaceOne", input, find, replacement);
}

auto replaceAll(ImplicitValue input, ImplicitValue find, ImplicitValue replacement) {
    return eval("$replaceAll", input, find, replacement);
}

}  // namespace

TEST(ExpressionEvaluateReplaceTest, ExpectsStringsOrNullish) {
    // If any argument is non-string non-nullish, it's an error.
    ASSERT_THROWS(replaceOne(1, BSONNULL, BSONNULL).second, AssertionException);
    ASSERT_THROWS(replaceOne(BSONNULL, 1, BSONNULL).second, AssertionException);
    ASSERT_THROWS(replaceOne(BSONNULL, BSONNULL, 1).second, AssertionException);

    ASSERT_THROWS(replaceAll(1, BSONNULL, BSONNULL).second, AssertionException);
    ASSERT_THROWS(replaceAll(BSONNULL, 1, BSONNULL).second, AssertionException);
    ASSERT_THROWS(replaceAll(BSONNULL, BSONNULL, 1).second, AssertionException);

    ASSERT_THROWS(replaceOne(1, ""sv, ""sv).second, AssertionException);
    ASSERT_THROWS(replaceOne(""sv, 1, ""sv).second, AssertionException);
    ASSERT_THROWS(replaceOne(""sv, ""sv, 1).second, AssertionException);

    ASSERT_THROWS(replaceAll(1, ""sv, ""sv).second, AssertionException);
    ASSERT_THROWS(replaceAll(""sv, 1, ""sv).second, AssertionException);
    ASSERT_THROWS(replaceAll(""sv, ""sv, 1).second, AssertionException);

    ASSERT_THROWS(replaceOne(1, BSONRegEx(), ""sv).second, AssertionException);
    ASSERT_THROWS(replaceOne(""sv, BSONRegEx(), 1).second, AssertionException);

    ASSERT_THROWS(replaceAll(1, BSONRegEx(), ""sv).second, AssertionException);
    ASSERT_THROWS(replaceAll(""sv, BSONRegEx(), 1).second, AssertionException);
}

TEST(ExpressionEvaluateReplaceTest, HandlesNullish) {
    // If any argument is nullish, the result is null.
    ASSERT_VALUE_EQ(replaceOne(BSONNULL, ""sv, ""sv).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceOne(""sv, BSONNULL, ""sv).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceOne(""sv, ""sv, BSONNULL).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceOne(BSONNULL, BSONRegEx(), ""sv).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceOne(""sv, BSONRegEx(), BSONNULL).second, Value(BSONNULL));

    ASSERT_VALUE_EQ(replaceAll(BSONNULL, ""sv, ""sv).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceAll(""sv, BSONNULL, ""sv).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceAll(""sv, ""sv, BSONNULL).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceAll(BSONNULL, BSONRegEx(), ""sv).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceAll(""sv, BSONRegEx(), BSONNULL).second, Value(BSONNULL));
}

TEST(ExpressionEvaluateReplaceTest, ReplacesNothingWhenNoMatches) {
    // When there are no matches, the result is the input, unchanged.
    ASSERT_VALUE_EQ(replaceOne(""sv, "x"sv, "y"sv).second, Value(""sv));
    ASSERT_VALUE_EQ(replaceOne("a"sv, "x"sv, "y"sv).second, Value("a"sv));
    ASSERT_VALUE_EQ(replaceOne("abcd"sv, "x"sv, "y"sv).second, Value("abcd"sv));
    ASSERT_VALUE_EQ(replaceOne("abcd"sv, "xyz"sv, "y"sv).second, Value("abcd"sv));
    ASSERT_VALUE_EQ(replaceOne("xyyz"sv, "xyz"sv, "y"sv).second, Value("xyyz"sv));

    ASSERT_VALUE_EQ(replaceOne(""sv, BSONRegEx("x"), "y"sv).second, Value(""sv));
    ASSERT_VALUE_EQ(replaceOne("a"sv, BSONRegEx("x"), "y"sv).second, Value("a"sv));
    ASSERT_VALUE_EQ(replaceOne("abcd"sv, BSONRegEx("xyz"), "y"sv).second, Value("abcd"sv));
    ASSERT_VALUE_EQ(replaceOne("abcd"sv, BSONRegEx("xyz"), "y"sv).second, Value("abcd"sv));
    ASSERT_VALUE_EQ(replaceOne("xyyz"sv, BSONRegEx("xyz"), "y"sv).second, Value("xyyz"sv));

    ASSERT_VALUE_EQ(replaceAll(""sv, "x"sv, "y"sv).second, Value(""sv));
    ASSERT_VALUE_EQ(replaceAll("a"sv, "x"sv, "y"sv).second, Value("a"sv));
    ASSERT_VALUE_EQ(replaceAll("abcd"sv, "x"sv, "y"sv).second, Value("abcd"sv));
    ASSERT_VALUE_EQ(replaceAll("abcd"sv, "xyz"sv, "y"sv).second, Value("abcd"sv));
    ASSERT_VALUE_EQ(replaceAll("xyyz"sv, "xyz"sv, "y"sv).second, Value("xyyz"sv));

    ASSERT_VALUE_EQ(replaceAll(""sv, BSONRegEx("x"), "y"sv).second, Value(""sv));
    ASSERT_VALUE_EQ(replaceAll("a"sv, BSONRegEx("x"), "y"sv).second, Value("a"sv));
    ASSERT_VALUE_EQ(replaceAll("abcd"sv, BSONRegEx("xyz"), "y"sv).second, Value("abcd"sv));
    ASSERT_VALUE_EQ(replaceAll("abcd"sv, BSONRegEx("xyz"), "y"sv).second, Value("abcd"sv));
    ASSERT_VALUE_EQ(replaceAll("xyyz"sv, BSONRegEx("xyz"), "y"sv).second, Value("xyyz"sv));
}

TEST(ExpressionEvaluateReplaceTest, ReplacesOnlyMatch) {
    ASSERT_VALUE_EQ(replaceOne(""sv, ""sv, "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceOne("x"sv, "x"sv, "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceOne("xyz"sv, "xyz"sv, "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceOne("..xyz.."sv, "xyz"sv, "abc"sv).second, Value("..abc.."sv));
    ASSERT_VALUE_EQ(replaceOne("..xyz"sv, "xyz"sv, "abc"sv).second, Value("..abc"sv));
    ASSERT_VALUE_EQ(replaceOne("xyz.."sv, "xyz"sv, "abc"sv).second, Value("abc.."sv));

    ASSERT_VALUE_EQ(replaceOne(""sv, BSONRegEx(""), "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceOne("x"sv, BSONRegEx("x"), "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceOne("xyz"sv, BSONRegEx("xyz"), "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceOne("..xyz.."sv, BSONRegEx("xyz"), "abc"sv).second, Value("..abc.."sv));
    ASSERT_VALUE_EQ(replaceOne("..xyz"sv, BSONRegEx("xyz"), "abc"sv).second, Value("..abc"sv));
    ASSERT_VALUE_EQ(replaceOne("xyz.."sv, BSONRegEx("xyz"), "abc"sv).second, Value("abc.."sv));

    ASSERT_VALUE_EQ(replaceAll(""sv, ""sv, "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceAll("x"sv, "x"sv, "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceAll("xyz"sv, "xyz"sv, "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceAll("..xyz.."sv, "xyz"sv, "abc"sv).second, Value("..abc.."sv));
    ASSERT_VALUE_EQ(replaceAll("..xyz"sv, "xyz"sv, "abc"sv).second, Value("..abc"sv));
    ASSERT_VALUE_EQ(replaceAll("xyz.."sv, "xyz"sv, "abc"sv).second, Value("abc.."sv));

    ASSERT_VALUE_EQ(replaceAll(""sv, BSONRegEx(""), "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceAll("x"sv, BSONRegEx("x"), "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceAll("xyz"sv, BSONRegEx("xyz"), "abc"sv).second, Value("abc"sv));
    ASSERT_VALUE_EQ(replaceAll("..xyz.."sv, BSONRegEx("xyz"), "abc"sv).second, Value("..abc.."sv));
    ASSERT_VALUE_EQ(replaceAll("..xyz"sv, BSONRegEx("xyz"), "abc"sv).second, Value("..abc"sv));
    ASSERT_VALUE_EQ(replaceAll("xyz.."sv, BSONRegEx("xyz"), "abc"sv).second, Value("abc.."sv));
}

TEST(ExpressionReplaceOneTest, ReplacesFirstMatchOnly) {
    ASSERT_VALUE_EQ(replaceOne("."sv, ""sv, "abc"sv).second, Value("abc."sv));
    ASSERT_VALUE_EQ(replaceOne(".."sv, ""sv, "abc"sv).second, Value("abc.."sv));
    ASSERT_VALUE_EQ(replaceOne(".."sv, "."sv, "abc"sv).second, Value("abc."sv));
    ASSERT_VALUE_EQ(replaceOne("abc->defg->hij"sv, "->"sv, "."sv).second, Value("abc.defg->hij"sv));

    ASSERT_VALUE_EQ(replaceOne("."sv, BSONRegEx(""), "abc"sv).second, Value("abc."sv));
    ASSERT_VALUE_EQ(replaceOne(".."sv, BSONRegEx(""), "abc"sv).second, Value("abc.."sv));
    ASSERT_VALUE_EQ(replaceOne(".."sv, BSONRegEx("[.]"), "abc"sv).second, Value("abc."sv));
    ASSERT_VALUE_EQ(replaceOne("abc->defg->hij"sv, BSONRegEx("->"), "."sv).second,
                    Value("abc.defg->hij"sv));
}

TEST(ExpressionReplaceAllTest, ReplacesAllMatches) {
    ASSERT_VALUE_EQ(replaceAll("."sv, ""sv, "abc"sv).second, Value("abc.abc"sv));
    ASSERT_VALUE_EQ(replaceAll(".."sv, ""sv, "abc"sv).second, Value("abc.abc.abc"sv));
    ASSERT_VALUE_EQ(replaceAll(".."sv, "."sv, "abc"sv).second, Value("abcabc"sv));
    ASSERT_VALUE_EQ(replaceAll("abc->defg->hij"sv, "->"sv, "."sv).second, Value("abc.defg.hij"sv));

    ASSERT_VALUE_EQ(replaceAll("."sv, BSONRegEx(""), "abc"sv).second, Value("abc.abc"sv));
    ASSERT_VALUE_EQ(replaceAll(".."sv, BSONRegEx(""), "abc"sv).second, Value("abc.abc.abc"sv));
    ASSERT_VALUE_EQ(replaceAll(".."sv, BSONRegEx("[.]"), "abc"sv).second, Value("abcabc"sv));
    ASSERT_VALUE_EQ(replaceAll("abc->defg->hij"sv, BSONRegEx("->"), "."sv).second,
                    Value("abc.defg.hij"sv));
}

TEST(ExpressionEvaluateReplaceTest, DoesNotReplaceInTheReplacement) {
    ASSERT_VALUE_EQ(replaceOne("a.b.c"sv, "."sv, ".."sv).second, Value("a..b.c"sv));
    ASSERT_VALUE_EQ(replaceAll("a.b.c"sv, "."sv, ".."sv).second, Value("a..b..c"sv));

    ASSERT_VALUE_EQ(replaceOne("a.b.c"sv, BSONRegEx("[.]"), ".."sv).second, Value("a..b.c"sv));
    ASSERT_VALUE_EQ(replaceAll("a.b.c"sv, BSONRegEx("[.]"), ".."sv).second, Value("a..b..c"sv));
}

TEST(ExpressionEvaluateReplaceTest, DoesNotNormalizeUnicode) {
    std::string_view combiningAcute = "́"sv;
    std::string_view combinedAcuteE = "é"sv;
    ASSERT_EQ(combinedAcuteE[0], 'e');
    ASSERT_EQ(combinedAcuteE.substr(1), combiningAcute);

    std::string_view precomposedAcuteE = "é";
    ASSERT_NOT_EQUALS(precomposedAcuteE[0], 'e');

    // If the input has combining characters, you can match and replace the base letter.
    ASSERT_VALUE_EQ(replaceOne(combinedAcuteE, "e"sv, "a"sv).second, Value("á"sv));
    ASSERT_VALUE_EQ(replaceAll(combinedAcuteE, "e"sv, "a"sv).second, Value("á"sv));
    ASSERT_VALUE_EQ(replaceOne(combinedAcuteE, BSONRegEx("e"), "a"sv).second, Value("á"sv));
    ASSERT_VALUE_EQ(replaceAll(combinedAcuteE, BSONRegEx("e"), "a"sv).second, Value("á"sv));

    // If the input has precomposed characters, you can't replace the base letter.
    ASSERT_VALUE_EQ(replaceOne(precomposedAcuteE, "e"sv, "x"sv).second, Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(precomposedAcuteE, "e"sv, "x"sv).second, Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceOne(precomposedAcuteE, BSONRegEx("e"), "x"sv).second,
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(precomposedAcuteE, BSONRegEx("e"), "x"sv).second,
                    Value(precomposedAcuteE));

    // Precomposed characters and combined forms can't match each other.
    ASSERT_VALUE_EQ(replaceOne(precomposedAcuteE, combinedAcuteE, "x"sv).second,
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(precomposedAcuteE, combinedAcuteE, "x"sv).second,
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceOne(combinedAcuteE, precomposedAcuteE, "x"sv).second,
                    Value(combinedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(combinedAcuteE, precomposedAcuteE, "x"sv).second,
                    Value(combinedAcuteE));
    ASSERT_VALUE_EQ(replaceOne(precomposedAcuteE, BSONRegEx(combinedAcuteE), "x"sv).second,
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(precomposedAcuteE, BSONRegEx(combinedAcuteE), "x"sv).second,
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceOne(combinedAcuteE, BSONRegEx(precomposedAcuteE), "x"sv).second,
                    Value(combinedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(combinedAcuteE, BSONRegEx(precomposedAcuteE), "x"sv).second,
                    Value(combinedAcuteE));
}

TEST(ExpressionEvaluateReplaceTest, ReplacesWithVariableRegExPattern) {
    ASSERT_VALUE_EQ(replaceOne("xyz"sv, BSONRegEx("x*"), "a"sv).second, Value("ayz"sv));
    ASSERT_VALUE_EQ(replaceOne("abcdefghij"sv, BSONRegEx("...."), "<-->"sv).second,
                    Value("<-->efghij"sv));
    ASSERT_VALUE_EQ(replaceOne("xyzxx"sv, BSONRegEx("x+"), "abc"sv).second, Value("abcyzxx"sv));
    ASSERT_VALUE_EQ(replaceOne("abc->defg->hij"sv, BSONRegEx(".*"), "a"sv).second, Value("a"sv));

    ASSERT_VALUE_EQ(replaceAll("xyz"sv, BSONRegEx("x*"), "a"sv).second, Value("aayaza"sv));
    ASSERT_VALUE_EQ(replaceAll("abcdefghij"sv, BSONRegEx("...."), "<-->"sv).second,
                    Value("<--><-->ij"sv));
    ASSERT_VALUE_EQ(replaceAll("xyzxx"sv, BSONRegEx("x+"), "abc"sv).second, Value("abcyzabc"sv));
    ASSERT_VALUE_EQ(replaceAll("abc->defg->hij"sv, BSONRegEx(".*"), "a"sv).second, Value("aa"sv));
}

}  // namespace expression_evaluation_test
}  // namespace mongo
