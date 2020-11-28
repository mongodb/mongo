/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/decimal128.h"
#include <string>

#include "mongo/bson/json.h"
#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/cst/parser_gen.hpp"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CstMatchTest, ParsesEmptyPredicate) {
    CNode output;
    auto input = fromjson("{filter: {}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(), "{}");
}

TEST(CstMatchTest, ParsesEqualityPredicates) {
    CNode output;
    auto input = fromjson("{filter: {a: 5.0, b: NumberInt(10), _id: NumberLong(15)}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname a>: \"<UserDouble 5.000000>\", <UserFieldname b>: \"<UserInt "
              "10>\", <UserFieldname _id>: \"<UserLong 15>\" }");
}

TEST(CstMatchTest, ParsesLogicalOperatorsWithOneChild) {
    {
        CNode output;
        auto input = fromjson("{filter: {$and: [{a: 1}]}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname andExpr>: [ { <UserFieldname a>: \"<UserInt 1>\" } ] }");
    }
    {
        CNode output;
        auto input = fromjson("{filter: {$or: [{a: 1}]}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname orExpr>: [ { <UserFieldname a>: \"<UserInt 1>\" } ] }");
    }
    {
        CNode output;
        auto input = fromjson("{filter: {$nor: [{a: 1}]}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname norExpr>: [ { <UserFieldname a>: \"<UserInt 1>\" } ] }");
    }
}

TEST(CstMatchTest, ParsesLogicalOperatorsWithMultipleChildren) {
    {
        CNode output;
        auto input = fromjson("{filter: {$and: [{a: 1}, {b: 'bee'}]}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname andExpr>: [ { <UserFieldname a>: \"<UserInt 1>\" }, { "
                  "<UserFieldname b>: \"<UserString bee>\" } ] }");
    }
    {
        CNode output;
        auto input = fromjson("{filter: {$or: [{a: 1}, {b: 'bee'}]}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname orExpr>: [ { <UserFieldname a>: \"<UserInt 1>\" }, { "
                  "<UserFieldname b>: \"<UserString bee>\" } ] }");
    }
    {
        CNode output;
        auto input = fromjson("{filter: {$nor: [{a: 1}, {b: 'bee'}]}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname norExpr>: [ { <UserFieldname a>: \"<UserInt 1>\" }, { "
                  "<UserFieldname b>: \"<UserString bee>\" } ] }");
    }
}

TEST(CstMatchTest, ParsesNotWithRegex) {
    CNode output;
    auto input = fromjson("{filter: {a: {$not: /^a/}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname a>: { <KeyFieldname notExpr>: \"<UserRegex /^a/>\" } }");
}

TEST(CstMatchTest, ParsesNotWithChildExpression) {
    CNode output;
    auto input = fromjson("{filter: {a: {$not: {$not: /^a/}}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname a>: { <KeyFieldname notExpr>: { <KeyFieldname notExpr>: "
              "\"<UserRegex /^a/>\" } } }");
}

TEST(CstMatchTest, ParsesExistsWithSimpleValueChild) {
    CNode output;
    auto input = fromjson("{filter: {a: {$exists: true}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname a>: { <KeyFieldname existsExpr>: \"<UserBoolean true>\" } }");
}

TEST(CstMatchTest, ParsesExistsWithCompoundValueChild) {
    CNode output;
    auto input = fromjson("{filter: {a: {$exists: [\"hello\", 5, true, \"goodbye\"]}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname a>: { <KeyFieldname existsExpr>: [ \"<UserString hello>\", "
              "\"<UserInt 5>\", "
              "\"<UserBoolean true>\", \"<UserString goodbye>\" ] } }");
}

TEST(CstMatchTest, ParsesExistsWithObjectChild) {
    CNode output;
    auto input = fromjson("{filter: {a: {$exists: {a: false }}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname a>: { <KeyFieldname existsExpr>: { <UserFieldname a>: "
              "\"<UserBoolean false>\" } } }");
}

