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
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

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

    ASSERT_THROWS(replaceOne(1, ""_sd, ""_sd).second, AssertionException);
    ASSERT_THROWS(replaceOne(""_sd, 1, ""_sd).second, AssertionException);
    ASSERT_THROWS(replaceOne(""_sd, ""_sd, 1).second, AssertionException);

    ASSERT_THROWS(replaceAll(1, ""_sd, ""_sd).second, AssertionException);
    ASSERT_THROWS(replaceAll(""_sd, 1, ""_sd).second, AssertionException);
    ASSERT_THROWS(replaceAll(""_sd, ""_sd, 1).second, AssertionException);

    ASSERT_THROWS(replaceOne(1, BSONRegEx(), ""_sd).second, AssertionException);
    ASSERT_THROWS(replaceOne(""_sd, BSONRegEx(), 1).second, AssertionException);

    ASSERT_THROWS(replaceAll(1, BSONRegEx(), ""_sd).second, AssertionException);
    ASSERT_THROWS(replaceAll(""_sd, BSONRegEx(), 1).second, AssertionException);
}

TEST(ExpressionEvaluateReplaceTest, HandlesNullish) {
    // If any argument is nullish, the result is null.
    ASSERT_VALUE_EQ(replaceOne(BSONNULL, ""_sd, ""_sd).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceOne(""_sd, BSONNULL, ""_sd).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceOne(""_sd, ""_sd, BSONNULL).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceOne(BSONNULL, BSONRegEx(), ""_sd).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceOne(""_sd, BSONRegEx(), BSONNULL).second, Value(BSONNULL));

    ASSERT_VALUE_EQ(replaceAll(BSONNULL, ""_sd, ""_sd).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceAll(""_sd, BSONNULL, ""_sd).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceAll(""_sd, ""_sd, BSONNULL).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceAll(BSONNULL, BSONRegEx(), ""_sd).second, Value(BSONNULL));
    ASSERT_VALUE_EQ(replaceAll(""_sd, BSONRegEx(), BSONNULL).second, Value(BSONNULL));
}

TEST(ExpressionEvaluateReplaceTest, ReplacesNothingWhenNoMatches) {
    // When there are no matches, the result is the input, unchanged.
    ASSERT_VALUE_EQ(replaceOne(""_sd, "x"_sd, "y"_sd).second, Value(""_sd));
    ASSERT_VALUE_EQ(replaceOne("a"_sd, "x"_sd, "y"_sd).second, Value("a"_sd));
    ASSERT_VALUE_EQ(replaceOne("abcd"_sd, "x"_sd, "y"_sd).second, Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceOne("abcd"_sd, "xyz"_sd, "y"_sd).second, Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceOne("xyyz"_sd, "xyz"_sd, "y"_sd).second, Value("xyyz"_sd));

    ASSERT_VALUE_EQ(replaceOne(""_sd, BSONRegEx("x"), "y"_sd).second, Value(""_sd));
    ASSERT_VALUE_EQ(replaceOne("a"_sd, BSONRegEx("x"), "y"_sd).second, Value("a"_sd));
    ASSERT_VALUE_EQ(replaceOne("abcd"_sd, BSONRegEx("xyz"), "y"_sd).second, Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceOne("abcd"_sd, BSONRegEx("xyz"), "y"_sd).second, Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceOne("xyyz"_sd, BSONRegEx("xyz"), "y"_sd).second, Value("xyyz"_sd));

    ASSERT_VALUE_EQ(replaceAll(""_sd, "x"_sd, "y"_sd).second, Value(""_sd));
    ASSERT_VALUE_EQ(replaceAll("a"_sd, "x"_sd, "y"_sd).second, Value("a"_sd));
    ASSERT_VALUE_EQ(replaceAll("abcd"_sd, "x"_sd, "y"_sd).second, Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceAll("abcd"_sd, "xyz"_sd, "y"_sd).second, Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceAll("xyyz"_sd, "xyz"_sd, "y"_sd).second, Value("xyyz"_sd));

    ASSERT_VALUE_EQ(replaceAll(""_sd, BSONRegEx("x"), "y"_sd).second, Value(""_sd));
    ASSERT_VALUE_EQ(replaceAll("a"_sd, BSONRegEx("x"), "y"_sd).second, Value("a"_sd));
    ASSERT_VALUE_EQ(replaceAll("abcd"_sd, BSONRegEx("xyz"), "y"_sd).second, Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceAll("abcd"_sd, BSONRegEx("xyz"), "y"_sd).second, Value("abcd"_sd));
    ASSERT_VALUE_EQ(replaceAll("xyyz"_sd, BSONRegEx("xyz"), "y"_sd).second, Value("xyyz"_sd));
}

