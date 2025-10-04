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
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <climits>
#include <cmath>
#include <limits>

namespace mongo {
namespace expression_evaluation_test {

using boost::intrusive_ptr;
using std::string;

namespace {

/**
 * Creates an expression which parses named arguments via an object specification, then evaluates it
 * and returns the result.
 */
Value evaluateNamedArgExpression(const string& expressionName, const Document& operand) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON(expressionName << operand);
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Value result = expression->evaluate({}, &expCtx.variables);
    return result;
}

}  // namespace

namespace strcasecmp {

void assertResult(int expectedResult, const BSONObj& spec) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj specObj = BSON("" << spec);
    BSONElement specElement = specObj.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
    ASSERT_BSONOBJ_EQ(BSON("" << expectedResult),
                      toBson(expression->evaluate({}, &expCtx.variables)));
}

void runTest(string arg1, string arg2, int expectedResult) {
    assertResult(expectedResult, BSON("$strcasecmp" << BSON_ARRAY(arg1 << arg2)));
    assertResult(-expectedResult, BSON("$strcasecmp" << BSON_ARRAY(arg2 << arg1)));
}

TEST(ExpressionStrcaseCmpTest, NullBegin) {
    runTest(string("\0ab", 3), string("\0AB", 3), 0);
}

TEST(ExpressionStrcaseCmpTest, NullEnd) {
    runTest(string("ab\0", 3), string("aB\0", 3), 0);
}

TEST(ExpressionStrcaseCmpTest, NullMiddleLt) {
    runTest(string("a\0a", 3), string("a\0B", 3), -1);
}

TEST(ExpressionStrcaseCmpTest, NullMiddleEq) {
    runTest(string("a\0b", 3), string("a\0B", 3), 0);
}

TEST(ExpressionStrcaseCmpTest, NullMiddleGt) {
    runTest(string("a\0c", 3), string("a\0B", 3), 1);
}
}  // namespace strcasecmp

