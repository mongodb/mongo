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

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

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
    assertExpectedResults("$strLenBytes", {{{Value("abc"sv)}, Value(3)}});
}

TEST(ExpressionStrLenBytes, ComputesLengthOfEmptyString) {
    assertExpectedResults("$strLenBytes", {{{Value(std::string_view())}, Value(0)}});
}

TEST(ExpressionStrLenBytes, ComputesLengthOfStringWithNull) {
    assertExpectedResults("$strLenBytes", {{{Value("ab\0c"sv)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNullAtEnd) {
    assertExpectedResults("$strLenBytes", {{{Value("abc\0"sv)}, Value(4)}});
}

}  // namespace str_len_bytes

namespace str_len_cp {

TEST(ExpressionStrLenCP, ComputesLengthOfASCIIString) {
    assertExpectedResults("$strLenCP", {{{Value("abc"sv)}, Value(3)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfEmptyString) {
    assertExpectedResults("$strLenCP", {{{Value(std::string_view())}, Value(0)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNull) {
    assertExpectedResults("$strLenCP", {{{Value("ab\0c"sv)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithNullAtEnd) {
    assertExpectedResults("$strLenCP", {{{Value("abc\0"sv)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithAccent) {
    assertExpectedResults("$strLenCP", {{{Value("a\0bâ"sv)}, Value(4)}});
}

TEST(ExpressionStrLenCP, ComputesLengthOfStringWithSpecialCharacters) {
    assertExpectedResults("$strLenCP", {{{Value("ºabøåß"sv)}, Value(6)}});
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

    const auto str = "abcdef"sv;
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

    const auto continuationByte = "\x80\x00"sv;
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

    const auto leadingByte = "\xFF\x00"sv;
    const auto expr = Expression::parseExpression(
        &expCtx, BSON("$substrCP" << BSON_ARRAY(leadingByte << 0 << 1)), vps);
    ASSERT_THROWS(
        [&] {
            expr->evaluate({}, &expCtx.variables);
        }(),
        AssertionException);
}

TEST(ExpressionSubstrCPTest, WithStandardValue) {
    assertExpectedResults("$substrCP", {{{Value("abc"sv), Value(0), Value(2)}, Value("ab"sv)}});
}

TEST(ExpressionSubstrCPTest, WithNullCharacter) {
    assertExpectedResults("$substrCP",
                          {{{Value("abc\0d"sv), Value(2), Value(3)}, Value("c\0d"sv)}});
}

TEST(ExpressionSubstrCPTest, WithNullCharacterAtEnd) {
    assertExpectedResults("$substrCP", {{{Value("abc\0"sv), Value(2), Value(2)}, Value("c\0"sv)}});
}

TEST(ExpressionSubstrCPTest, WithOutOfRangeString) {
    assertExpectedResults("$substrCP",
                          {{{Value("abc"sv), Value(3), Value(2)}, Value(std::string_view())}});
}

TEST(ExpressionSubstrCPTest, WithPartiallyOutOfRangeString) {
    assertExpectedResults("$substrCP", {{{Value("abc"sv), Value(1), Value(4)}, Value("bc"sv)}});
}

TEST(ExpressionSubstrCPTest, WithUnicodeValue) {
    assertExpectedResults("$substrCP", {{{Value("øø∫å"sv), Value(0), Value(4)}, Value("øø∫å"sv)}});
    assertExpectedResults("$substrBytes", {{{Value("øø∫å"sv), Value(0), Value(4)}, Value("øø"sv)}});
}

TEST(ExpressionSubstrCPTest, WithMixedUnicodeAndASCIIValue) {
    assertExpectedResults("$substrCP",
                          {{{Value("a∫bøßabc"sv), Value(1), Value(4)}, Value("∫bøß"sv)}});
    assertExpectedResults("$substrBytes",
                          {{{Value("a∫bøßabc"sv), Value(1), Value(4)}, Value("∫b"sv)}});
}

TEST(ExpressionSubstrCPTest, ShouldCoerceDateToString) {
    assertExpectedResults("$substrCP",
                          {{{Value(Date_t::fromMillisSinceEpoch(0)), Value(0), Value(1000)},
                            Value("1970-01-01T00:00:00.000Z"sv)}});
    assertExpectedResults("$substrBytes",
                          {{{Value(Date_t::fromMillisSinceEpoch(0)), Value(0), Value(1000)},
                            Value("1970-01-01T00:00:00.000Z"sv)}});
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
    ASSERT_THROWS(evaluateNamedArgExpression("$trim", Document{{"input", " x "sv}, {"chars", 1}}),
                  AssertionException);
    ASSERT_THROWS(evaluateNamedArgExpression(
                      "$trim", Document{{"input", " x "sv}, {"chars", BSON_ARRAY(1 << 2)}}),
                  AssertionException);
    ASSERT_THROWS(evaluateNamedArgExpression("$ltrim", Document{{"input", " x "sv}, {"chars", 3}}),
                  AssertionException);
    ASSERT_THROWS(evaluateNamedArgExpression(
                      "$rtrim", Document{{"input", " x "sv}, {"chars", Document{{"x", 1}}}}),
                  AssertionException);
}

TEST(ExpressionTrimTest, DoesTrimAsciiWhitespace) {
    // Trim from both sides.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "  abc  "sv}}),
                    Value{"abc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "\n  abc \r\n "sv}}),
                    Value{"abc"sv});

    // Trim just from the right.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "abc  "sv}}),
                    Value{"abc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "abc \r\n "sv}}),
                    Value{"abc"sv});

    // Trim just from the left.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "  abc"sv}}),
                    Value{"abc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "\n  abc"sv}}),
                    Value{"abc"sv});

    // Make sure we don't trim from the opposite side when doing $ltrim or $rtrim.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "  abc"sv}}),
                    Value{"  abc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "\t \nabc \r\n "sv}}),
                    Value{"\t \nabc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "  abc  "sv}}),
                    Value{"abc  "sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "\n  abc \t\n  "sv}}),
                    Value{"abc \t\n  "sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "abc  "sv}}),
                    Value{"abc  "sv});
}

TEST(ExpressionTrimTest, DoesTrimNullCharacters) {
    // Trim from both sides.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "\0\0abc\0"sv}}),
                    Value{"abc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "\0 \0 abc \0  "sv}}),
                    Value{"abc"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "\n \0  abc \r\0\n "sv}}),
        Value{"abc"sv});

    // Trim just from the right.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "abc\0\0"sv}}),
                    Value{"abc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "abc \r\0\n\0 "sv}}),
                    Value{"abc"sv});

    // Trim just from the left.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "\0\0abc"sv}}),
                    Value{"abc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "\n \0\0 abc"sv}}),
                    Value{"abc"sv});

    // Make sure we don't trim from the opposite side when doing $ltrim or $rtrim.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "\0\0abc"sv}}),
                    Value{"\0\0abc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", " \0 abc"sv}}),
                    Value{" \0 abc"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "\t\0\0 \nabc \r\n "sv}}),
        Value{"\t\0\0 \nabc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "  abc\0\0"sv}}),
                    Value{"abc\0\0"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "\n  abc \t\0\n \0\0 "sv}}),
        Value{"abc \t\0\n \0\0 "sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "abc\0\0"sv}}),
                    Value{"abc\0\0"sv});
}

TEST(ExpressionTrimTest, DoesTrimUnicodeWhitespace) {
    // Trim from both sides.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "\u2001abc\u2004\u200A"sv}}),
        Value{"abc"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$trim", Document{{"input", "\n\u0020 \0\u2007  abc \r\0\n\u0009\u200A "sv}}),
        Value{"abc"sv});

    // Trim just from the right.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "abc\u2007\u2006"sv}}),
                    Value{"abc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", "abc \r\u2009\u0009\u200A\n\0 "sv}}),
                    Value{"abc"sv});

    // Trim just from the left.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "\u2009\u2004abc"sv}}),
                    Value{"abc"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", "\n \u2000 \0\u2008\0 \u200Aabc"sv}}),
                    Value{"abc"sv});
}

TEST(ExpressionTrimTest, DoesTrimCustomAsciiCharacters) {
    // Trim from both sides.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "xxXXxx"sv}, {"chars", "x"sv}}),
        Value{"XX"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "00123"sv}, {"chars", "0"sv}}),
        Value{"123"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim",
                                   Document{{"input", "30:00:12 I don't care about the time"sv},
                                            {"chars", "0123456789: "sv}}),
        Value{"I don't care about the time"sv});

    // Trim just from the right.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "xxXXxx"sv}, {"chars", "x"sv}}),
        Value{"xxXX"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "00123"sv}, {"chars", "0"sv}}),
        Value{"00123"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim",
                                   Document{{"input", "30:00:12 I don't care about the time"sv},
                                            {"chars", "0123456789: "sv}}),
        Value{"30:00:12 I don't care about the time"sv});

    // Trim just from the left.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "xxXXxx"sv}, {"chars", "x"sv}}),
        Value{"xxXX"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "00123"sv}, {"chars", "0"sv}}),
        Value{"00123"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim",
                                   Document{{"input", "30:00:12 I don't care about the time"sv},
                                            {"chars", "0123456789: "sv}}),
        Value{"30:00:12 I don't care about the time"sv});
}

