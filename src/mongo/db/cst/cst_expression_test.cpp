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
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CstExpressionTest, ParsesProjectWithAnd) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {_id: 9.10, a: {$and: [4, {$and: [7, 8]}]}, b: {$and: [2, "
        "-3]}}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <KeyFieldname id>: \"<NonZeroKey of type "
              "double 9.100000>\", <ProjectionPath a>: { <KeyFieldname andExpr>: [ { "
              "<KeyFieldname andExpr>: [ \"<UserInt 8>\", \"<UserInt 7>\" ] }, \"<UserInt 4>\" ] "
              "}, <ProjectionPath b>: { <KeyFieldname andExpr>: "
              "[ \"<UserInt -3>\", \"<UserInt 2>\" ] } } }");
}

TEST(CstExpressionTest, ParsesProjectWithOr) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {_id: 9.10, a: {$or: [4, {$or: [7, 8]}]}, b: {$or: [2, -3]}}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <KeyFieldname id>: \"<NonZeroKey of type "
              "double 9.100000>\", <ProjectionPath a>: { <KeyFieldname orExpr>: [ "
              "{ <KeyFieldname orExpr>: [ \"<UserInt 8>\", \"<UserInt 7>\" ] }, \"<UserInt 4>\" ] "
              "}, <ProjectionPath b>: { "
              "<KeyFieldname orExpr>: [ \"<UserInt -3>\", \"<UserInt 2>\" ] } } }");
}

TEST(CstExpressionTest, ParsesProjectWithNot) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {_id: 9.10, a: {$not: [4]}, b: {$and: [1.0, {$not: "
        "[true]}]}}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ <KeyFieldname projectInclusion>: { <KeyFieldname id>: \"<NonZeroKey of type "
        "double 9.100000>\", <ProjectionPath a>: { <KeyFieldname notExpr>: [ "
        "\"<UserInt 4>\" ] }, <ProjectionPath b>: { <KeyFieldname andExpr>: [ { <KeyFieldname "
        "notExpr>: [ \"<UserBoolean 1>\" ] }, "
        "\"<UserDouble 1.000000>\" ] } } }");
}

TEST(CstExpressionTest, ParsesComparisonExpressions) {
    auto parseAndTest = [](StringData expr) {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {_id: {$" + expr + ": [1, 2.5]}}}]}");
        BSONLexer lexer(input["pipeline"]);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_EQ(0, parseTree.parse());
        auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
        ASSERT_EQ(1, stages.size());
        ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
        ASSERT_EQ(stages[0].toBson().toString(),
                  "{ <KeyFieldname projectInclusion>: { <KeyFieldname id>: { <KeyFieldname " +
                      expr + ">: [ \"<UserInt 1>\", \"<UserDouble 2.500000>\" ] } } }");
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
            BSONLexer lexer(input["pipeline"]);
            auto parseTree = ParserGen(lexer, &output);
            ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
        }
        {
            CNode output;
            auto input = fromjson("{pipeline: [{$project: {_id: {$" + expr + ": [1, 2, 3]}}}]}");
            BSONLexer lexer(input["pipeline"]);
            auto parseTree = ParserGen(lexer, &output);
            ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
        }
        {
            CNode output;
            auto input = fromjson("{pipeline: [{$project: {_id: {$" + expr + ": 1}}}]}");
            BSONLexer lexer(input["pipeline"]);
            auto parseTree = ParserGen(lexer, &output);
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
        auto input = fromjson("{pipeline: [{$project: {a: {$convert: 'x'}}}]}");
        BSONLexer lexer(input["pipeline"]);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
    {
        CNode output;
        auto input = fromjson("{pipeline: [{$project: {a: {$convert: {input: 'x'}}}}]}");
        BSONLexer lexer(input["pipeline"]);
        auto parseTree = ParserGen(lexer, &output);
        ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
    }
}

