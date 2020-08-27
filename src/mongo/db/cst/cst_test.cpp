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
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CstGrammarTest, BuildsAndPrints) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::atan2,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserDouble{2.0}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname atan2>\": [\"<UserDouble 3.000000>\", "
                                   "\"<UserDouble 2.000000>\"]}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::projectInclusion,
             CNode{CNode::ObjectChildren{
                 {ProjectionPath{make_vector<std::string>("a")}, CNode{KeyValue::trueKey}},
                 {KeyFieldname::id, CNode{KeyValue::falseKey}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname projectInclusion>\": {\"<ProjectionPath a>\": \"<KeyValue "
                     "trueKey>\", \"<KeyFieldname id>\": \"<KeyValue falseKey>\"}}"),
            cst.toBson());
    }
}

TEST(CstGrammarTest, EmptyPipeline) {
    CNode output;
    auto input = fromjson("{pipeline: []}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_TRUE(stdx::get_if<CNode::ArrayChildren>(&output.payload));
    ASSERT_EQ(0, stdx::get_if<CNode::ArrayChildren>(&output.payload)->size());
}

TEST(CstGrammarTest, ParsesInternalInhibitOptimization) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$_internalInhibitOptimization: {}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::inhibitOptimization == stages[0].firstKeyFieldname());
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$_internalInhibitOptimization: 'invalid'}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, ParsesUnionWith) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$unionWith: {coll: 'hey', pipeline: 1.0}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::unionWith == stages[0].firstKeyFieldname());
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$unionWith: {pipeline: 1.0, coll: 'hey'}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::unionWith == stages[0].firstKeyFieldname());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ <KeyFieldname unionWith>: { <KeyFieldname collArg>: \"<UserString hey>\", "
                  "<KeyFieldname pipelineArg>: \"<UserDouble "
                  "1.000000>\" } }");
    }
}

TEST(CstGrammarTest, ParseSkipInt) {
    CNode output;
    auto input = fromjson("{pipeline: [{$skip: 5}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::skip == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname skip>\": \"<UserInt 5>\" }"), stages[0].toBson());
}

TEST(CstGrammarTest, ParseSkipDouble) {
    CNode output;
    auto input = fromjson("{pipeline: [{$skip: 1.5}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::skip == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname skip>\": \"<UserDouble 1.500000>\" }"),
                      stages[0].toBson());
}

TEST(CstGrammarTest, ParseSkipLong) {
    CNode output;
    auto input = fromjson("{pipeline: [{$skip: 8223372036854775807}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::skip == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname skip>\": \"<UserLong 8223372036854775807>\" }"),
                      stages[0].toBson());
}

TEST(CstGrammarTest, InvalidParseSkipObject) {
    CNode output;
    auto input = fromjson("{pipeline: [{$skip: {}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, InvalidParseSkipString) {
    CNode output;
    auto input = fromjson("{pipeline: [{$skip: '5'}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, ParsesLimitInt) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: 5}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::limit == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname limit>\": \"<UserInt 5>\"}"), stages[0].toBson());
}

TEST(CstGrammarTest, ParsesLimitDouble) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: 5.0}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::limit == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname limit>\": \"<UserDouble 5.000000>\"}"),
                      stages[0].toBson());
}

TEST(CstGrammarTest, ParsesLimitLong) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: 123123123123}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::limit == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname limit>\": \"<UserLong 123123123123>\"}"),
                      stages[0].toBson());
}