TEST(ExpressionTrimTest, DoesTrimCustomUnicodeCharacters) {
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "∃x.x ≥ y"sv}, {"chars", "∃"sv}}),
        Value{"x.x ≥ y"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "∃x.x ≥ y"sv}, {"chars", "∃"sv}}),
        Value{"∃x.x ≥ y"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "∃x.x ≥ y"sv}, {"chars", "∃"sv}}),
        Value{"x.x ≥ y"sv});

    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "⌊x⌋"sv}, {"chars", "⌊⌋"sv}}),
        Value{"x⌋"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "⌊x⌋"sv}, {"chars", "⌊⌋"sv}}),
        Value{"⌊x"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "⌊x⌋"sv}, {"chars", "⌊⌋"sv}}),
        Value{"x"sv});
}

TEST(ExpressionTrimTest, DoesTrimCustomMixOfUnicodeAndAsciiCharacters) {
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "∃x.x ≥ y"sv}, {"chars", "∃y"sv}}),
        Value{"x.x ≥ y"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "∃x.x ≥ y"sv}, {"chars", "∃y"sv}}),
        Value{"∃x.x ≥ "sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "∃x.x ≥ y"sv}, {"chars", "∃y"sv}}),
        Value{"x.x ≥ "sv});

    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "⌊x⌋"sv}, {"chars", "⌊x⌋"sv}}),
        Value{""sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "⌊x⌋"sv}, {"chars", "⌊x⌋"sv}}),
        Value{""sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "⌊x⌋"sv}, {"chars", "⌊x⌋"sv}}),
        Value{""sv});

    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$ltrim", Document{{"input", "▹▱◯□ I ▙◉VE Shapes □◯▱◃"sv}, {"chars", "□◯▱◃▹ "sv}}),
        Value{"I ▙◉VE Shapes □◯▱◃"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$rtrim", Document{{"input", "▹▱◯□ I ▙◉VE Shapes □◯▱◃"sv}, {"chars", "□◯▱◃▹ "sv}}),
        Value{"▹▱◯□ I ▙◉VE Shapes"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$trim", Document{{"input", "▹▱◯□ I ▙◉VE Shapes □◯▱◃"sv}, {"chars", "□◯▱◃▹ "sv}}),
        Value{"I ▙◉VE Shapes"sv});
}