TEST(CstExpressionTest, ParsesConvertExpressions) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {a: {$toBool: 1}, b: {$toDate: 1100000000000}, "
        "c: {$toDecimal: 5}, d: {$toDouble: -2}, e: {$toInt: 1.999999}, "
        "f: {$toLong: 1.999999}, g: {$toObjectId: '$_id'}, h: {$toString: false}}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath a>: { <KeyFieldname toBool>: "
              "\"<UserInt 1>\" }, <ProjectionPath b>: { <KeyFieldname toDate>: \"<UserLong "
              "1100000000000>\" }, <ProjectionPath c>: { <KeyFieldname toDecimal>: \"<UserInt 5>\" "
              "}, <ProjectionPath d>: { <KeyFieldname toDouble>: \"<UserInt "
              "-2>\" }, <ProjectionPath e>: { <KeyFieldname toInt>: \"<UserDouble 1.999999>\" }, "
              "<ProjectionPath f>: { <KeyFieldname toLong>: \"<UserDouble "
              "1.999999>\" }, <ProjectionPath g>: { <KeyFieldname toObjectId>: \"<AggregationPath "
              "_id>\" }, <ProjectionPath h>: { <KeyFieldname toString>: "
              "\"<UserBoolean 0>\" } } }");
}

TEST(CstExpressionTest, ParsesConvertExpressionsNoOptArgs) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {a: {$convert: {input: 1, to: 'string'}}, "
        "b: {$convert : {input: 'true', to: 'bool'}}}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ <KeyFieldname projectInclusion>: { <ProjectionPath a>: { <KeyFieldname convert>: { "
        "<KeyFieldname inputArg>: \"<UserInt 1>\", <KeyFieldname toArg>: \"<UserString "
        "string>\", <KeyFieldname onErrorArg>: \"<KeyValue absentKey>\", <KeyFieldname "
        "onNullArg>: \"<KeyValue "
        "absentKey>\" } }, <ProjectionPath b>: { <KeyFieldname convert>: { <KeyFieldname "
        "inputArg>: \"<UserString true>\", <KeyFieldname toArg>: "
        "\"<UserString bool>\", <KeyFieldname onErrorArg>: \"<KeyValue absentKey>\", "
        "<KeyFieldname onNullArg>: "
        "\"<KeyValue absentKey>\" } } } }");
}

TEST(CstExpressionTest, ParsesConvertExpressionsWithOptArgs) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: {a: {$convert: {input: 1, to: 'string', "
        "onError: 'Could not convert'}}, b : {$convert : {input: "
        "true, to : 'double', onNull : 0}}}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ <KeyFieldname projectInclusion>: { <ProjectionPath a>: { <KeyFieldname convert>: { "
        "<KeyFieldname inputArg>: \"<UserInt 1>\", <KeyFieldname toArg>: \"<UserString "
        "string>\", <KeyFieldname onErrorArg>: \"<UserString Could not convert>\", "
        "<KeyFieldname onNullArg>: \"<KeyValue "
        "absentKey>\" } }, <ProjectionPath b>: { <KeyFieldname convert>: { <KeyFieldname "
        "inputArg>: \"<UserBoolean 1>\", <KeyFieldname toArg>: "
        "\"<UserString double>\", <KeyFieldname onErrorArg>: \"<KeyValue absentKey>\", "
        "<KeyFieldname onNullArg>: "
        "\"<UserInt 0>\" } } } }");
}

TEST(CstExpressionTest, ParsesIndexOf) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { "
        "b: { $indexOfBytes: ['ABC', 'B']}, "
        "c: { $indexOfCP: [ 'cafeteria', 'e' ] }, "
        "d: { $indexOfBytes: [ 'foo.bar.fi', '.', 5, 7 ] }}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath b>: { <KeyFieldname "
              "indexOfBytes>: [ \"<UserString ABC>\", \"<UserString B>\" "
              "] }, <ProjectionPath c>: "
              "{ <KeyFieldname indexOfCP>: [ \"<UserString cafeteria>\", \"<UserString e>\" ] }, "
              "<ProjectionPath d>: { "
              "<KeyFieldname indexOfBytes>: [ \"<UserString foo.bar.fi>\", \"<UserString .>\", "
              "\"<UserInt 5>\", "
              "\"<UserInt 7>\" ] } } }");
}