namespace str_len_bytes {

TEST(ExpressionStrLenBytes, ComputesLengthOfString) {
    assertExpectedResults("$strLenBytes", {{{Value("abc"_sd)}, Value(3)}});
}

TEST(ExpressionStrLenBytes, ComputesLengthOfEmptyString) {
    assertExpectedResults("$strLenBytes", {{{Value(StringData())}, Value(0)}});
}

TEST(ExpressionStrLenBytes, ComputesLengthOfStringWithNull) {
    assertExpectedResults("$strLenBytes", {{{Value("ab\0c"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNullAtEnd) {
    assertExpectedResults("$strLenBytes", {{{Value("abc\0"_sd)}, Value(4)}});
}

}  // namespace str_len_bytes

namespace str_len_cp {

TEST(ExpressionStrLenCP, ComputesLengthOfASCIIString) {
    assertExpectedResults("$strLenCP", {{{Value("abc"_sd)}, Value(3)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfEmptyString) {
    assertExpectedResults("$strLenCP", {{{Value(StringData())}, Value(0)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNull) {
    assertExpectedResults("$strLenCP", {{{Value("ab\0c"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNullAtEnd) {
    assertExpectedResults("$strLenCP", {{{Value("abc\0"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithAccent) {
    assertExpectedResults("$strLenCP", {{{Value("a\0bâ"_sd)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithSpecialCharacters) {
    assertExpectedResults("$strLenCP", {{{Value("ºabøåß"_sd)}, Value(6)}});
}

}  // namespace str_len_cp

namespace substr_bytes {
void runTest(string str, int offset, int length, string expectedResult) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj specObj = BSON("" << BSON("$substrBytes" << BSON_ARRAY(str << offset << length)));
    BSONElement specElement = specObj.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
    ASSERT_BSONOBJ_EQ(BSON("" << expectedResult),
                      toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionSubstrBytesTest, FullNull) {
    /** Retrieve a full string containing a null character. */
    runTest(string("a\0b", 3), 0, 3, string("a\0b", 3));
}

TEST(ExpressionSubstrBytesTest, BeginAtNull) {
    /** Retrieve a substring beginning with a null character. */
    runTest(string("a\0b", 3), 1, 2, string("\0b", 2));
}

TEST(ExpressionSubstrBytesTest, EndAtNull) {
    /** Retrieve a substring ending with a null character. */
    runTest(string("a\0b", 3), 0, 2, string("a\0", 2));
}

TEST(ExpressionSubstrBytesTest, DropBeginningNull) {
    /** Drop a beginning null character. */
    runTest(string("\0b", 2), 1, 1, "b");
}

TEST(ExpressionSubstrBytesTest, DropEndingNull) {
    /** Drop a beginning null character. */
    runTest(string("a\0", 2), 0, 1, "a");
}

TEST(ExpressionSubstrBytesTest, NegativeLength) {
    /** When length is negative, the remainder of the string should be returned. */
    runTest(string("abcdefghij"), 2, -1, "cdefghij");
}

TEST(ExpressionSubstrTest, ThrowsWithNegativeStart) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    const auto str = "abcdef"_sd;
    const auto expr =
        Expression::parseExpression(&expCtx, BSON("$substrCP" << BSON_ARRAY(str << -5 << 1)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);
}

}  // namespace substr_bytes

namespace substr_cp {

TEST(ExpressionSubstrCPTest, DoesThrowWithBadContinuationByte) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    const auto continuationByte = "\x80\x00"_sd;
    const auto expr = Expression::parseExpression(
        &expCtx, BSON("$substrCP" << BSON_ARRAY(continuationByte << 0 << 1)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);
}

TEST(ExpressionSubstrCPTest, DoesThrowWithInvalidLeadingByte) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;

    const auto leadingByte = "\xFF\x00"_sd;
    const auto expr = Expression::parseExpression(
        &expCtx, BSON("$substrCP" << BSON_ARRAY(leadingByte << 0 << 1)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);
}

TEST(ExpressionSubstrCPTest, WithStandardValue) {
    assertExpectedResults("$substrCP", {{{Value("abc"_sd), Value(0), Value(2)}, Value("ab"_sd)}});
}

TEST(ExpressionSubstrCPTest, WithNullCharacter) {
    assertExpectedResults("$substrCP",
                          {{{Value("abc\0d"_sd), Value(2), Value(3)}, Value("c\0d"_sd)}});
}

TEST(ExpressionSubstrCPTest, WithNullCharacterAtEnd) {
    assertExpectedResults("$substrCP",
                          {{{Value("abc\0"_sd), Value(2), Value(2)}, Value("c\0"_sd)}});
}

TEST(ExpressionSubstrCPTest, WithOutOfRangeString) {
    assertExpectedResults("$substrCP",
                          {{{Value("abc"_sd), Value(3), Value(2)}, Value(StringData())}});
}

TEST(ExpressionSubstrCPTest, WithPartiallyOutOfRangeString) {
    assertExpectedResults("$substrCP", {{{Value("abc"_sd), Value(1), Value(4)}, Value("bc"_sd)}});
}

TEST(ExpressionSubstrCPTest, WithUnicodeValue) {
    assertExpectedResults("$substrCP",
                          {{{Value("øø∫å"_sd), Value(0), Value(4)}, Value("øø∫å"_sd)}});
    assertExpectedResults("$substrBytes",
                          {{{Value("øø∫å"_sd), Value(0), Value(4)}, Value("øø"_sd)}});
}

TEST(ExpressionSubstrCPTest, WithMixedUnicodeAndASCIIValue) {
    assertExpectedResults("$substrCP",
                          {{{Value("a∫bøßabc"_sd), Value(1), Value(4)}, Value("∫bøß"_sd)}});
    assertExpectedResults("$substrBytes",
                          {{{Value("a∫bøßabc"_sd), Value(1), Value(4)}, Value("∫b"_sd)}});
}

TEST(ExpressionSubstrCPTest, ShouldCoerceDateToString) {
    assertExpectedResults("$substrCP",
                          {{{Value(Date_t::fromMillisSinceEpoch(0)), Value(0), Value(1000)},
                            Value("1970-01-01T00:00:00.000Z"_sd)}});
    assertExpectedResults("$substrBytes",
                          {{{Value(Date_t::fromMillisSinceEpoch(0)), Value(0), Value(1000)},
                            Value("1970-01-01T00:00:00.000Z"_sd)}});
}

}  // namespace substr_cp

namespace to_lower {

void runTest(string str, string expectedResult) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj specObj = BSON("" << BSON("$toLower" << BSON_ARRAY(str)));
    BSONElement specElement = specObj.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
    ASSERT_BSONOBJ_EQ(BSON("" << expectedResult),
                      toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionToLowerTest, NullBegin) {
    /** String beginning with a null character. */
    runTest(string("\0aB", 3), string("\0ab", 3));
}

TEST(ExpressionToLowerTest, NullMiddle) {
    /** String containing a null character. */
    runTest(string("a\0B", 3), string("a\0b", 3));
}

TEST(ExpressionToLowerTest, NullEnd) {
    /** String ending with a null character. */
    runTest(string("aB\0", 3), string("ab\0", 3));
}

}  // namespace to_lower

namespace to_upper {

void runTest(string str, string expectedResult) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj specObj = BSON("" << BSON("$toUpper" << BSON_ARRAY(str)));
    BSONElement specElement = specObj.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    intrusive_ptr<Expression> expression = Expression::parseOperand(&expCtx, specElement, vps);
    ASSERT_BSONOBJ_EQ(BSON("" << expectedResult),
                      toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionToUpperTest, NullBegin) {
    /** String beginning with a null character. */
    runTest(string("\0aB", 3), string("\0AB", 3));
}

TEST(ExpressionToUpperTest, NullMiddle) {
    /** String containing a null character. */
    runTest(string("a\0B", 3), string("A\0B", 3));
}

TEST(ExpressionToUpperTest, NullEnd) {
    /** String ending with a null character. */
    runTest(string("aB\0", 3), string("AB\0", 3));
}

}  // namespace to_upper

TEST(ExpressionTrimTest, ThrowsIfInputIsNotString) {
    ASSERT_THROWS(evaluateNamedArgExpression("$trim", Document{{"input", 1}}), AssertionException);
    ASSERT_THROWS(evaluateNamedArgExpression("$trim", Document{{"input", BSON_ARRAY(1 << 2)}}),
                  AssertionException);
    ASSERT_THROWS(evaluateNamedArgExpression("$ltrim", Document{{"input", 3}}), AssertionException);
    ASSERT_THROWS(evaluateNamedArgExpression("$rtrim", Document{{"input", Document{{"x", 1}}}}),
                  AssertionException);
}

TEST(ExpressionTrimTest, ThrowsIfCharsIsNotAString) {
    ASSERT_THROWS(evaluateNamedArgExpression("$trim", Document{{"input", " x "_sd}, {"chars", 1}}),
                  AssertionException);
    ASSERT_THROWS(evaluateNamedArgExpression(
                      "$trim", Document{{"input", " x "_sd}, {"chars", BSON_ARRAY(1 << 2)}}),
                  AssertionException);
    ASSERT_THROWS(evaluateNamedArgExpression("$ltrim", Document{{"input", " x "_sd}, {"chars", 3}}),
                  AssertionException);
    ASSERT_THROWS(evaluateNamedArgExpression(
                      "$rtrim", Document{{"input", " x "_sd}, {"chars", Document{{"x", 1}}}}),
                  AssertionException);
}

TEST(ExpressionTrimTest, DoesTrimAsciiWhitespace) {
    // Trim from both sides.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "  abc  "_sd}}),
                    Value{"abc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "\n  abc \r\n "_sd}}),
                    Value{"abc"_sd});

    // Trim just from the right.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "abc  "_sd}}),
                    Value{"abc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "abc \r\n "_sd}}),
                    Value{"abc"_sd});

    // Trim just from the left.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "  abc"_sd}}),
                    Value{"abc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "\n  abc"_sd}}),
                    Value{"abc"_sd});

    // Make sure we don't trim from the opposite side when doing $ltrim or $rtrim.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "  abc"_sd}}),
                    Value{"  abc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "\t \nabc \r\n "_sd}}),
                    Value{"\t \nabc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "  abc  "_sd}}),
                    Value{"abc  "_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "\n  abc \t\n  "_sd}}),
                    Value{"abc \t\n  "_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "abc  "_sd}}),
                    Value{"abc  "_sd});
}

TEST(ExpressionTrimTest, DoesTrimNullCharacters) {
    // Trim from both sides.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "\0\0abc\0"_sd}}),
                    Value{"abc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "\0 \0 abc \0  "_sd}}),
                    Value{"abc"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "\n \0  abc \r\0\n "_sd}}),
        Value{"abc"_sd});

    // Trim just from the right.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "abc\0\0"_sd}}),
                    Value{"abc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "abc \r\0\n\0 "_sd}}),
                    Value{"abc"_sd});

    // Trim just from the left.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "\0\0abc"_sd}}),
                    Value{"abc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "\n \0\0 abc"_sd}}),
                    Value{"abc"_sd});

    // Make sure we don't trim from the opposite side when doing $ltrim or $rtrim.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "\0\0abc"_sd}}),
                    Value{"\0\0abc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", " \0 abc"_sd}}),
                    Value{" \0 abc"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "\t\0\0 \nabc \r\n "_sd}}),
        Value{"\t\0\0 \nabc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "  abc\0\0"_sd}}),
                    Value{"abc\0\0"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "\n  abc \t\0\n \0\0 "_sd}}),
        Value{"abc \t\0\n \0\0 "_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "abc\0\0"_sd}}),
                    Value{"abc\0\0"_sd});
}

TEST(ExpressionTrimTest, DoesTrimUnicodeWhitespace) {
    // Trim from both sides.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "\u2001abc\u2004\u200A"_sd}}),
        Value{"abc"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$trim", Document{{"input", "\n\u0020 \0\u2007  abc \r\0\n\u0009\u200A "_sd}}),
        Value{"abc"_sd});

    // Trim just from the right.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "abc\u2007\u2006"_sd}}),
                    Value{"abc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", "abc \r\u2009\u0009\u200A\n\0 "_sd}}),
                    Value{"abc"_sd});

    // Trim just from the left.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "\u2009\u2004abc"_sd}}),
                    Value{"abc"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", "\n \u2000 \0\u2008\0 \u200Aabc"_sd}}),
                    Value{"abc"_sd});
}

TEST(ExpressionTrimTest, DoesTrimCustomAsciiCharacters) {
    // Trim from both sides.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "xxXXxx"_sd}, {"chars", "x"_sd}}),
        Value{"XX"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "00123"_sd}, {"chars", "0"_sd}}),
        Value{"123"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim",
                                   Document{{"input", "30:00:12 I don't care about the time"_sd},
                                            {"chars", "0123456789: "_sd}}),
        Value{"I don't care about the time"_sd});

    // Trim just from the right.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "xxXXxx"_sd}, {"chars", "x"_sd}}),
        Value{"xxXX"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "00123"_sd}, {"chars", "0"_sd}}),
        Value{"00123"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim",
                                   Document{{"input", "30:00:12 I don't care about the time"_sd},
                                            {"chars", "0123456789: "_sd}}),
        Value{"30:00:12 I don't care about the time"_sd});

    // Trim just from the left.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "xxXXxx"_sd}, {"chars", "x"_sd}}),
        Value{"xxXX"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "00123"_sd}, {"chars", "0"_sd}}),
        Value{"00123"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim",
                                   Document{{"input", "30:00:12 I don't care about the time"_sd},
                                            {"chars", "0123456789: "_sd}}),
        Value{"30:00:12 I don't care about the time"_sd});
}

