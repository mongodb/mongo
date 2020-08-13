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
#include "mongo/db/cst/pipeline_parser_gen.hpp"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CstTest, BuildsAndPrints) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::atan2,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserDouble{2.0}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{atan2: [\"<UserDouble 3.000000>\", \"<UserDouble 2.000000>\"]}"),
            cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::projectInclusion,
             CNode{CNode::ObjectChildren{{UserFieldname{"a"}, CNode{KeyValue::trueKey}},
                                         {KeyFieldname::id, CNode{KeyValue::falseKey}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson(
                "{projectInclusion : {a: \"<KeyValue trueKey>\", id: \"<KeyValue falseKey>\"}}"),
            cst.toBson());
    }
}

TEST(CstGrammarTest, EmptyPipeline) {
    CNode output;
    auto input = fromjson("{pipeline: []}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_TRUE(stdx::get_if<CNode::ArrayChildren>(&output.payload));
    ASSERT_EQ(0, stdx::get_if<CNode::ArrayChildren>(&output.payload)->size());
}

TEST(CstGrammarTest, ParsesInternalInhibitOptimization) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$_internalInhibitOptimization: {}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::inhibitOptimization == stages[0].firstKeyFieldname());
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$_internalInhibitOptimization: 'invalid'}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, ParsesUnionWith) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$unionWith: {coll: 'hey', pipeline: 1.0}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::unionWith == stages[0].firstKeyFieldname());
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$unionWith: {pipeline: 1.0, coll: 'hey'}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::unionWith == stages[0].firstKeyFieldname());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ unionWith: { collArg: \"<UserString hey>\", pipelineArg: \"<UserDouble "
                  "1.000000>\" } }");
    }
}

TEST(CstGrammarTest, ParseSkipInt) {
    CNode output;
    auto input = fromjson("{pipeline: [{$skip: 5}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::skip == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{skip : \"<UserInt 5>\" }"), stages[0].toBson());
}

TEST(CstGrammarTest, ParseSkipDouble) {
    CNode output;
    auto input = fromjson("{pipeline: [{$skip: 1.5}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::skip == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{skip : \"<UserDouble 1.500000>\" }"), stages[0].toBson());
}

TEST(CstGrammarTest, ParseSkipLong) {
    CNode output;
    auto input = fromjson("{pipeline: [{$skip: 8223372036854775807}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::skip == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{skip : \"<UserLong 8223372036854775807>\" }"), stages[0].toBson());
}

TEST(CstGrammarTest, InvalidParseSkipObject) {
    CNode output;
    auto input = fromjson("{pipeline: [{$skip: {}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, InvalidParseSkipString) {
    CNode output;
    auto input = fromjson("{pipeline: [{$skip: '5'}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, ParsesLimitInt) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: 5}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::limit == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{limit : \"<UserInt 5>\"}"), stages[0].toBson());
}

TEST(CstGrammarTest, ParsesLimitDouble) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: 5.0}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::limit == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{limit : \"<UserDouble 5.000000>\"}"), stages[0].toBson());
}

TEST(CstGrammarTest, ParsesLimitLong) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: 123123123123}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::limit == stages[0].firstKeyFieldname());
    ASSERT_BSONOBJ_EQ(fromjson("{limit : \"<UserLong 123123123123>\"}"), stages[0].toBson());
}