TEST(CstExpressionTest, ParsesDateFromString) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { m: { $dateFromString: { dateString: '2017-02-08T12:10:40.787', "
        "timezone: 'America/New_York' } } }}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath m>: { <KeyFieldname "
              "dateFromString>: { <KeyFieldname dateStringArg>: \"<UserString "
              "2017-02-08T12:10:40.787>\", <KeyFieldname formatArg>: \"<KeyValue absentKey>\", "
              "<KeyFieldname timezoneArg>: "
              "\"<UserString America/New_York>\", <KeyFieldname onErrorArg>: \"<KeyValue "
              "absentKey>\", <KeyFieldname onNullArg>: "
              "\"<KeyValue absentKey>\" } } } }");
}

TEST(CstExpressionTest, ParsesDateToString) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { m: { $dateToString: { date: '$date', "
        "format: '%Y-%m-%d' } } } } ] }");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ <KeyFieldname projectInclusion>: { <ProjectionPath m>: { <KeyFieldname dateToString>: { "
        "<KeyFieldname dateArg>: \"<AggregationPath date>\", <KeyFieldname formatArg>: "
        "\"<UserString %Y-%m-%d>\", <KeyFieldname timezoneArg>: \"<KeyValue absentKey>\", "
        "<KeyFieldname onNullArg>: "
        "\"<KeyValue absentKey>\" } } } }");
}

TEST(CstExpressionTest, ParsesReplaceStringExpressions) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { "
        "h: { $replaceOne: { input: '$name', find: 'Cafe', replacement: 'CAFE' } }, "
        "i: { $replaceAll: { input: 'cafeSeattle', find: 'cafe', replacement: 'CAFE' } } }}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ <KeyFieldname projectInclusion>: { <ProjectionPath h>: { <KeyFieldname replaceOne>: { "
        "<KeyFieldname inputArg>: \"<AggregationPath name>\", <KeyFieldname findArg>: "
        "\"<UserString Cafe>\", <KeyFieldname replacementArg>: \"<UserString CAFE>\" } }, "
        "<ProjectionPath i>: { <KeyFieldname replaceAll>: "
        "{ <KeyFieldname inputArg>: \"<UserString cafeSeattle>\", <KeyFieldname findArg>: "
        "\"<UserString cafe>\", "
        "<KeyFieldname replacementArg>: \"<UserString CAFE>\" } } } }");
}

TEST(CstExpressionTest, ParsesTrim) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { "
        "d: { $ltrim: { input: ' ggggoodbyeeeee' } }, "
        "e: { $rtrim: { input: 'ggggoodbyeeeee   '} }, "
        "f: { $trim: { input: '    ggggoodbyeeeee', chars: ' ge' } } }}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath d>: { <KeyFieldname ltrim>: { "
              "<KeyFieldname inputArg>: \"<UserString  ggggoodbyeeeee>\", <KeyFieldname charsArg>: "
              "\"<KeyValue absentKey>\" } }, <ProjectionPath e>: { <KeyFieldname rtrim>: { "
              "<KeyFieldname inputArg>: \"<UserString ggggoodbyeeeee  "
              " >\", <KeyFieldname charsArg>: \"<KeyValue absentKey>\" } }, <ProjectionPath f>: { "
              "<KeyFieldname trim>: { <KeyFieldname inputArg>: \"<UserString  "
              "   ggggoodbyeeeee>\", <KeyFieldname charsArg>: \"<UserString  ge>\" } } } }");
}

TEST(CstExpressionTest, ParsesToUpperAndLower) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { "
        "g: { $toUpper: 'abc' }, "
        "v: { $toLower: 'ABC' }}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath g>: { <KeyFieldname toUpper>: "
              "\"<UserString abc>\" }, <ProjectionPath v>: { <KeyFieldname toLower>: \"<UserString "
              "ABC>\" } } }");
}

TEST(CstExpressionTest, ParsesRegexExpressions) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { "
        "j: { $regexFind: { input: '$details', regex: /^[a-z0-9_.+-]/, options: 'i' } }, "
        "k: { $regexFindAll: { input: '$fname', regex: /(C(ar)*)ol/ } }, "
        "l: { $regexMatch: { input: '$description', regex: /lin(e|k)/ } } }}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ <KeyFieldname projectInclusion>: { <ProjectionPath j>: { <KeyFieldname regexFind>: "
        "{ <KeyFieldname inputArg>: \"<AggregationPath details>\", <KeyFieldname regexArg>: "
        "\"<UserRegex /^[a-z0-9_.+-]/>\", <KeyFieldname optionsArg>: \"<UserString i>\" } }, "
        "<ProjectionPath k>: { <KeyFieldname regexFindAll>: { "
        "<KeyFieldname inputArg>: \"<AggregationPath fname>\", <KeyFieldname regexArg>: "
        "\"<UserRegex /(C(ar)*)ol/>\", <KeyFieldname optionsArg>: "
        "\"<KeyValue absentKey>\" } }, <ProjectionPath l>: { <KeyFieldname regexMatch>: { "
        "<KeyFieldname inputArg>: \"<AggregationPath description>\", <KeyFieldname regexArg>: "
        "\"<UserRegex /lin(e|k)/>\", <KeyFieldname optionsArg>: \"<KeyValue absentKey>\" } } } }");
}