TEST(ExpressionTrimTest, DoesNotTrimFromMiddle) {
    // Using ascii whitespace.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "  a\tb c  "sv}}),
                    Value{"a\tb c"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "\n  a\nb  c \r\n "sv}}),
                    Value{"a\nb  c"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "  a\tb c  "sv}}),
                    Value{"  a\tb c"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "\n  a\nb  c \r\n "sv}}),
        Value{"\n  a\nb  c"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "  a\tb c  "sv}}),
                    Value{"a\tb c  "sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "\n  a\nb  c \r\n "sv}}),
        Value{"a\nb  c \r\n "sv});

    // Using unicode whitespace.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", "\u2001a\u2001\u000Ab\u2009c\u2004\u200A"sv}}),
                    Value{"a\u2001\u000Ab\u2009c"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", "\u2001a\u2001\u000Ab\u2009c\u2004\u200A"sv}}),
                    Value{"a\u2001\u000Ab\u2009c\u2004\u200A"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", "\u2001a\u2001\u000Ab\u2009c\u2004\u200A"sv}}),
                    Value{"\u2001a\u2001\u000Ab\u2009c"sv});

    // With custom ascii characters.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "xxXxXxx"sv}, {"chars", "x"sv}}),
        Value{"XxX"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "xxXxXxx"sv}, {"chars", "x"sv}}),
        Value{"xxXxX"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "xxXxXxx"sv}, {"chars", "x"sv}}),
        Value{"XxXxx"sv});

    // With custom unicode characters.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "⌊y + 2⌋⌊x⌋"sv}, {"chars", "⌊⌋"sv}}),
        Value{"y + 2⌋⌊x"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", "⌊y + 2⌋⌊x⌋"sv}, {"chars", "⌊⌋"sv}}),
                    Value{"y + 2⌋⌊x⌋"sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", "⌊y + 2⌋⌊x⌋"sv}, {"chars", "⌊⌋"sv}}),
                    Value{"⌊y + 2⌋⌊x"sv});
}