TEST(CstGrammarTest, InvalidParseLimitString) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: \"5\"}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, InvalidParseLimitObject) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: {}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, InvalidParseLimitArray) {
    CNode output;
    auto input = fromjson("{pipeline: [{$limit: [2]}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstGrammarTest, ParsesProject) {
    {
        CNode output;
        auto input = fromjson(
            "{pipeline: [{$project: {a: 1.0, b: {c: NumberInt(1), d: NumberDecimal('1.0') }, _id: "
            "NumberLong(1)}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ projectInclusion: { a: \"<NonZeroKey of type double 1.000000>\", b: { "
                  "<CompoundInclusionKey>: { c: \"<NonZeroKey of type int 1>\", d: \"<NonZeroKey "
                  "of type decimal 1.0>\" } }, id: \"<NonZeroKey of type long 1>\" } }");
    }
    {
        CNode output;
        auto input = fromjson(
            "{pipeline: [{$project: {a: 0.0, b: NumberInt(0), c: { d: { e: NumberLong(0)}}}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectExclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(
            stages[0].toBson().toString(),
            "{ projectExclusion: { a: \"<KeyValue doubleZeroKey>\", b: \"<KeyValue intZeroKey>\", "
            "c: { <CompoundExclusionKey>: { d: { e: \"<KeyValue longZeroKey>\" } } } } }");
    }
    {
        CNode output;
        auto input = fromjson(
            "{pipeline: [{$project: {_id: 9.10, a: {$add: [4, 5, {$add: [6, 7, 8]}]}, b: "
            "{$atan2: "
            "[1.0, {$add: [2, -3]}]}}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ projectInclusion: { id: \"<NonZeroKey of type double 9.100000>\", a: { add: [ "
                  "{ add: [ "
                  "\"<UserInt 8>\", \"<UserInt 7>\", \"<UserInt 6>\" ] }, \"<UserInt 5>\", "
                  "\"<UserInt 4>\" ] }, b: { atan2: [ \"<UserDouble 1.000000>\", { add: [ "
                  "\"<UserInt -3>\", \"<UserInt 2>\" ] } ] } } }");
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: {$add: [6]}}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ projectInclusion: { a: { add: [ \"<UserInt 6>\" ] } } }");
    }
}

TEST(CstGrammarTest, FailsToParseMixedProject) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: 1, b: 0.0}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: 0, b: {$add: [5, 67]}}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, FailsToParseCompoundMixedProject) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: {b: 1, c: 0.0}}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: {b: {c: {d: NumberLong(0)}, e: 45}}}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstGrammarTest, FailsToParseProjectWithDollarFieldNames) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {$a: 1}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {b: 1, $a: 1}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstTest, BuildsAndPrintsAnd) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::andExpr,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserString{"green"}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{andExpr: [\"<UserDouble 3.000000>\", \"<UserString green>\"]}"),
            cst.toBson());
    }
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::andExpr, CNode{CNode::ArrayChildren{}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{andExpr: []}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::andExpr,
             CNode{CNode::ArrayChildren{
                 CNode{UserDouble{3.0}}, CNode{UserInt{2}}, CNode{UserDouble{5.0}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{andExpr: [\"<UserDouble 3.000000>\", \"<UserInt 2>\", "
                                   "\"<UserDouble 5.000000>\"]}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::andExpr,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserInt{2}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{andExpr: [\"<UserDouble 3.000000>\", \"<UserInt 2>\"]}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::andExpr,
             CNode{CNode::ArrayChildren{CNode{UserInt{0}}, CNode{UserBoolean{true}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{andExpr: [\"<UserInt 0>\", \"<UserBoolean 1>\"]}"),
                          cst.toBson());
    }
}

TEST(CstTest, BuildsAndPrintsOr) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::orExpr,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserString{"green"}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{orExpr: [\"<UserDouble 3.000000>\", \"<UserString green>\"]}"),
                          cst.toBson());
    }
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::orExpr, CNode{CNode::ArrayChildren{}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{orExpr: []}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::orExpr,
             CNode{CNode::ArrayChildren{
                 CNode{UserDouble{3.0}}, CNode{UserInt{2}}, CNode{UserDouble{5.0}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{orExpr: [\"<UserDouble 3.000000>\", \"<UserInt 2>\", "
                                   "\"<UserDouble 5.000000>\"]}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::orExpr,
             CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}, CNode{UserInt{2}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{orExpr: [\"<UserDouble 3.000000>\", \"<UserInt 2>\"]}"),
                          cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::orExpr,
             CNode{CNode::ArrayChildren{CNode{UserInt{0}}, CNode{UserBoolean{true}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{orExpr: [\"<UserInt 0>\", \"<UserBoolean 1>\"]}"),
                          cst.toBson());
    }
}

TEST(CstTest, BuildsAndPrintsNot) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::notExpr, CNode{CNode::ArrayChildren{CNode{UserDouble{3.0}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{notExpr: [\"<UserDouble 3.000000>\"]}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::notExpr, CNode{CNode::ArrayChildren{CNode{UserBoolean{true}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{notExpr: [\"<UserBoolean 1>\"]}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::notExpr, CNode{CNode::ArrayChildren{CNode{UserBoolean{false}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{notExpr: [\"<UserBoolean 0>\"]}"), cst.toBson());
    }
}

TEST(CstGrammarTest, ParsesSampleWithNumericSizeArgument) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: NumberInt(1)}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT_EQ(stages[0].toBson().toString(), "{ sample: { sizeArg: \"<UserInt 1>\" } }");
    }
    {
        // Although negative numbers are not valid, this is enforced at translation time.
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: NumberInt(-1)}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT_EQ(stages[0].toBson().toString(), "{ sample: { sizeArg: \"<UserInt -1>\" } }");
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: NumberLong(5)}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT_EQ(stages[0].toBson().toString(), "{ sample: { sizeArg: \"<UserLong 5>\" } }");
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: 10.0}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ sample: { sizeArg: \"<UserDouble 10.000000>\" } }");
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: 0}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT_EQ(stages[0].toBson().toString(), "{ sample: { sizeArg: \"<UserInt 0>\" } }");
    }
}

TEST(CstGrammarTest, InvalidParseSample) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: 2}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {notSize: 2}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$sample: {size: 'gots ta be a number'}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstTest, BuildsAndPrintsConvert) {
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::convert,
             CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{UserInt{3}}},
                                         {KeyFieldname::toArg, CNode{UserString{"string"}}}}}}}};
        ASSERT_BSONOBJ_EQ(
            fromjson("{convert: {inputArg: \"<UserInt 3>\", toArg: \"<UserString string>\"}}"),
            cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::convert,
             CNode{CNode::ObjectChildren{{KeyFieldname::inputArg, CNode{CNode::ArrayChildren{}}},
                                         {KeyFieldname::toArg, CNode{UserInt{8}}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{convert: {inputArg: [], toArg: \"<UserInt 8>\"}}"),
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
        ASSERT_BSONOBJ_EQ(
            fromjson(
                "{convert: {inputArg: {add: [\"<UserInt 4>\", \"<UserInt 5>\"]}, toArg: \"<UserInt "
                "1>\"}}"),
            cst.toBson());
    }
}

TEST(CstTest, BuildsAndPrintsToBool) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toBool, CNode{UserString{"a"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toBool: \"<UserString a>\"}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{{KeyFieldname::toBool, CNode{UserNull{}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toBool: \"<UserNull>\"}"), cst.toBson());
    }
}

TEST(CstTest, BuildsAndPrintsToDate) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toDate, CNode{UserString{"2018-03-03"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toDate: \"<UserString 2018-03-03>\"}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::toDate, CNode{UserObjectId{"5ab9c3da31c2ab715d421285"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toDate: \"<UserObjectId 5ab9c3da31c2ab715d421285>\"}"),
                          cst.toBson());
    }
}