TEST(ExpressionTrimTest, DoesTrimCustomUnicodeCharacters) {
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "∃x.x ≥ y"_sd}, {"chars", "∃"_sd}}),
        Value{"x.x ≥ y"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "∃x.x ≥ y"_sd}, {"chars", "∃"_sd}}),
        Value{"∃x.x ≥ y"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "∃x.x ≥ y"_sd}, {"chars", "∃"_sd}}),
        Value{"x.x ≥ y"_sd});

    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "⌊x⌋"_sd}, {"chars", "⌊⌋"_sd}}),
        Value{"x⌋"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "⌊x⌋"_sd}, {"chars", "⌊⌋"_sd}}),
        Value{"⌊x"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "⌊x⌋"_sd}, {"chars", "⌊⌋"_sd}}),
        Value{"x"_sd});
}

TEST(ExpressionTrimTest, DoesTrimCustomMixOfUnicodeAndAsciiCharacters) {
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", "∃x.x ≥ y"_sd}, {"chars", "∃y"_sd}}),
                    Value{"x.x ≥ y"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", "∃x.x ≥ y"_sd}, {"chars", "∃y"_sd}}),
                    Value{"∃x.x ≥ "_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "∃x.x ≥ y"_sd}, {"chars", "∃y"_sd}}),
        Value{"x.x ≥ "_sd});

    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "⌊x⌋"_sd}, {"chars", "⌊x⌋"_sd}}),
        Value{""_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "⌊x⌋"_sd}, {"chars", "⌊x⌋"_sd}}),
        Value{""_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "⌊x⌋"_sd}, {"chars", "⌊x⌋"_sd}}),
        Value{""_sd});

    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$ltrim", Document{{"input", "▹▱◯□ I ▙◉VE Shapes □◯▱◃"_sd}, {"chars", "□◯▱◃▹ "_sd}}),
        Value{"I ▙◉VE Shapes □◯▱◃"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$rtrim", Document{{"input", "▹▱◯□ I ▙◉VE Shapes □◯▱◃"_sd}, {"chars", "□◯▱◃▹ "_sd}}),
        Value{"▹▱◯□ I ▙◉VE Shapes"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$trim", Document{{"input", "▹▱◯□ I ▙◉VE Shapes □◯▱◃"_sd}, {"chars", "□◯▱◃▹ "_sd}}),
        Value{"I ▙◉VE Shapes"_sd});
}