TEST(CstGrammarTest, InvalidParseLimitString) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: \"5\"}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, InvalidParseLimitObject) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: {}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, InvalidParseLimitArray) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: [2]}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, ParsesProject) {
    {
        CNode output;
        auto input = fromjson(
            "{pipeline: [{$project: {a: 1.0, b: {c: NumberInt(1), d: NumberDecimal('1.0') }, _id: "
            "NumberLong(1)}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(
            stages[0].toBson().toString(),
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
            "{pipeline: [{$project: {_id: 9.10, a: {$add: [4, 5, {$add: [6, 7, 8]}]}, b: "
            "{$atan2: "
            "[1.0, {$add: [2, -3]}]}}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(
            stages[0].toBson().toString(),
            "{ <KeyFieldname projectInclusion>: { <KeyFieldname id>: \"<NonZeroKey of type "
            "double 9.100000>\", <ProjectionPath a>: { <KeyFieldname add>: [ "
            "{ <KeyFieldname add>: [ \"<UserInt 8>\", \"<UserInt 7>\", \"<UserInt 6>\" ] }, "
            "\"<UserInt 5>\", "
            "\"<UserInt 4>\" ] }, <ProjectionPath b>: { <KeyFieldname atan2>: [ \"<UserDouble "
            "1.000000>\", { <KeyFieldname add>: [ \"<UserInt -3>\", \"<UserInt 2>\" ] } ] } } }");
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: {$add: [6]}}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(
            stages[0].toBson().toString(),
            "{ <KeyFieldname projectInclusion>: { <ProjectionPath a>: { <KeyFieldname add>: [ "
            "\"<UserInt 6>\" ] } } }");
    }
}

TEST(CstGrammarTest, ParsesCompoundProject) {
    {
        CNode output;
        auto input = fromjson(
            "{pipeline: [{$project: {a: 0.0, b: NumberInt(0), c: { d: { e: NumberLong(0)}}}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectExclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ <KeyFieldname projectExclusion>: { <ProjectionPath a>: \"<KeyValue "
                  "doubleZeroKey>\", <ProjectionPath b>: \"<KeyValue intZeroKey>\", "
                  "<ProjectionPath c>: { <CompoundExclusionKey>: { <ProjectionPath d>: { "
                  "<ProjectionPath e>: \"<KeyValue longZeroKey>\" } } } } }");
    }
    {
        CNode output;
        auto input = fromjson(
            "{pipeline: [{$project: {a: 0.0, b: NumberInt(0), \"c.d.e\": NumberLong(0)}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectExclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ <KeyFieldname projectExclusion>: { <ProjectionPath a>: \"<KeyValue "
                  "doubleZeroKey>\", <ProjectionPath b>: \"<KeyValue intZeroKey>\", "
                  "<ProjectionPath c.d.e>: \"<KeyValue longZeroKey>\" } }");
    }
    {
        CNode output;
        auto input = fromjson(
            "{pipeline: [{$project: {a: 1.1, b: NumberInt(1), c: { \"d.e\": NumberLong(1)}}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ <KeyFieldname projectInclusion>: { <ProjectionPath a>: \"<NonZeroKey of type "
                  "double 1.100000"
                  ">\", <ProjectionPath b>: \"<NonZeroKey of type int 1>\", "
                  "<ProjectionPath c>: { <CompoundInclusionKey>: { <ProjectionPath d.e>: "
                  "\"<NonZeroKey of type long 1>\" } } } }");
    }
}