TEST(CstTest, BuildsAndPrintsToDecimal) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toDecimal, CNode{UserBoolean{false}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toDecimal: \"<UserBoolean 0>\"}"), cst.toBson());
    }
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toDecimal, CNode{UserString{"-5.5"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toDecimal: \"<UserString -5.5>\"}"), cst.toBson());
    }
}

TEST(CstTest, BuildsAndPrintsToDouble) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toDouble, CNode{UserBoolean{true}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toDouble: \"<UserBoolean 1>\"}"), cst.toBson());
    }
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toDouble, CNode{UserLong{10000}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toDouble: \"<UserLong 10000>\"}"), cst.toBson());
    }
}

TEST(CstTest, BuildsAndPrintsToInt) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toInt, CNode{UserString{"-2"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toInt: \"<UserString -2>\"}"), cst.toBson());
    }
    {
        const auto cst = CNode{
            CNode::ObjectChildren{{KeyFieldname::toInt, CNode{UserDecimal{5.50000000000000}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toInt: \"<UserDecimal 5.50000000000000>\"}"), cst.toBson());
    }
}

TEST(CstTest, BuildsAndPrintsToLong) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toLong, CNode{UserString{"-2"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toLong: \"<UserString -2>\"}"), cst.toBson());
    }
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toLong, CNode{UserInt{10000}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toLong: \"<UserInt 10000>\"}"), cst.toBson());
    }
}

TEST(CstTest, BuildsAndPrintsToObjectId) {
    const auto cst = CNode{CNode::ObjectChildren{
        {KeyFieldname::toObjectId, CNode{UserString{"5ab9cbfa31c2ab715d42129e"}}}}};
    ASSERT_BSONOBJ_EQ(fromjson("{toObjectId: \"<UserString 5ab9cbfa31c2ab715d42129e>\"}"),
                      cst.toBson());
}