TEST(ExpressionTrimTest, DoesNotTrimFromMiddle) {
    // Using ascii whitespace.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "  a\tb c  "_sd}}),
                    Value{"a\tb c"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "\n  a\nb  c \r\n "_sd}}),
        Value{"a\nb  c"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "  a\tb c  "_sd}}),
                    Value{"  a\tb c"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "\n  a\nb  c \r\n "_sd}}),
        Value{"\n  a\nb  c"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "  a\tb c  "_sd}}),
                    Value{"a\tb c  "_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "\n  a\nb  c \r\n "_sd}}),
        Value{"a\nb  c \r\n "_sd});

    // Using unicode whitespace.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", "\u2001a\u2001\u000Ab\u2009c\u2004\u200A"_sd}}),
                    Value{"a\u2001\u000Ab\u2009c"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$ltrim", Document{{"input", "\u2001a\u2001\u000Ab\u2009c\u2004\u200A"_sd}}),
        Value{"a\u2001\u000Ab\u2009c\u2004\u200A"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$rtrim", Document{{"input", "\u2001a\u2001\u000Ab\u2009c\u2004\u200A"_sd}}),
        Value{"\u2001a\u2001\u000Ab\u2009c"_sd});

    // With custom ascii characters.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "xxXxXxx"_sd}, {"chars", "x"_sd}}),
        Value{"XxX"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "xxXxXxx"_sd}, {"chars", "x"_sd}}),
        Value{"xxXxX"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "xxXxXxx"_sd}, {"chars", "x"_sd}}),
        Value{"XxXxx"_sd});

    // With custom unicode characters.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", "⌊y + 2⌋⌊x⌋"_sd}, {"chars", "⌊⌋"_sd}}),
                    Value{"y + 2⌋⌊x"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", "⌊y + 2⌋⌊x⌋"_sd}, {"chars", "⌊⌋"_sd}}),
                    Value{"y + 2⌋⌊x⌋"_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", "⌊y + 2⌋⌊x⌋"_sd}, {"chars", "⌊⌋"_sd}}),
                    Value{"⌊y + 2⌋⌊x"_sd});
}

