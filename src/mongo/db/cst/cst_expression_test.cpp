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

TEST(CstExpressionTest, ParsesProjectWithAnd) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {_id: 9.10, a: {$and: [4, {$and: [7, 8]}]}, b: {$and: [2, "
        "-3]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::project == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ project: { id: \"<NonZeroKey of type double 9.100000>\", a: { andExpr: [ "
        "\"<UserInt 4>\", { andExpr: [ \"<UserInt 7>\", \"<UserInt 8>\" ] } ] }, b: { andExpr: [ "
        "\"<UserInt 2>\", \"<UserInt -3>\" ] } } }");
}

TEST(CstExpressionTest, ParsesProjectWithOr) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {_id: 9.10, a: {$or: [4, {$or: [7, 8]}]}, b: {$or: [2, -3]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::project == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ project: { id: \"<NonZeroKey of type double 9.100000>\", a: { orExpr: [ "
        "\"<UserInt 4>\", { orExpr: [ \"<UserInt 7>\", \"<UserInt 8>\" ] } ] }, b: { orExpr: [ "
        "\"<UserInt 2>\", \"<UserInt -3>\" ] } } }");
}

TEST(CstExpressionTest, ParsesProjectWithNot) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {_id: 9.10, a: {$not: [4]}, b: {$and: [1.0, {$not: "
        "[true]}]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::project == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ project: { id: \"<NonZeroKey of type double 9.100000>\", a: { notExpr: [ "
              "\"<UserInt 4>\" ] }, b: { andExpr: [ \"<UserDouble 1.000000>\", { notExpr: [ "
              "\"<UserBoolean 1>\" ] } ] } } }");
}

TEST(CstExpressionTest, ParsesComparisonExpressions) {
    auto parseAndTest = [](StringData expr) {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {_id: {$" + expr + ": [1, 2.5]}}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::project == stages[0].firstKeyFieldname());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ project: { id: { " + expr +
                      ": [ \"<UserInt 1>\", \"<UserDouble 2.500000>\" ] } } }");
    };

    for (auto&& expr : {"cmp"_sd, "eq"_sd, "gt"_sd, "gte"_sd, "lt"_sd, "lte"_sd, "ne"_sd}) {
        parseAndTest(expr);
    }
}

TEST(CstExpressionTest, FailsToParseInvalidComparisonExpressions) {
    auto assertFailsToParse = [](StringData expr) {
        {
            CNode output;
            auto input = fromjson("{pipeline: [{$project: {_id: {$" + expr + ": [1]}}}]}");
            BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
            auto parseTree = PipelineParserGen(lexer, &output);
            ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
        }
        {
            CNode output;
            auto input = fromjson("{pipeline: [{$project: {_id: {$" + expr + ": [1, 2, 3]}}}]}");
            BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
            auto parseTree = PipelineParserGen(lexer, &output);
            ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
        }
        {
            CNode output;
            auto input = fromjson("{pipeline: [{$project: {_id: {$" + expr + ": 1}}}]}");
            BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
            auto parseTree = PipelineParserGen(lexer, &output);
            ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
        }
    };

    for (auto&& expr : {"cmp"_sd, "eq"_sd, "gt"_sd, "gte"_sd, "lt"_sd, "lte"_sd, "ne"_sd}) {
        assertFailsToParse(expr);
    }
}

TEST(CstExpressionTest, FailsToParseInvalidConvertExpressions) {
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: {$convert: {input: 'x', to: true}}}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: {$convert: {input: 'x'}}}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        auto parseTree = PipelineParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstExpressionTest, ParsesConvertExpressions) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {a: {$toBool: 1}, b: {$toDate: 1100000000000}, "
        "c: {$toDecimal: 5}, d: {$toDouble: -2}, e: {$toInt: 1.999999}, "
        "f: {$toLong: 1.999999}, g: {$toObjectId: '$_id'}, h: {$toString: false}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::project == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ project: { a: { toBool: \"<UserInt 1>\" }, b: { toDate: \"<UserLong "
              "1100000000000>\" }, c: { toDecimal: \"<UserInt 5>\" }, d: { toDouble: \"<UserInt "
              "-2>\" }, e: { toInt: \"<UserDouble 1.999999>\" }, f: { toLong: \"<UserDouble "
              "1.999999>\" }, g: { toObjectId: \"<UserString $_id>\" }, h: { toString: "
              "\"<UserBoolean 0>\" } } }");
}

TEST(CstExpressionTest, ParsesConvertExpressionsNoOptArgs) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {a: {$convert: {input: 1, to: 'string'}}, "
        "b: {$convert : {input: 'true', to: 'bool'}}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::project == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ project: { a: { convert: { inputArg: \"<UserInt 1>\", toArg: \"<UserString "
              "string>\", onErrorArg: \"<KeyValue absentKey>\", onNullArg: \"<KeyValue "
              "absentKey>\" } }, b: { convert: { inputArg: \"<UserString true>\", toArg: "
              "\"<UserString bool>\", onErrorArg: \"<KeyValue absentKey>\", onNullArg: "
              "\"<KeyValue absentKey>\" } } } }");
}

TEST(CstExpressionTest, ParsesConvertExpressionsWithOptArgs) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {a: {$convert: {input: 1, to: 'string', "
        "onError: 'Could not convert'}}, b : {$convert : {input: "
        "true, to : 'double', onNull : 0}}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    auto parseTree = PipelineParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::project == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ project: { a: { convert: { inputArg: \"<UserInt 1>\", toArg: \"<UserString "
              "string>\", onErrorArg: \"<UserString Could not convert>\", onNullArg: \"<KeyValue "
              "absentKey>\" } }, b: { convert: { inputArg: \"<UserBoolean 1>\", toArg: "
              "\"<UserString double>\", onErrorArg: \"<KeyValue absentKey>\", onNullArg: "
              "\"<UserInt 0>\" } } } }");
}

}  // namespace
}  // namespace mongo
