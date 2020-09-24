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

#include <string>

#include "mongo/bson/json.h"
#include "mongo/db/cst/bson_lexer.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/cst/parser_gen.hpp"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CstProjectTest, ParsesEmptyProjection) {
    CNode output;
    auto input = fromjson("{project: {}}");
    BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(), "{ <KeyFieldname projectInclusion>: {} }");
}

TEST(CstProjectTest, ParsesBasicProjection) {
    {
        CNode output;
        auto input = fromjson(
            "{project: {a: 1.0, b: {c: NumberInt(1), d: NumberDecimal('1.0') }, _id: "
            "NumberLong(1)}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(
            output.toBson().toString(),
            "{ <KeyFieldname projectInclusion>: { <ProjectionPath a>: \"<NonZeroKey of type double "
            "1.000000>\", <ProjectionPath b>: { "
            "<CompoundInclusionKey>: { <ProjectionPath c>: \"<NonZeroKey of type int 1>\", "
            "<ProjectionPath d>: \"<NonZeroKey "
            "of type decimal 1.00000000000000>\" } }, <KeyFieldname id>: \"<NonZeroKey of type "
            "long 1>\" } }");
    }
    {
        CNode output;
        auto input = fromjson(
            "{project: {_id: 9.10, a: {$add: [4, 5, {$add: [6, 7, 8]}]}, b: {$atan2: "
            "[1.0, {$add: [2, -3]}]}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(
            output.toBson().toString(),
            "{ <KeyFieldname projectInclusion>: { <KeyFieldname id>: \"<NonZeroKey of type double "
            "9.100000>\", <ProjectionPath a>: { <KeyFieldname add>: [ \"<UserInt 4>\", \"<UserInt "
            "5>\", { <KeyFieldname add>: [ \"<UserInt 6>\", \"<UserInt 7>\", \"<UserInt 8>\" ] } ] "
            "}, <ProjectionPath b>: { <KeyFieldname atan2>: [ \"<UserDouble 1.000000>\", { "
            "<KeyFieldname add>: [ \"<UserInt 2>\", \"<UserInt -3>\" ] } ] } } }");
    }
    {
        CNode output;
        auto input = fromjson("{project: {a: {$add: [6]}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(
            output.toBson().toString(),
            "{ <KeyFieldname projectInclusion>: { <ProjectionPath a>: { <KeyFieldname add>: [ "
            "\"<UserInt 6>\" ] } } }");
    }
}

TEST(CstProjectTest, ParsesCompoundProjection) {
    {
        CNode output;
        auto input = fromjson("{project: {a: 0.0, b: NumberInt(0), c: { d: { e: NumberLong(0)}}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname projectExclusion>: { <ProjectionPath a>: \"<KeyValue "
                  "doubleZeroKey>\", <ProjectionPath b>: \"<KeyValue intZeroKey>\", "
                  "<ProjectionPath c>: { <CompoundExclusionKey>: { <ProjectionPath d>: { "
                  "<ProjectionPath e>: \"<KeyValue longZeroKey>\" } } } } }");
    }
    {
        CNode output;
        auto input = fromjson("{project: {a: 0.0, b: NumberInt(0), \"c.d.e\": NumberLong(0)}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname projectExclusion>: { <ProjectionPath a>: \"<KeyValue "
                  "doubleZeroKey>\", <ProjectionPath b>: \"<KeyValue intZeroKey>\", "
                  "<ProjectionPath c.d.e>: \"<KeyValue longZeroKey>\" } }");
    }
    {
        CNode output;
        auto input = fromjson("{project: {a: 1.1, b: NumberInt(1), c: { \"d.e\": NumberLong(1)}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname projectInclusion>: { <ProjectionPath a>: \"<NonZeroKey of type "
                  "double 1.100000>\", <ProjectionPath b>: \"<NonZeroKey of type int 1>\", "
                  "<ProjectionPath c>: { <CompoundInclusionKey>: { <ProjectionPath d.e>: "
                  "\"<NonZeroKey of type long 1>\" } } } }");
    }
}