TEST(ExpressionTrimTest, DoesTrimEntireString) {
    // Using ascii whitespace.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "  \t \n  "sv}}),
                    Value{""sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", "   \t  \n\0  "sv}}),
                    Value{""sv});
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$rtrim", Document{{"input", "  \t   "sv}}),
                    Value{""sv});

    // Using unicode whitespace.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$trim", Document{{"input", "\u2001 \u2001\t\u000A  \u2009\u2004\u200A"sv}}),
        Value{""sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$ltrim", Document{{"input", "\u2001 \u2001\t\u000A  \u2009\u2004\u200A"sv}}),
        Value{""sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$rtrim", Document{{"input", "\u2001 \u2001\t\u000A  \u2009\u2004\u200A"sv}}),
        Value{""sv});

    // With custom characters.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "xxXxXxx"sv}, {"chars", "x"sv}}),
        Value{"XxX"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "xxXxXxx"sv}, {"chars", "x"sv}}),
        Value{"xxXxX"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "xxXxXxx"sv}, {"chars", "x"sv}}),
        Value{"XxXxx"sv});

    // With custom unicode characters.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "⌊y⌋⌊x⌋"sv}, {"chars", "⌊xy⌋"sv}}),
        Value{""sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$ltrim", Document{{"input", "⌊y⌋⌊x⌋"sv}, {"chars", "⌊xy⌋"sv}}),
        Value{""sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$rtrim", Document{{"input", "⌊y⌋⌊x⌋"sv}, {"chars", "⌊xy⌋"sv}}),
        Value{""sv});
}

TEST(ExpressionTrimTest, DoesNotTrimAnyThingWithEmptyChars) {
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "abcde"sv}, {"chars", ""sv}}),
        Value{"abcde"sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", "  "sv}, {"chars", ""sv}}),
        Value{"  "sv});
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", " ⌊y⌋⌊x⌋ "sv}, {"chars", ""sv}}),
        Value{" ⌊y⌋⌊x⌋ "sv});
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

    ASSERT_VALUE_EQ(trim->evaluate({}, &expCtx.variables), Value("XX"sv));
}

TEST(ExpressionTrimTest, ShouldRejectInvalidUTFInCharsArgument) {
    const auto twoThirdsOfExistsSymbol = "\xE2\x88"sv;  // Full ∃ symbol would be "\xE2\x88\x83".
    ASSERT_THROWS(evaluateNamedArgExpression(
                      "$trim", Document{{"input", "abcde"sv}, {"chars", twoThirdsOfExistsSymbol}}),
                  AssertionException);
    const auto stringWithExtraContinuationByte = "\xE2\x88\x83\x83"sv;
    ASSERT_THROWS(
        evaluateNamedArgExpression(
            "$trim", Document{{"input", "ab∃"sv}, {"chars", stringWithExtraContinuationByte}}),
        AssertionException);
    ASSERT_THROWS(evaluateNamedArgExpression(
                      "$ltrim",
                      Document{{"input", "a" + std::string(twoThirdsOfExistsSymbol) + "b∃"},
                               {"chars", stringWithExtraContinuationByte}}),
                  AssertionException);
}