TEST(ExpressionTrimTest, DoesTrimEntireString) {
    // Using ascii whitespace.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "  \t \n  "_sd}}),
                    Value{""_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "   \t  \n\0  "_sd}}),
                    Value{""_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "  \t   "_sd}}),
                    Value{""_sd});

    // Using unicode whitespace.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$trim", Document{{"input", "\u2001 \u2001\t\u000A  \u2009\u2004\u200A"_sd}}),
        Value{""_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$ltrim", Document{{"input", "\u2001 \u2001\t\u000A  \u2009\u2004\u200A"_sd}}),
        Value{""_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$rtrim", Document{{"input", "\u2001 \u2001\t\u000A  \u2009\u2004\u200A"_sd}}),
        Value{""_sd});

    // With custom characters.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "xxXxXxx"_sd}, {"chars", "x"_sd}}),
        Value{"XxX"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "xxXxXxx"_sd}, {"chars", "x"_sd}}),
        Value{"xxXxX"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "xxXxXxx"_sd}, {"chars", "x"_sd}}),
        Value{"XxXxx"_sd});

    // With custom unicode characters.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "⌊y⌋⌊x⌋"_sd}, {"chars", "⌊xy⌋"_sd}}),
        Value{""_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", "⌊y⌋⌊x⌋"_sd}, {"chars", "⌊xy⌋"_sd}}),
                    Value{""_sd});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", "⌊y⌋⌊x⌋"_sd}, {"chars", "⌊xy⌋"_sd}}),
                    Value{""_sd});
}