TEST(CstGrammarTest, FailsToParseDottedPathBelowProjectOuterObjects) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {a: [{b: 5}, {\"c.d\": 7}]}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, FailsToParseRedundantPaths) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: {b: 1}, \"a.b\": 1}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input =
            fromjson("{pipeline: [{$project: {a: {b: {c: {$atan2: [1, 0]}}, \"b.c\": 1}}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}
TEST(CstGrammarTest, FailsToParsePrefixPaths) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: 1, \"a.b\": 1}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input =
            fromjson("{pipeline: [{$project: {a: {b: {c: {d: {$atan2: [1, 0]}}}, \"b.c\": 1}}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, FailsToParseMixedProject) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: 1, b: 0.0}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: 0, b: {$add: [5, 67]}}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, FailsToParseCompoundMixedProject) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: {b: 1, c: 0.0}}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: {b: {c: {d: NumberLong(0)}, e: 45}}}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, FailsToParseProjectWithDollarFieldNames) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {$a: 1}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {b: 1, $a: 1}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, BuildsAndPrintsAnd) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::andExpr,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserString{"green"}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname andExpr>\": [\"<UserDouble 3.000000>\", "
                                   "\"<UserString green>\"]}"),
                          cst.toBson());
    }
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::andExpr, CNode{CNode::ArrayChildren{}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname andExpr>\": []}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::andExpr,
             CNode{CNode::ArrayChildren{
                 CNode{UserDouble{3.0}}, CNode{UserInt{2}}, CNode{UserDouble{5.0}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname andExpr>\": [\"<UserDouble 3.000000>\", \"<UserInt 2>\", "
                     "\"<UserDouble 5.000000>\"]}"),
            cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::andExpr,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserInt{2}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname andExpr>\": [\"<UserDouble 3.000000>\", \"<UserInt 2>\"]}"),
            cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::andExpr,
             CNode{CNode::ArrayChildren{CNode{UserInt{0}}, CNode{UserBoolean{true}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname andExpr>\": [\"<UserInt 0>\", \"<UserBoolean 1>\"]}"),
            cst.toBson());
    }
}

TEST(CstGrammarTest, BuildsAndPrintsOr) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::orExpr,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserString{"green"}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson(
                "{\"<KeyFieldname orExpr>\": [\"<UserDouble 3.000000>\", \"<UserString green>\"]}"),
            cst.toBson());
    }
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::orExpr, CNode{CNode::ArrayChildren{}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname orExpr>\": []}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::orExpr,
             CNode{CNode::ArrayChildren{
                 CNode{UserDouble{3.0}}, CNode{UserInt{2}}, CNode{UserDouble{5.0}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname orExpr>\": [\"<UserDouble 3.000000>\", \"<UserInt 2>\", "
                     "\"<UserDouble 5.000000>\"]}"),
            cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::orExpr,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserInt{2}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname orExpr>\": [\"<UserDouble 3.000000>\", \"<UserInt 2>\"]}"),
            cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::orExpr,
             CNode{CNode::ArrayChildren{CNode{UserInt{0}}, CNode{UserBoolean{true}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname orExpr>\": [\"<UserInt 0>\", \"<UserBoolean 1>\"]}"),
            cst.toBson());
    }
}

TEST(CstGrammarTest, BuildsAndPrintsNot) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::notExpr, CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname notExpr>\": [\"<UserDouble 3.000000>\"]}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::notExpr, CNode{CNode::ArrayChildren{CNode{UserBoolean{true}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname notExpr>\": [\"<UserBoolean 1>\"]}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::notExpr, CNode{CNode::ArrayChildren{CNode{UserBoolean{false}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname notExpr>\": [\"<UserBoolean 0>\"]}"),
                          cst.toBson());
    }
}

TEST(CstGrammarTest, ParsesSampleWithNumericSizeArgument) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: NumberInt(1)}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ <KeyFieldname sample>: { <KeyFieldname sizeArg>: \"<UserInt 1>\" } }");
    }
    {
        // Although negative numbers are not valid, this is enforced at translation time.
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: NumberInt(-1)}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ <KeyFieldname sample>: { <KeyFieldname sizeArg>: \"<UserInt -1>\" } }");
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: NumberLong(5)}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ <KeyFieldname sample>: { <KeyFieldname sizeArg>: \"<UserLong 5>\" } }");
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: 10.0}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT_EQ(
            stages[0].toBson().toString(),
            "{ <KeyFieldname sample>: { <KeyFieldname sizeArg>: \"<UserDouble 10.000000>\" } }");
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: 0}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ <KeyFieldname sample>: { <KeyFieldname sizeArg>: \"<UserInt 0>\" } }");
    }
}