TEST(ExpressionEvaluateReplaceTest, ReplacesOnlyMatch) {
    ASSERT_VALUE_EQ(replaceOne(""_sd, ""_sd, "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("x"_sd, "x"_sd, "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("xyz"_sd, "xyz"_sd, "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("..xyz.."_sd, "xyz"_sd, "abc"_sd).second, Value("..abc.."_sd));
    ASSERT_VALUE_EQ(replaceOne("..xyz"_sd, "xyz"_sd, "abc"_sd).second, Value("..abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("xyz.."_sd, "xyz"_sd, "abc"_sd).second, Value("abc.."_sd));

    ASSERT_VALUE_EQ(replaceOne(""_sd, BSONRegEx(""), "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("x"_sd, BSONRegEx("x"), "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("xyz"_sd, BSONRegEx("xyz"), "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("..xyz.."_sd, BSONRegEx("xyz"), "abc"_sd).second,
                    Value("..abc.."_sd));
    ASSERT_VALUE_EQ(replaceOne("..xyz"_sd, BSONRegEx("xyz"), "abc"_sd).second, Value("..abc"_sd));
    ASSERT_VALUE_EQ(replaceOne("xyz.."_sd, BSONRegEx("xyz"), "abc"_sd).second, Value("abc.."_sd));

    ASSERT_VALUE_EQ(replaceAll(""_sd, ""_sd, "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("x"_sd, "x"_sd, "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("xyz"_sd, "xyz"_sd, "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("..xyz.."_sd, "xyz"_sd, "abc"_sd).second, Value("..abc.."_sd));
    ASSERT_VALUE_EQ(replaceAll("..xyz"_sd, "xyz"_sd, "abc"_sd).second, Value("..abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("xyz.."_sd, "xyz"_sd, "abc"_sd).second, Value("abc.."_sd));

    ASSERT_VALUE_EQ(replaceAll(""_sd, BSONRegEx(""), "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("x"_sd, BSONRegEx("x"), "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("xyz"_sd, BSONRegEx("xyz"), "abc"_sd).second, Value("abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("..xyz.."_sd, BSONRegEx("xyz"), "abc"_sd).second,
                    Value("..abc.."_sd));
    ASSERT_VALUE_EQ(replaceAll("..xyz"_sd, BSONRegEx("xyz"), "abc"_sd).second, Value("..abc"_sd));
    ASSERT_VALUE_EQ(replaceAll("xyz.."_sd, BSONRegEx("xyz"), "abc"_sd).second, Value("abc.."_sd));
}

TEST(ExpressionReplaceOneTest, ReplacesFirstMatchOnly) {
    ASSERT_VALUE_EQ(replaceOne("."_sd, ""_sd, "abc"_sd).second, Value("abc."_sd));
    ASSERT_VALUE_EQ(replaceOne(".."_sd, ""_sd, "abc"_sd).second, Value("abc.."_sd));
    ASSERT_VALUE_EQ(replaceOne(".."_sd, "."_sd, "abc"_sd).second, Value("abc."_sd));
    ASSERT_VALUE_EQ(replaceOne("abc->defg->hij"_sd, "->"_sd, "."_sd).second,
                    Value("abc.defg->hij"_sd));

    ASSERT_VALUE_EQ(replaceOne("."_sd, BSONRegEx(""), "abc"_sd).second, Value("abc."_sd));
    ASSERT_VALUE_EQ(replaceOne(".."_sd, BSONRegEx(""), "abc"_sd).second, Value("abc.."_sd));
    ASSERT_VALUE_EQ(replaceOne(".."_sd, BSONRegEx("[.]"), "abc"_sd).second, Value("abc."_sd));
    ASSERT_VALUE_EQ(replaceOne("abc->defg->hij"_sd, BSONRegEx("->"), "."_sd).second,
                    Value("abc.defg->hij"_sd));
}

TEST(ExpressionReplaceAllTest, ReplacesAllMatches) {
    ASSERT_VALUE_EQ(replaceAll("."_sd, ""_sd, "abc"_sd).second, Value("abc.abc"_sd));
    ASSERT_VALUE_EQ(replaceAll(".."_sd, ""_sd, "abc"_sd).second, Value("abc.abc.abc"_sd));
    ASSERT_VALUE_EQ(replaceAll(".."_sd, "."_sd, "abc"_sd).second, Value("abcabc"_sd));
    ASSERT_VALUE_EQ(replaceAll("abc->defg->hij"_sd, "->"_sd, "."_sd).second,
                    Value("abc.defg.hij"_sd));

    ASSERT_VALUE_EQ(replaceAll("."_sd, BSONRegEx(""), "abc"_sd).second, Value("abc.abc"_sd));
    ASSERT_VALUE_EQ(replaceAll(".."_sd, BSONRegEx(""), "abc"_sd).second, Value("abc.abc.abc"_sd));
    ASSERT_VALUE_EQ(replaceAll(".."_sd, BSONRegEx("[.]"), "abc"_sd).second, Value("abcabc"_sd));
    ASSERT_VALUE_EQ(replaceAll("abc->defg->hij"_sd, BSONRegEx("->"), "."_sd).second,
                    Value("abc.defg.hij"_sd));
}

TEST(ExpressionEvaluateReplaceTest, DoesNotReplaceInTheReplacement) {
    ASSERT_VALUE_EQ(replaceOne("a.b.c"_sd, "."_sd, ".."_sd).second, Value("a..b.c"_sd));
    ASSERT_VALUE_EQ(replaceAll("a.b.c"_sd, "."_sd, ".."_sd).second, Value("a..b..c"_sd));

    ASSERT_VALUE_EQ(replaceOne("a.b.c"_sd, BSONRegEx("[.]"), ".."_sd).second, Value("a..b.c"_sd));
    ASSERT_VALUE_EQ(replaceAll("a.b.c"_sd, BSONRegEx("[.]"), ".."_sd).second, Value("a..b..c"_sd));
}

TEST(ExpressionEvaluateReplaceTest, DoesNotNormalizeUnicode) {
    StringData combiningAcute = "́"_sd;
    StringData combinedAcuteE = "é"_sd;
    ASSERT_EQ(combinedAcuteE[0], 'e');
    ASSERT_EQ(combinedAcuteE.substr(1), combiningAcute);

    StringData precomposedAcuteE = "é";
    ASSERT_NOT_EQUALS(precomposedAcuteE[0], 'e');

    // If the input has combining characters, you can match and replace the base letter.
    ASSERT_VALUE_EQ(replaceOne(combinedAcuteE, "e"_sd, "a"_sd).second, Value("á"_sd));
    ASSERT_VALUE_EQ(replaceAll(combinedAcuteE, "e"_sd, "a"_sd).second, Value("á"_sd));
    ASSERT_VALUE_EQ(replaceOne(combinedAcuteE, BSONRegEx("e"), "a"_sd).second, Value("á"_sd));
    ASSERT_VALUE_EQ(replaceAll(combinedAcuteE, BSONRegEx("e"), "a"_sd).second, Value("á"_sd));

    // If the input has precomposed characters, you can't replace the base letter.
    ASSERT_VALUE_EQ(replaceOne(precomposedAcuteE, "e"_sd, "x"_sd).second, Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(precomposedAcuteE, "e"_sd, "x"_sd).second, Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceOne(precomposedAcuteE, BSONRegEx("e"), "x"_sd).second,
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(precomposedAcuteE, BSONRegEx("e"), "x"_sd).second,
                    Value(precomposedAcuteE));

    // Precomposed characters and combined forms can't match each other.
    ASSERT_VALUE_EQ(replaceOne(precomposedAcuteE, combinedAcuteE, "x"_sd).second,
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(precomposedAcuteE, combinedAcuteE, "x"_sd).second,
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceOne(combinedAcuteE, precomposedAcuteE, "x"_sd).second,
                    Value(combinedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(combinedAcuteE, precomposedAcuteE, "x"_sd).second,
                    Value(combinedAcuteE));
    ASSERT_VALUE_EQ(replaceOne(precomposedAcuteE, BSONRegEx(combinedAcuteE), "x"_sd).second,
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(precomposedAcuteE, BSONRegEx(combinedAcuteE), "x"_sd).second,
                    Value(precomposedAcuteE));
    ASSERT_VALUE_EQ(replaceOne(combinedAcuteE, BSONRegEx(precomposedAcuteE), "x"_sd).second,
                    Value(combinedAcuteE));
    ASSERT_VALUE_EQ(replaceAll(combinedAcuteE, BSONRegEx(precomposedAcuteE), "x"_sd).second,
                    Value(combinedAcuteE));
}

TEST(ExpressionEvaluateReplaceTest, ReplacesWithVariableRegExPattern) {
    ASSERT_VALUE_EQ(replaceOne("xyz"_sd, BSONRegEx("x*"), "a"_sd).second, Value("ayz"_sd));
    ASSERT_VALUE_EQ(replaceOne("abcdefghij"_sd, BSONRegEx("...."), "<-->"_sd).second,
                    Value("<-->efghij"_sd));
    ASSERT_VALUE_EQ(replaceOne("xyzxx"_sd, BSONRegEx("x+"), "abc"_sd).second, Value("abcyzxx"_sd));
    ASSERT_VALUE_EQ(replaceOne("abc->defg->hij"_sd, BSONRegEx(".*"), "a"_sd).second, Value("a"_sd));

    ASSERT_VALUE_EQ(replaceAll("xyz"_sd, BSONRegEx("x*"), "a"_sd).second, Value("aayaza"_sd));
    ASSERT_VALUE_EQ(replaceAll("abcdefghij"_sd, BSONRegEx("...."), "<-->"_sd).second,
                    Value("<--><-->ij"_sd));
    ASSERT_VALUE_EQ(replaceAll("xyzxx"_sd, BSONRegEx("x+"), "abc"_sd).second, Value("abcyzabc"_sd));
    ASSERT_VALUE_EQ(replaceAll("abc->defg->hij"_sd, BSONRegEx(".*"), "a"_sd).second,
                    Value("aa"_sd));
}

}  // namespace expression_evaluation_test
}  // namespace mongo