TEST(CstMatchTest, ParsesTypeSingleArgument) {
    // Check that $type parses with a string argument - a BSON type alias
    {
        CNode output;
        auto input = fromjson("{filter: {a: {$type: \"bool\" }}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <UserFieldname a>: { <KeyFieldname type>: \"<UserString bool>\" } }");
    }
    // Check that $type parses a number (corresponding to a BSON type)
    {
        CNode output;
        auto input = fromjson("{filter: {a: {$type: 1}}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <UserFieldname a>: { <KeyFieldname type>: \"<UserInt 1>\" } }");
    }
}

TEST(CstMatchTest, ParsersTypeArrayArgument) {
    CNode output;
    auto input = fromjson("{filter: {a: {$type: [\"number\", 5, 127, \"objectId\"]}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname a>: { <KeyFieldname type>: [ \"<UserString number>\", \"<UserInt "
              "5>\", \"<UserInt 127>\", "
              "\"<UserString objectId>\" ] } }");
}

TEST(CstMatchTest, ParsesCommentWithSimpleValueChild) {
    CNode output;
    auto input = fromjson("{filter: {a: 1, $comment: true}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname a>: \"<UserInt 1>\", <KeyFieldname commentExpr>: \"<UserBoolean "
              "true>\" }");
}

TEST(CstMatchTest, ParsesCommentWithCompoundValueChild) {
    CNode output;
    auto input = fromjson("{filter: {a: 1, $comment: [\"hi\", 5]}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname a>: \"<UserInt 1>\", <KeyFieldname commentExpr>: [ \"<UserString "
              "hi>\", \"<UserInt 5>\" ] }");
}