TEST(CstGrammarTest, InvalidParseSample) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: 2}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {notSize: 2}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: 'gots ta be a number'}}]}");
        BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, BuildsAndPrintsConvert) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::convert,
             CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{UserInt{3}}},
                                         {KeyFieldname::toArg, CNode{UserString{"string"}}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname convert>\": {\"<KeyFieldname inputArg>\": \"<UserInt 3>\", "
                     "\"<KeyFieldname toArg>\": \"<UserString string>\"}}"),
            cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::convert,
             CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{CNode::ArrayChildren{}}},
                                         {KeyFieldname::toArg, CNode{UserInt{8}}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname convert>\": {\"<KeyFieldname inputArg>\": [], "
                                   "\"<KeyFieldname toArg>\": \"<UserInt 8>\"}}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::convert,
             CNode{CNode::ObjectChildren{
                 {KeyFieldname::inputArg,
                  CNode{CNode::ObjectChildren{
                      {KeyFieldname::add,
                       CNode{CNode::ArrayChildren{CNode{UserInt{4}}, CNode{UserInt{5}}}}}}}},
                 {KeyFieldname::toArg, CNode{UserInt{1}}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname convert>\": {\"<KeyFieldname inputArg>\": "
                                   "{\"<KeyFieldname add>\": [\"<UserInt 4>\", \"<UserInt 5>\"]}, "
                                   "\"<KeyFieldname toArg>\": \"<UserInt 1>\"}}"),
                          cst.toBson());
    }
}

TEST(CstGrammarTest, BuildsAndPrintsToBool) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toBool, CNode{UserString{"a"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toBool>\": \"<UserString a>\"}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::toBool, CNode{UserNull{}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toBool>\": \"<UserNull>\"}"), cst.toBson());
    }
}

TEST(CstGrammarTest, BuildsAndPrintsToDate) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toDate, CNode{UserString{"2018-03-03"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toDate>\": \"<UserString 2018-03-03>\"}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::toDate, CNode{UserObjectId{"5ab9c3da31c2ab715d421285"}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname toDate>\": \"<UserObjectId 5ab9c3da31c2ab715d421285>\"}"),
            cst.toBson());
    }
}

TEST(CstGrammarTest, BuildsAndPrintsToDecimal) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toDecimal, CNode{UserBoolean{false}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toDecimal>\": \"<UserBoolean 0>\"}"),
                          cst.toBson());
    }
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toDecimal, CNode{UserString{"-5.5"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toDecimal>\": \"<UserString -5.5>\"}"),
                          cst.toBson());
    }
}

TEST(CstGrammarTest, BuildsAndPrintsToDouble) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toDouble, CNode{UserBoolean{true}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toDouble>\": \"<UserBoolean 1>\"}"),
                          cst.toBson());
    }
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toDouble, CNode{UserLong{10000}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toDouble>\": \"<UserLong 10000>\"}"),
                          cst.toBson());
    }
}

TEST(CstGrammarTest, BuildsAndPrintsToInt) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toInt, CNode{UserString{"-2"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toInt>\": \"<UserString -2>\"}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{
            CNode::ObjectChildren{{KeyFieldname::toInt, CNode{UserDecimal{5.50000000000000}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname toInt>\": \"<UserDecimal 5.50000000000000>\"}"),
            cst.toBson());
    }
}

TEST(CstGrammarTest, BuildsAndPrintsToLong) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toLong, CNode{UserString{"-2"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toLong>\": \"<UserString -2>\"}"),
                          cst.toBson());
    }
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toLong, CNode{UserInt{10000}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toLong>\": \"<UserInt 10000>\"}"),
                          cst.toBson());
    }
}

TEST(CstGrammarTest, BuildsAndPrintsToObjectId) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::toObjectId, CNode{UserString{"5ab9cbfa31c2ab715d42129e"}}}}};
    ASSERT_BSONOBJ_EQ(
        fromjson("{\"<KeyFieldname toObjectId>\": \"<UserString 5ab9cbfa31c2ab715d42129e>\"}"),
        cst.toBson());
}

TEST(CstGrammarTest, BuildsAndPrintsToString) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toString, CNode{UserDouble{2.5}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname toString>\": \"<UserDouble 2.500000>\"}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::toString, CNode{UserObjectId{"5ab9cbfa31c2ab715d42129e"}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{\"<KeyFieldname toString>\": \"<UserObjectId 5ab9cbfa31c2ab715d42129e>\"}"),
            cst.toBson());
    }
}

