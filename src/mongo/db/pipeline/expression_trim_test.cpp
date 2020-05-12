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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/config.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace ExpressionTests {
namespace {
using boost::intrusive_ptr;
using std::string;

/**
 * Creates an expression which parses named arguments via an object specification, then evaluates it
 * and returns the result.
 */
static Value evaluateNamedArgExpression(const string& expressionName, const Document& operand) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON(expressionName << operand);
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Value result = expression->evaluate({}, &expCtx.variables);
    return result;
}

namespace Trim {

TEST(ExpressionTrimParsingTest, ThrowsIfSpecIsNotAnObject) {
    auto expCtx = ExpressionContextForTest{};

    ASSERT_THROWS(
        Expression::parseExpression(&expCtx, BSON("$trim" << 1), expCtx.variablesParseState),
        AssertionException);
    ASSERT_THROWS(Expression::parseExpression(
                      &expCtx, BSON("$trim" << BSON_ARRAY(1 << 2)), expCtx.variablesParseState),
                  AssertionException);
    ASSERT_THROWS(Expression::parseExpression(
                      &expCtx, BSON("$ltrim" << BSONNULL), expCtx.variablesParseState),
                  AssertionException);
    ASSERT_THROWS(Expression::parseExpression(&expCtx,
                                              BSON("$rtrim"
                                                   << "string"),
                                              expCtx.variablesParseState),
                  AssertionException);
}

TEST(ExpressionTrimParsingTest, ThrowsIfSpecDoesNotSpecifyInput) {
    auto expCtx = ExpressionContextForTest{};

    ASSERT_THROWS(Expression::parseExpression(
                      &expCtx, BSON("$trim" << BSONObj()), expCtx.variablesParseState),
                  AssertionException);
    ASSERT_THROWS(Expression::parseExpression(&expCtx,
                                              BSON("$ltrim" << BSON("chars"
                                                                    << "xyz")),
                                              expCtx.variablesParseState),
                  AssertionException);
}

TEST(ExpressionTrimParsingTest, ThrowsIfSpecContainsUnrecognizedField) {
    auto expCtx = ExpressionContextForTest{};

    ASSERT_THROWS(Expression::parseExpression(
                      &expCtx, BSON("$trim" << BSON("other" << 1)), expCtx.variablesParseState),
                  AssertionException);
    ASSERT_THROWS(Expression::parseExpression(&expCtx,
                                              BSON("$ltrim" << BSON("chars"
                                                                    << "xyz"
                                                                    << "other" << 1)),
                                              expCtx.variablesParseState),
                  AssertionException);
    ASSERT_THROWS(Expression::parseExpression(&expCtx,
                                              BSON("$rtrim" << BSON("input"
                                                                    << "$x"
                                                                    << "chars"
                                                                    << "xyz"
                                                                    << "other" << 1)),
                                              expCtx.variablesParseState),
                  AssertionException);
}

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
                                            BSON("$trim" << BSON("input"
                                                                 << "xxXXxx"
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

TEST(ExpressionTrimTest, DoesOptimizeToConstantWithNoChars) {
    auto expCtx = ExpressionContextForTest{};
    auto trim = Expression::parseExpression(&expCtx,
                                            BSON("$trim" << BSON("input"
                                                                 << " abc ")),
                                            expCtx.variablesParseState);
    auto optimized = trim->optimize();
    auto constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"_sd));

    // Test that it optimizes to a constant if the input also optimizes to a constant.
    trim = Expression::parseExpression(
        &expCtx,
        BSON("$trim" << BSON("input" << BSON("$concat" << BSON_ARRAY(" "
                                                                     << "abc ")))),
        expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"_sd));
}

TEST(ExpressionTrimTest, DoesOptimizeToConstantWithCustomChars) {
    auto expCtx = ExpressionContextForTest{};
    auto trim = Expression::parseExpression(&expCtx,
                                            BSON("$trim" << BSON("input"
                                                                 << " abc "
                                                                 << "chars"
                                                                 << " ")),
                                            expCtx.variablesParseState);
    auto optimized = trim->optimize();
    auto constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"_sd));

    // Test that it optimizes to a constant if the chars argument optimizes to a constant.
    trim = Expression::parseExpression(
        &expCtx,
        BSON("$trim" << BSON("input"
                             << "  abc "
                             << "chars" << BSON("$substrCP" << BSON_ARRAY("  " << 1 << 1)))),
        expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"_sd));

    // Test that it optimizes to a constant if both arguments optimize to a constant.
    trim = Expression::parseExpression(
        &expCtx,
        BSON("$trim" << BSON("input" << BSON("$concat" << BSON_ARRAY(" "
                                                                     << "abc "))
                                     << "chars"
                                     << BSON("$substrCP" << BSON_ARRAY("  " << 1 << 1)))),
        expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"_sd));
}

TEST(ExpressionTrimTest, DoesNotOptimizeToConstantWithFieldPaths) {
    auto expCtx = ExpressionContextForTest{};

    // 'input' is field path.
    auto trim = Expression::parseExpression(&expCtx,
                                            BSON("$trim" << BSON("input"
                                                                 << "$inputField")),
                                            expCtx.variablesParseState);
    auto optimized = trim->optimize();
    auto constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_FALSE(constant);

    // 'chars' is field path.
    trim = Expression::parseExpression(&expCtx,
                                       BSON("$trim" << BSON("input"
                                                            << " abc "
                                                            << "chars"
                                                            << "$secondInput")),
                                       expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_FALSE(constant);

    // Both are field paths.
    trim = Expression::parseExpression(&expCtx,
                                       BSON("$trim" << BSON("input"
                                                            << "$inputField"
                                                            << "chars"
                                                            << "$secondInput")),
                                       expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_FALSE(constant);
}

TEST(ExpressionTrimTest, DoesAddInputDependencies) {
    auto expCtx = ExpressionContextForTest{};

    auto trim = Expression::parseExpression(&expCtx,
                                            BSON("$trim" << BSON("input"
                                                                 << "$inputField")),
                                            expCtx.variablesParseState);
    DepsTracker deps;
    trim->addDependencies(&deps);
    ASSERT_EQ(deps.fields.count("inputField"), 1u);
    ASSERT_EQ(deps.fields.size(), 1u);
}

TEST(ExpressionTrimTest, DoesAddCharsDependencies) {
    auto expCtx = ExpressionContextForTest{};

    auto trim = Expression::parseExpression(&expCtx,
                                            BSON("$trim" << BSON("input"
                                                                 << "$inputField"
                                                                 << "chars"
                                                                 << "$$CURRENT.a")),
                                            expCtx.variablesParseState);
    DepsTracker deps;
    trim->addDependencies(&deps);
    ASSERT_EQ(deps.fields.count("inputField"), 1u);
    ASSERT_EQ(deps.fields.count("a"), 1u);
    ASSERT_EQ(deps.fields.size(), 2u);
}

TEST(ExpressionTrimTest, DoesSerializeCorrectly) {
    auto expCtx = ExpressionContextForTest{};

    auto trim = Expression::parseExpression(&expCtx,
                                            BSON("$trim" << BSON("input"
                                                                 << " abc ")),
                                            expCtx.variablesParseState);
    ASSERT_VALUE_EQ(trim->serialize(false), trim->serialize(true));
    ASSERT_VALUE_EQ(
        trim->serialize(false),
        Value(Document{{"$trim", Document{{"input", Document{{"$const", " abc "_sd}}}}}}));

    // Make sure we can re-parse it and evaluate it.
    auto reparsedTrim = Expression::parseExpression(
        &expCtx, trim->serialize(false).getDocument().toBson(), expCtx.variablesParseState);
    ASSERT_VALUE_EQ(reparsedTrim->evaluate({}, &expCtx.variables), Value("abc"_sd));

    // Use $ltrim, and specify the 'chars' option.
    trim = Expression::parseExpression(&expCtx,
                                       BSON("$ltrim" << BSON("input"
                                                             << "$inputField"
                                                             << "chars"
                                                             << "$$CURRENT.a")),
                                       expCtx.variablesParseState);
    ASSERT_VALUE_EQ(
        trim->serialize(false),
        Value(Document{{"$ltrim", Document{{"input", "$inputField"_sd}, {"chars", "$a"_sd}}}}));

    // Make sure we can re-parse it and evaluate it.
    reparsedTrim = Expression::parseExpression(
        &expCtx, trim->serialize(false).getDocument().toBson(), expCtx.variablesParseState);
    ASSERT_VALUE_EQ(reparsedTrim->evaluate(Document{{"inputField", " , 4"_sd}, {"a", " ,"_sd}},
                                           &expCtx.variables),
                    Value("4"_sd));
}
}  // namespace Trim

}  // namespace
}  // namespace ExpressionTests
}  // namespace mongo