TEST(CstProjectTest, ParsesPositonalProjection) {
    {
        CNode output;
        auto input = fromjson("{project: {\"a.$\": 1}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname projectInclusion>: { <PositionalProjectionPath a>: "
                  "\"<NonZeroKey of type int 1>\" } }");
    }
    {
        CNode output;
        auto input = fromjson("{project: {\"a.b.c.$\": 1.0}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname projectInclusion>: { <PositionalProjectionPath a.b.c>: "
                  "\"<NonZeroKey of type double 1.000000>\" } }");
    }
}

TEST(CstProjectTest, ParsesElemMatch) {
    {
        CNode output;
        auto input = fromjson("{project: {\"a.b\": {$elemMatch: {c: 12}}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname projectInclusion>: { <ProjectionPath a.b>: { <KeyFieldname "
                  "elemMatch>: "
                  "{ <UserFieldname c>: \"<UserInt 12>\" } } } }");
    }
    {
        CNode output;
        auto input = fromjson(
            "{project: {\"a.b\": {$elemMatch: {c: 22}}, \"c.d\": {$tan: {$add: [4, 9]}}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        ASSERT_EQ(output.toBson().toString(),
                  "{ <KeyFieldname projectInclusion>: { <ProjectionPath a.b>: { <KeyFieldname "
                  "elemMatch>: { <UserFieldname c>: \"<UserInt 22>\" } }, <ProjectionPath c.d>: { "
                  "<KeyFieldname tan>: { <KeyFieldname add>: [ \"<UserInt 4>\", \"<UserInt 9>\" ] "
                  "} } } }");
    }
}

TEST(CstProjectTest, ParsesMeta) {
    CNode output;
    auto input = fromjson("{project: {\"a.b.c\": {$meta: \"textScore\"}}}");
    BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath a.b.c>: { <KeyFieldname meta>: "
              "\"<KeyValue textScore>\" } } }");
}

TEST(CstProjectTest, ParsesSlice) {
    CNode output;
    auto input = fromjson("{project: {\"a.b.c.d\": {$slice: 14}}}");
    BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath a.b.c.d>: { <KeyFieldname "
              "slice>: \"<UserInt 14>\" } } }");
}

TEST(CstGrammarTest, FailsToParseDottedPathBelowProjectOuterObjects) {
    CNode output;
    auto input = fromjson("{project: {a: [{b: 5}, {\"c.d\": 7}]}}");
    BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, FailsToParseRedundantPaths) {
    {
        CNode output;
        auto input = fromjson("{project: {a: {b: 1}, \"a.b\": 1}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{project: {a: {b: {c: {$atan2: [1, 0]}}, \"b.c\": 1}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, FailsToParsePrefixPaths) {
    {
        CNode output;
        auto input = fromjson("{project: {a: 1, \"a.b\": 1}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{project: {a: {b: {c: {d: {$atan2: [1, 0]}}}, \"b.c\": 1}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, FailsToParseMixedProject) {
    {
        CNode output;
        auto input = fromjson("{project: {a: 1, b: 0.0}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{project: {a: 0, b: {$add: [5, 67]}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, FailsToParseCompoundMixedProject) {
    {
        CNode output;
        auto input = fromjson("{project: {a: {b: 1, c: 0.0}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{project: {a: {b: {c: {d: NumberLong(0)}, e: 45}}}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, FailsToParseProjectWithDollarFieldNames) {
    {
        CNode output;
        auto input = fromjson("{project: {$a: 1}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{project: {b: 1, $a: 1}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{project: {b: 1, $add: 1, c: 1}}");
        BSONLexer lexer(input["project"].embeddedObject(), ParserGen::token::START_PROJECT);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

}  // namespace
}  // namespace mongo