TEST(CstMatchTest, ParsesExpr) {
    CNode output;
    auto input = fromjson("{filter: {$expr: 123}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(), "{ <KeyFieldname expr>: \"<UserInt 123>\" }");
}

TEST(CstMatchTest, ParsesText) {
    CNode output;
    auto input = fromjson("{filter: {$text: {$search: \"abc\"}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <KeyFieldname text>: { "
              "<KeyFieldname caseSensitive>: \"<KeyValue absentKey>\", "
              "<KeyFieldname diacriticSensitive>: \"<KeyValue absentKey>\", "
              "<KeyFieldname language>: \"<KeyValue absentKey>\", "
              "<KeyFieldname search>: \"<UserString abc>\" } }");
}

TEST(CstMatchTest, ParsesTextOptions) {
    CNode output;
    auto input = fromjson(
        "{filter: {$text: {"
        "$search: \"abc\", "
        "$caseSensitive: true, "
        "$diacriticSensitive: true, "
        "$language: \"asdfzxcv\" } } }");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <KeyFieldname text>: { "
              "<KeyFieldname caseSensitive>: \"<UserBoolean true>\", "
              "<KeyFieldname diacriticSensitive>: \"<UserBoolean true>\", "
              "<KeyFieldname language>: \"<UserString asdfzxcv>\", "
              "<KeyFieldname search>: \"<UserString abc>\" } }");
}

TEST(CstMatchTest, ParsesWhere) {
    CNode output;
    auto input = fromjson("{filter: {$where: \"return true;\"}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <KeyFieldname where>: \"<UserString return true;>\" }");
}

TEST(CstMatchTest, FailsToParseNotWithNonObject) {
    CNode output;
    auto input = fromjson("{filter: {a: {$not: 1}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    ASSERT_THROWS_CODE_AND_WHAT(ParserGen(lexer, nullptr).parse(),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "syntax error, unexpected 1 (int), expecting object or regex at "
                                "element '1' within '$not' of input filter");
}

TEST(CstMatchTest, FailsToParseUnknownOperatorWithinNotExpression) {
    CNode output;
    auto input = fromjson("{filter: {a: {$not: {$and: [{a: 1}]}}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    ASSERT_THROWS_CODE(
        ParserGen(lexer, nullptr).parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstMatchTest, FailsToParseNotWithEmptyObject) {
    CNode output;
    auto input = fromjson("{filter: {a: {$not: {}}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    ASSERT_THROWS_CODE(
        ParserGen(lexer, nullptr).parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstMatchTest, FailsToParseDollarPrefixedPredicates) {
    {
        auto input = fromjson("{filter: {$atan2: [3, 5]}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        ASSERT_THROWS_CODE_AND_WHAT(
            ParserGen(lexer, nullptr).parse(),
            AssertionException,
            ErrorCodes::FailedToParse,
            "syntax error, unexpected ATAN2 at element '$atan2' of input filter");
    }
    {
        auto input = fromjson("{filter: {$prefixed: 5}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        ASSERT_THROWS_CODE_AND_WHAT(
            ParserGen(lexer, nullptr).parse(),
            AssertionException,
            ErrorCodes::FailedToParse,
            "syntax error, unexpected $-prefixed fieldname at element '$prefixed' of input filter");
    }
    {
        auto input = fromjson("{filter: {$$ROOT: 5}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        ASSERT_THROWS_CODE_AND_WHAT(
            ParserGen(lexer, nullptr).parse(),
            AssertionException,
            ErrorCodes::FailedToParse,
            "syntax error, unexpected $-prefixed fieldname at element '$$ROOT' of input filter");
    }
}

TEST(CstMatchTest, FailsToParseDollarPrefixedPredicatesWithinLogicalExpression) {
    auto input = fromjson("{filter: {$and: [{$prefixed: 5}]}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    ASSERT_THROWS_CODE_AND_WHAT(
        ParserGen(lexer, nullptr).parse(),
        AssertionException,
        ErrorCodes::FailedToParse,
        "syntax error, unexpected $-prefixed fieldname at element '$prefixed' within array at "
        "index 0 within '$and' of input filter");
}

TEST(CstMatchTest, FailsToParseNonArrayLogicalKeyword) {
    auto input = fromjson("{filter: {$and: {a: 5}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    ASSERT_THROWS_CODE_AND_WHAT(ParserGen(lexer, nullptr).parse(),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "syntax error, unexpected object, expecting array at element "
                                "'start object' within '$and' of input filter");
}

TEST(CstMatchTest, FailsToParseNonObjectWithinLogicalKeyword) {
    auto input = fromjson("{filter: {$or: [5]}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    ASSERT_THROWS_CODE_AND_WHAT(ParserGen(lexer, nullptr).parse(),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "syntax error, unexpected arbitrary integer, expecting object at "
                                "element '5' within array at index 0 within '$or' of input filter");
}

TEST(CstMatchTest, FailsToParseLogicalKeywordWithEmptyArray) {
    auto input = fromjson("{filter: {$nor: []}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    ASSERT_THROWS_CODE_AND_WHAT(ParserGen(lexer, nullptr).parse(),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "syntax error, unexpected end of array, expecting object at "
                                "element 'end array' within '$nor' of input filter");
}

TEST(CstMatchTest, FailsToParseTypeWithBadSpecifier) {
    // Shouldn't parse if the number given isn't a valid BSON type specifier.
    {
        auto input = fromjson("{filter: {a: {$type: 0}}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        ASSERT_THROWS_CODE(
            ParserGen(lexer, nullptr).parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    // Shouldn't parse if the string given isn't a valid BSON type alias.
    {
        auto input = fromjson("{filter: {$type: {a: \"notABsonType\"}}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        ASSERT_THROWS_CODE(
            ParserGen(lexer, nullptr).parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    // Shouldn't parse if any argument isn't a valid BSON type alias.
    {
        auto input = fromjson("{filter: {a: {$type: [1, \"number\", \"notABsonType\"]}}}");
        BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
        ASSERT_THROWS_CODE(
            ParserGen(lexer, nullptr).parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstMatchTest, ParsesMod) {
    CNode output;
    auto input = fromjson("{filter: {a: {$mod: [3, 2.0]}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname a>: { <KeyFieldname matchMod>: ["
              " \"<UserInt 3>\", \"<UserDouble 2.000000>\" ] } }");
}

TEST(CstMatchTest, FailsToParseModWithEmptyArray) {
    auto input = fromjson("{filter: {a: {$mod: []}}}");
    BSONLexer lexer(input["filter"].embeddedObject(), ParserGen::token::START_MATCH);
    ASSERT_THROWS_CODE_AND_WHAT(ParserGen(lexer, nullptr).parse(),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "syntax error, unexpected end of array at "
                                "element 'end array' within '$mod' of input filter");
}

}  // namespace
}  // namespace mongo