TEST(ExpressionTrimTest, ShouldIgnoreUTF8InputWithTruncatedCodePoint) {
    const auto twoThirdsOfExistsSymbol = "\xE2\x88"sv;  // Full ∃ symbol would be "\xE2\x88\x83".

    // We are OK producing invalid UTF-8 if the input string was invalid UTF-8, so if the truncated
    // code point is in the middle and we never examine it, it should work fine.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim",
                        Document{{"input", "abc" + std::string(twoThirdsOfExistsSymbol) + "edf∃"},
                                 {"chars", "∃"sv}}),
                    Value("abc" + std::string(twoThirdsOfExistsSymbol) + "edf"));

    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", twoThirdsOfExistsSymbol}, {"chars", "∃"sv}}),
                    Value(twoThirdsOfExistsSymbol));
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$rtrim",
            Document{{"input", "abc" + std::string(twoThirdsOfExistsSymbol)}, {"chars", "∃"sv}}),
        Value("abc" + std::string(twoThirdsOfExistsSymbol)));
}

TEST(ExpressionTrimTest, ShouldNotTrimUTF8InputWithTrailingExtraContinuationBytes) {
    const auto stringWithExtraContinuationByte = "\xE2\x88\x83\x83"sv;

    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim",
                        Document{{"input", std::string(stringWithExtraContinuationByte) + "edf∃"},
                                 {"chars", "∃"sv}}),
                    Value("\x83" + std::string("edf"sv)));
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$trim",
            Document{{"input", "abc" + std::string(stringWithExtraContinuationByte) + "edf∃"},
                     {"chars", "∃"sv}}),
        Value("abc" + std::string(stringWithExtraContinuationByte) + std::string("edf"sv)));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim",
                        Document{{"input", "Abc" + std::string(stringWithExtraContinuationByte)},
                                 {"chars", "∃"sv}}),
                    Value("Abc" + std::string(stringWithExtraContinuationByte)));
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression(
            "$rtrim", Document{{"input", stringWithExtraContinuationByte}, {"chars", "∃"sv}}),
        Value(stringWithExtraContinuationByte));
}

TEST(ExpressionTrimTest, ShouldRetunNullIfInputIsNullish) {
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", BSONNULL}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", "$missingField"sv}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$trim", Document{{"input", BSONUndefined}}),
                    Value(BSONNULL));

    // Test with a chars argument provided.
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", BSONNULL}, {"chars", "∃"sv}}),
        Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", "$missingField"sv}, {"chars", "∃"sv}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", BSONUndefined}, {"chars", "∃"sv}}),
        Value(BSONNULL));

    // Test other variants of trim.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression("$ltrim", Document{{"input", BSONNULL}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", "$missingField"sv}, {"chars", "∃"sv}}),
                    Value(BSONNULL));
}

TEST(ExpressionTrimTest, ShouldRetunNullIfCharsIsNullish) {
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", " x "sv}, {"chars", BSONNULL}}),
        Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", " x "sv}, {"chars", "$missingField"sv}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", " x "sv}, {"chars", BSONUndefined}}),
        Value(BSONNULL));

    // Test other variants of trim.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", " x "sv}, {"chars", "$missingField"sv}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", " x "sv}, {"chars", BSONUndefined}}),
                    Value(BSONNULL));
}

TEST(ExpressionTrimTest, ShouldReturnNullIfBothCharsAndCharsAreNullish) {
    ASSERT_VALUE_EQ(
        evaluateNamedArgExpression("$trim", Document{{"input", BSONNULL}, {"chars", BSONNULL}}),
        Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", BSONUndefined}, {"chars", "$missingField"sv}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$trim", Document{{"input", "$missingField"sv}, {"chars", BSONUndefined}}),
                    Value(BSONNULL));

    // Test other variants of trim.
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$rtrim", Document{{"input", BSONNULL}, {"chars", "$missingField"sv}}),
                    Value(BSONNULL));
    ASSERT_VALUE_EQ(evaluateNamedArgExpression(
                        "$ltrim", Document{{"input", "$missingField"sv}, {"chars", BSONNULL}}),
                    Value(BSONNULL));
}