TEST(CstTest, BuildsAndPrintsToString) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::toString, CNode{UserDouble{2.5}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toString: \"<UserDouble 2.500000>\"}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::toString, CNode{UserObjectId{"5ab9cbfa31c2ab715d42129e"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{toString: \"<UserObjectId 5ab9cbfa31c2ab715d42129e>\"}"),
                          cst.toBson());
    }
}

TEST(CstTest, BuildsAndPrintsType) {
    {
        const auto cst =
            CNode{CNode::ObjectChildren{{KeyFieldname::type, CNode{UserString{"$a"}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{type: \"<UserString $a>\"}"), cst.toBson());
    }
    {
        const auto cst = CNode{CNode::ObjectChildren{
            {KeyFieldname::type,
             CNode{CNode::ArrayChildren{CNode{CNode::ArrayChildren{CNode{UserInt{1}}}}}}}}};
        ASSERT_BSONOBJ_EQ(fromjson("{type: [[\"<UserInt 1>\"]]}"), cst.toBson());
    }
}

TEST(CstGrammarTest, ParsesValidNumberAbs) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$abs: 1}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { abs: \"<UserInt 1>\" } } }");
}

TEST(CstGrammarTest, ParsesValidCeil) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$ceil: 1.5}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { ceil: \"<UserDouble 1.500000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidDivide) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$divide: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { divide: [ \"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidExp) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$exp: 1.5}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { exponent: \"<UserDouble 1.500000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidFloor) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$floor: 1.5}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { floor: \"<UserDouble 1.500000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidLn) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$ln: [37, 10]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { ln: [ \"<UserInt 10>\", \"<UserInt 37>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidLog) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$log: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { log: [ \"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidLog10) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$log10: 1.5}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { logten: \"<UserDouble 1.500000>\" } } }");
}

TEST(CstGrammarTest, ParsesValidMod) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$mod: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { mod: [ \"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidMultiply) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$multiply: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { multiply: [ \"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidPow) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$pow: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { pow: [ \"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidRound) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$round: [1.234, 2]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ projectInclusion: { val: { round: [ \"<UserDouble 1.234000>\", \"<UserInt 2>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidSqrt) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$sqrt: 25}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { sqrt: \"<UserInt 25>\" } } }");
}

TEST(CstGrammarTest, ParsesValidSubtract) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$subtract: [10, 5]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { val: { subtract: [ \"<UserInt 10>\", \"<UserInt 5>\" ] } } }");
}

TEST(CstGrammarTest, ParsesValidTrunc) {
    CNode output;
    auto input = fromjson("{pipeline: [{$project: {val: {$trunc: [1.234, 2]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ projectInclusion: { val: { trunc: [ \"<UserDouble 1.234000>\", \"<UserInt 2>\" ] } } }");
}

TEST(CstGrammarTest, ParsesEmptyMatchInFind) {
    CNode output;
    auto input = fromjson("{}");
    BSONLexer lexer(input, PipelineParserGen::token::START_MATCH);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(), "{}");
}

TEST(CstGrammarTest, ParsesMatchWithEqualityPredicates) {
    CNode output;
    auto input = fromjson("{a: 5.0, b: NumberInt(10), _id: NumberLong(15)}");
    BSONLexer lexer(input, PipelineParserGen::token::START_MATCH);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    ASSERT_EQ(output.toBson().toString(),
              "{ a: \"<UserDouble 5.000000>\", b: \"<UserInt 10>\", _id: \"<UserLong 15>\" }");
}

TEST(CstGrammarTest, FailsToParseDollarPrefixedPredicates) {
    {
        auto input = fromjson("{$atan2: [3, 5]}");
        BSONLexer lexer(input, PipelineParserGen::token::START_MATCH);
        ASSERT_THROWS_CODE_AND_WHAT(
            PipelineParserGen(lexer, nullptr).parse(),
            AssertionException,
            ErrorCodes::FailedToParse,
            "syntax error, unexpected ATAN2 at element '$atan2' of input filter");
    }
    {
        auto input = fromjson("{$prefixed: 5}");
        BSONLexer lexer(input, PipelineParserGen::token::START_MATCH);
        ASSERT_THROWS_CODE_AND_WHAT(
            PipelineParserGen(lexer, nullptr).parse(),
            AssertionException,
            ErrorCodes::FailedToParse,
            "syntax error, unexpected $-prefixed fieldname at element '$prefixed' of input filter");
    }
}

}  // namespace
}  // namespace mongo