TEST(CstGrammarTest, BuildsAndPrintsType) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::type, CNode{UserString{"$a"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname type>\": \"<UserString $a>\"}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::type,
             CNode{CNode::ArrayChildren{CNode{CNode::ArrayChildren{CNode{UserInt{1}}}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{\"<KeyFieldname type>\": [[\"<UserInt 1>\"]]}"), cst.toBson());
    }
}

TEST(CstGrammarTest, ParsesValidNumberAbs) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$abs: 1}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname abs>: "
              "\"<UserInt 1>\" } } }");
}

TEST(CstGrammarTest, ParsesValidCeil) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$ceil: 1.5}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname ceil>: "
              "\"<UserDouble 1.500000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidDivide) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$divide: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname divide>: "
              "[ \"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidExp) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$exp: 1.5}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname "
              "exponent>: \"<UserDouble 1.500000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidFloor) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$floor: 1.5}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname floor>: "
              "\"<UserDouble 1.500000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidLn) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$ln: [37, 10]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname ln>: [ "
              "\"<UserInt 10>\", \"<UserInt 37>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidLog) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$log: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname log>: [ "
              "\"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidLog10) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$log10: 1.5}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname logten>: "
              "\"<UserDouble 1.500000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidMod) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$mod: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname mod>: [ "
              "\"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidMultiply) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$multiply: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname "
              "multiply>: [ \"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidPow) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$pow: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname pow>: [ "
              "\"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidRound) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$round: [1.234, 2]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname round>: [ "
        "\"<UserDouble 1.234000>\", \"<UserInt 2>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidSqrt) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$sqrt: 25}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname sqrt>: "
              "\"<UserInt 25>\" } } }");
}

TEST(CstGrammarTest, ParsesValidSubtract) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$subtract: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname "
              "subtract>: [ \"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidTrunc) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$trunc: [1.234, 2]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname trunc>: [ "
        "\"<UserDouble 1.234000>\", \"<UserInt 2>\" ] } } }");
}

TEST(CstGrammarTest, ParsesBasicSort) {
    CNode output;
    auto input = fromjson("{sort: {val: 1, test: -1}}");
    BSONLexer lexer(input["sort"].embeddedObject(), ParserGen::token::START_SORT);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname val>: \"<KeyValue intOneKey>\", <UserFieldname test>: \"<KeyValue "
              "intNegOneKey>\" }");
}

TEST(CstGrammarTest, ParsesMetaSort) {
    CNode output;
    auto input = fromjson("{sort: {val: {$meta: \"textScore\"}}}");
    BSONLexer lexer(input["sort"].embeddedObject(), ParserGen::token::START_SORT);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ <UserFieldname val>: { <KeyFieldname meta>: \"<KeyValue textScore>\" } }");
}

TEST(CstGrammarTest, ParsesValidAllElementsTrue) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$allElementsTrue: [[true, 1]]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname "
              "allElementsTrue>: [ \"<UserInt 1>\", \"<UserBoolean 1>\" "
              "] } } }");
}

TEST(CstGrammarTest, ParsesValidSlice) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$slice: [[1, 2, 3], -15, 2]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname slice>: "
              "[ [ \"<UserInt 3>\", \"<UserInt 2>\", \"<UserInt 1>\" ], \"<UserInt -15>\", "
              "\"<UserInt 2>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidMeta) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$meta: \"indexKey\" }}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname meta>: "
              "\"<KeyValue indexKey>\" } } }");
}

TEST(CstGrammarTest, ParsesValidAnyElementTrue) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$anyElementTrue: [[false, 0]]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname "
              "anyElementTrue>: [ \"<UserInt 0>\", \"<UserBoolean 0>\" "
              "] } } }");
}

TEST(CstGrammarTest, ParsesValidSetDifference) {
    CNode output;
    auto input =
        fromjson("{pipeline: [{$project: {val: {$setDifference: [['a', 'c'], ['b', 'a']]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname "
              "setDifference>: [ [ \"<UserString c>\", \"<UserString a>\" ], "
              "[ \"<UserString a>\", \"<UserString b>\" ] ] } } }");
}