TEST(ExpressionTrimTest, DoesNotTrimAnyThingWithEmptyChars) {
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "abcde"_sd}, {"chars", ""_sd}}),
        Value{"abcde"_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "  "_sd}, {"chars", ""_sd}}),
        Value{"  "_sd});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", " ⌊y⌋⌊x⌋ "_sd}, {"chars", ""_sd}}),
        Value{" ⌊y⌋⌊x⌋ "_sd});
}

TEST(ExpressionTrimTest, TrimComparisonsShouldNotRespectCollation) {
    auto expCtx = ExpressionContextForTest{};
    auto caseInsensitive =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    expCtx.setCollator(std::move(caseInsensitive));

    auto trim = Expression::parseExpression(&expCtx,
                                            BSON("$trim" << BSON("input" << "xxXXxx"
                                                                         << "chars"
                                                                         << "x")),
                                            expCtx.variablesParseState);

    ASSERT_VALUE_EQ(trim->evaluate({}, &expCtx.variables), Value("XX"_sd));
}

TEST(ExpressionTrimTest, ShouldRejectInvalidUTFInCharsArgument) {
    const auto twoThirdsOfExistsSymbol = "\xE2\x88"_sd;  // Full ∃ symbol would be "\xE2\x88\x83".
    ASSERT_THROWS(evaluateNamedArgExpression(
                      "$trim", Document{{"input", "abcde"_sd}, {"chars", twoThirdsOfExistsSymbol}}),
                  AssertionException);
    const auto stringWithExtraContinuationByte = "\xE2\x88\x83\x83"_sd;
    ASSERT_THROWS(
        evaluateNamedArgExpression(
            "$trim", Document{{"input", "ab∃"_sd}, {"chars", stringWithExtraContinuationByte}}),
        AssertionException);
    ASSERT_THROWS(
        evaluateNamedArgExpression("$ltrim",
                                   Document{{"input", "a" + twoThirdsOfExistsSymbol + "b∃"},
                                            {"chars", stringWithExtraContinuationByte}}),
        AssertionException);
}