namespace concat {

intrusive_ptr<Expression> parseConcat(ExpressionContextForTest* expCtx, const BSONObj& spec) {
    VariablesParseState vps = expCtx->variablesParseState;
    return Expression::parseExpression(expCtx, spec, vps);
}

TEST(ExpressionConcatTest, TracksOutputMemoryAndReleasesAfterEvaluation) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = parseConcat(&expCtx, BSON("$concat" << BSON_ARRAY("$a"sv << "$b"sv)));

    SimpleMemoryUsageTracker tracker{1024};
    EvaluationContext ctx{.tracker = &tracker};

    Document doc{{"a", "hello"sv}, {"b", "world"sv}};
    ASSERT_VALUE_EQ(expr->evaluate(doc, &expCtx.variables, ctx), Value("helloworld"sv));

    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_GTE(tracker.peakTrackedMemoryBytes(), 10);
}

TEST(ExpressionConcatTest, FallbackTrackerWithinLimitDoesNotThrow) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = parseConcat(&expCtx, BSON("$concat" << BSON_ARRAY("$a"sv << "$b"sv)));

    const int64_t limit = 10 * 1024 * 1024;
    // Disable expression tracking so the fallback is standalone and enforces the per-expression cap
    unittest::ServerParameterGuard exprFlag{"featureFlagExpressionMemoryTracking", false};
    unittest::ServerParameterGuard limitGuard{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                              limit};

    EvaluationContext ctx{};
    Document doc{{"a", std::string(10, 'x')}, {"b", std::string(10, 'y')}};
    ASSERT_VALUE_EQ(expr->evaluate(doc, &expCtx.variables, ctx),
                    Value(std::string(10, 'x') + std::string(10, 'y')));

    // The fallback tracker recorded usage but stayed within the configured limit.
    auto& tracker = expCtx.getExpressionFallbackTracker();
    ASSERT_GT(tracker.peakTrackedMemoryBytes(), 0);
    ASSERT_LT(tracker.peakTrackedMemoryBytes(), limit);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
}

TEST(ExpressionConcatTest, FallbackTrackerEnforcesLimit) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = parseConcat(&expCtx, BSON("$concat" << BSON_ARRAY("$a"sv << "$b"sv)));

    const int64_t limit = 8;
    // Disable expression tracking so the fallback is standalone and enforces the per-expression cap
    unittest::ServerParameterGuard exprFlag{"featureFlagExpressionMemoryTracking", false};
    unittest::ServerParameterGuard limitGuard{"internalQueryMaxSingleExpressionMemoryUsageBytes",
                                              limit};

    EvaluationContext ctx{};
    Document doc{{"a", std::string(10, 'x')}, {"b", std::string(10, 'y')}};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$concat");
    }
    ASSERT_EQ(expCtx.getExpressionFallbackTracker().inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(expCtx.getExpressionFallbackTracker().peakTrackedMemoryBytes(), limit);
}

TEST(ExpressionConcatTest, ThrowsExceededMemoryLimitWhenQueryLimitExceeded) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = parseConcat(&expCtx, BSON("$concat" << BSON_ARRAY("$a"sv << "$b"sv)));

    const int64_t limit = 8;
    SimpleMemoryUsageTracker operationTracker{limit};
    SimpleMemoryUsageTracker stageTracker{&operationTracker, 100 * 1024 * 1024};
    EvaluationContext ctx{.tracker = &stageTracker};

    Document doc{{"a", std::string(10, 'x')}, {"b", std::string(10, 'y')}};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$concat");
    }
    ASSERT_EQ(operationTracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(operationTracker.peakTrackedMemoryBytes(), limit);
}

}  // namespace concat

}  // namespace expression_evaluation_test
}  // namespace mongo