TEST(CstGrammarTest, ParsesValidSetEquals) {
    CNode output;
    auto input =
        fromjson("{pipeline: [{$project: {val: {$setEquals: [['a', 'b', 'a'], ['b', 'a']]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname "
              "setEquals>: [ [ \"<UserString a>\", \"<UserString b>\", "
              "\"<UserString a>\" ], [ \"<UserString a>\", \"<UserString b>\" ] ] } } }");
}

TEST(CstGrammarTest, ParsesValidSetIntersection) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {val: {$setIntersection: [['a', 'b'], "
        "[['a', 'b']]]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname "
              "setIntersection>: [ [ \"<UserString b>\", \"<UserString "
              "a>\" ], "
              "[ [ \"<UserString b>\", \"<UserString a>\" ] ] ] } } }");
}

TEST(CstGrammarTest, ParsesValidSetIsSubset) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$setIsSubset: ['$A', '$B']}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname "
              "setIsSubset>: [ \"<AggregationPath A>\", "
              "\"<AggregationPath B>\" "
              "] } } }");
}

TEST(CstGrammarTest, ParsesValidSetUnion) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$setUnion: [[1, 2], [3]]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath val>: { <KeyFieldname "
              "setUnion>: [ [ \"<UserInt 2>\", \"<UserInt 1>\" ], [ "
              "\"<UserInt "
              "3>\" ] ] } } }");
}

TEST(CstGrammarTest, FailsToParseTooManyParametersSetExpression) {
    CNode output;
    auto input =
        fromjson("{pipeline: [{$project: {val: {$allElementsTrue: [[true, 1], [true, 3]]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    ASSERT_THROWS_CODE_AND_WHAT(ParserGen(lexer, &output).parse(),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "syntax error, unexpected array, expecting end of array at element "
                                "'start array' within array at index 1 within '$allElementsTrue' "
                                "within '$project' within array at index 0 of input pipeline");
}

TEST(CstGrammarTest, FailsToParseTooFewParametersSetExpression) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$setUnion: [[true, 1]]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    ASSERT_THROWS_CODE_AND_WHAT(ParserGen(lexer, &output).parse(),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "syntax error, unexpected end of array at element 'end array' "
                                "within '$setUnion' within "
                                "'$project' within array at index 0 of input pipeline");
}

TEST(CstGrammarTest, ParsesValidSin) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$sin: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname sin>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidCos) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$cos: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname cos>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidTan) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$tan: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname tan>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidSinh) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$sinh: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname sinh>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidCosh) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$cosh: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname cosh>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidTanh) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$tanh: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname tanh>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidAsin) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$asin: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname asin>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidAcos) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$acos: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname acos>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidAtan) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$atan: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname atan>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidAsinh) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$asinh: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname asinh>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidAcosh) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$acosh: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname acosh>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidAtanh) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$atanh: 0.927}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname atanh>: "
              "\"<UserDouble 0.927000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidDegreesToRadians) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$degreesToRadians: 30}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname "
              "degreesToRadians>: "
              "\"<UserInt 30>\" } } }");
}

TEST(CstGrammarTest, ParsesValidRadiansToDegrees) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {trig: {$radiansToDegrees: "
        "NumberDecimal(\"0.9272952180016122324285124629224290\")}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname "
              "radiansToDegrees>: "
              "\"<UserDecimal 0.9272952180016122324285124629224290>\" } } }");
}

TEST(CstGrammarTest, ParsesValidSinAsArray) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$sin: [0.927]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath trig>: { <KeyFieldname sin>: [ "
              "\"<UserDouble 0.927000>\" ] } } }");
}

TEST(CstGrammarTest, FailsToParseInvalidSinAsArray) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {trig: {$sin: [0.927, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].embeddedObject(), ParserGen::token::START_PIPELINE);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}


}  // namespace
}  // namespace mongo