TEST(ExpressionTrimTest, ShouldIgnoreUTF8InputWithTruncatedCodePoint) {
    const auto twoThirdsOfExistsSymbol = "\xE2\x88"_sd;  // Full ∃ symbol would be "\xE2\x88\x83".

    // We are OK producing invalid UTF-8 if the input string was invalid UTF-8, so if the truncated
    // code point is in the middle and we never examine it, it should work fine.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$rtrim",
            Document{{"input", "abc" + twoThirdsOfExistsSymbol + "edf∃"}, {"chars", "∃"_sd}}),
        Value("abc" + twoThirdsOfExistsSymbol + "edf"));

    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", twoThirdsOfExistsSymbol}, {"chars", "∃"_sd}}),
                    Value(twoThirdsOfExistsSymbol));
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$rtrim", Document{{"input", "abc" + twoThirdsOfExistsSymbol}, {"chars", "∃"_sd}}),
        Value("abc" + twoThirdsOfExistsSymbol));
}

TEST(ExpressionTrimTest, ShouldNotTrimUTF8InputWithTrailingExtraContinuationBytes) {
    const auto stringWithExtraContinuationByte = "\xE2\x88\x83\x83"_sd;

    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$trim",
            Document{{"input", stringWithExtraContinuationByte + "edf∃"}, {"chars", "∃"_sd}}),
        Value("\x83" + "edf"_sd));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim",
                        Document{{"input", "abc" + stringWithExtraContinuationByte + "edf∃"},
                                 {"chars", "∃"_sd}}),
                    Value("abc" + stringWithExtraContinuationByte + "edf"_sd));
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$trim",
            Document{{"input", "Abc" + stringWithExtraContinuationByte}, {"chars", "∃"_sd}}),
        Value("Abc" + stringWithExtraContinuationByte));
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$rtrim", Document{{"input", stringWithExtraContinuationByte}, {"chars", "∃"_sd}}),
        Value(stringWithExtraContinuationByte));
}

TEST(ExpressionTrimTest, ShouldRetunNullIfInputIsNullish) {
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", BSONNULL}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "$missingField"_sd}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", BSONUndefined}}),
                    Value(BSONNULL));

    // Test with a chars argument provided.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", BSONNULL}, {"chars", "∃"_sd}}),
        Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", "$missingField"_sd}, {"chars", "∃"_sd}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", BSONUndefined}, {"chars", "∃"_sd}}),
        Value(BSONNULL));

    // Test other variants of trim.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", BSONNULL}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", "$missingField"_sd}, {"chars", "∃"_sd}}),
                    Value(BSONNULL));
}

TEST(ExpressionTrimTest, ShouldRetunNullIfCharsIsNullish) {
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", " x "_sd}, {"chars", BSONNULL}}),
        Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", " x "_sd}, {"chars", "$missingField"_sd}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", " x "_sd}, {"chars", BSONUndefined}}),
                    Value(BSONNULL));

    // Test other variants of trim.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", " x "_sd}, {"chars", "$missingField"_sd}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", " x "_sd}, {"chars", BSONUndefined}}),
                    Value(BSONNULL));
}

TEST(ExpressionTrimTest, ShouldReturnNullIfBothCharsAndCharsAreNullish) {
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", BSONNULL}, {"chars", BSONNULL}}),
        Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", BSONUndefined}, {"chars", "$missingField"_sd}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", "$missingField"_sd}, {"chars", BSONUndefined}}),
                    Value(BSONNULL));

    // Test other variants of trim.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", BSONNULL}, {"chars", "$missingField"_sd}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", "$missingField"_sd}, {"chars", BSONNULL}}),
                    Value(BSONNULL));
}

}  // namespace expression_evaluation_test
}  // namespace mongo