TEST(CstExpressionTest, ParsesSubstrExpressions) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { "
        "s: { $substr: [ '$quarter', 2, -1 ] }, "
        "t: { $substrBytes: [ '$name', 0, 3 ] }, "
        "u: { $substrCP: [ 'Hello World!', 6, 5 ] }}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath s>: { <KeyFieldname substr>: [ "
              "\"<AggregationPath quarter>\", \"<UserInt 2>\", "
              "\"<UserInt -1>\" ] }, <ProjectionPath t>: { <KeyFieldname substrBytes>: [ "
              "\"<AggregationPath name>\", \"<UserInt 0>\", \"<UserInt 3>\" ] }, <ProjectionPath "
              "u>: { <KeyFieldname substrCP>: [ \"<UserString Hello World!>\", \"<UserInt 6>\", "
              "\"<UserInt 5>\" ] } } }");
}

TEST(CstExpressionTest, ParsesStringLengthExpressions) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { "
        "p: { $strLenBytes: 'cafeteria' }, "
        "q: { $strLenCP: 'Hello World!' }}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ <KeyFieldname projectInclusion>: { <ProjectionPath p>: { <KeyFieldname strLenBytes>: "
        "\"<UserString cafeteria>\" }, <ProjectionPath q>: { <KeyFieldname strLenCP>: "
        "\"<UserString Hello World!>\" } } }");
}

TEST(CstExpressionTest, ParsesSplit) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { "
        "o: { $split: [ {$toUpper: 'abc'}, '-' ] }}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ <KeyFieldname projectInclusion>: { <ProjectionPath o>: { <KeyFieldname split>: [ { "
        "<KeyFieldname toUpper>: \"<UserString abc>\" }, "
        "\"<UserString ->\" ] } } }");
}

TEST(CstExpressionTest, ParsesStrCaseCmp) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { "
        "r: { $strcasecmp: [ '$quarter', '13q4' ] }}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath r>: { <KeyFieldname "
              "strcasecmp>: [ \"<AggregationPath quarter>\", \"<UserString "
              "13q4>\" ] } } }");
}

TEST(CstExpressionTest, ParsesConcat) {
    CNode output;
    auto input = fromjson(
        "{pipeline: [{$project: { "
        "a: { $concat: [ 'item', ' - ', '$description' ]}}}]}");
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ <KeyFieldname projectInclusion>: { <ProjectionPath a>: { <KeyFieldname concat>: [ "
              "\"<AggregationPath description>\", "
              "\"<UserString  - >\", "
              "\"<UserString item>\" ] } } }");
}

TEST(CstExpressionTest, FailsToParseTripleDollar) {
    CNode output;
    auto input = BSON("pipeline" << BSON_ARRAY(BSON("$project" << BSON("a"
                                                                       << "$$$triple"))));
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstExpressionTest, FailsToParseLoneDollar) {
    CNode output;
    auto input = BSON("pipeline" << BSON_ARRAY(BSON("$project" << BSON("a"
                                                                       << "$"))));
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}

TEST(CstExpressionTest, FailsToParseInvalidVarName) {
    CNode output;
    auto input = BSON("pipeline" << BSON_ARRAY(BSON("$project" << BSON("a"
                                                                       << "$$invalid"))));
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_THROWS_CODE(parseTree.parse(), AssertionException, ErrorCodes::FailedToParse);
}
}  // namespace
}  // namespace mongo
