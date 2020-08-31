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
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ projectInclusion: { id: \"<NonZeroKey of type double 9.100000>\", a: { andExpr: [ { "
        "andExpr: [ \"<UserInt 8>\", \"<UserInt 7>\" ] }, \"<UserInt 4>\" ] }, b: { andExpr: "
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
              "{ projectInclusion: { id: \"<NonZeroKey of type double 9.100000>\", a: { orExpr: [ "
              "{ orExpr: "
              "[ \"<UserInt 8>\", \"<UserInt 7>\" ] }, \"<UserInt 4>\" ] }, b: { orExpr: [ "
              "\"<UserInt -3>\", \"<UserInt 2>\" ] } } }");
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
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { id: \"<NonZeroKey of type double 9.100000>\", a: { notExpr: [ "
              "\"<UserInt 4>\" ] }, b: { andExpr: [ { notExpr: [ \"<UserBoolean 1>\" ] }, "
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
                  "{ projectInclusion: { id: { " + expr +
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
              "{ projectInclusion: { a: { toBool: \"<UserInt 1>\" }, b: { toDate: \"<UserLong "
              "1100000000000>\" }, c: { toDecimal: \"<UserInt 5>\" }, d: { toDouble: \"<UserInt "
              "-2>\" }, e: { toInt: \"<UserDouble 1.999999>\" }, f: { toLong: \"<UserDouble "
              "1.999999>\" }, g: { toObjectId: \"<UserFieldPath $_id>\" }, h: { toString: "
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
        "{ projectInclusion: { a: { convert: { inputArg: \"<UserInt 1>\", toArg: \"<UserString "
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
    BSONLexer lexer(input["pipeline"]);
    auto parseTree = ParserGen(lexer, &output);
    ASSERT_EQ(0, parseTree.parse());
    auto stages = stdx::get<CNode::ArrayChildren>(output.payload);
    ASSERT_EQ(1, stages.size());
    ASSERT(KeyFieldname::projectInclusion == stages[0].firstKeyFieldname());
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ projectInclusion: { a: { convert: { inputArg: \"<UserInt 1>\", toArg: \"<UserString "
        "string>\", onErrorArg: \"<UserString Could not convert>\", onNullArg: \"<KeyValue "
        "absentKey>\" } }, b: { convert: { inputArg: \"<UserBoolean 1>\", toArg: "
        "\"<UserString double>\", onErrorArg: \"<KeyValue absentKey>\", onNullArg: "
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
              "{ projectInclusion: { b: { indexOfBytes: [ \"<UserString ABC>\", \"<UserString B>\" "
              "] }, c: "
              "{ indexOfCP: [ \"<UserString cafeteria>\", \"<UserString e>\" ] }, d: { "
              "indexOfBytes: [ \"<UserString foo.bar.fi>\", \"<UserString .>\", \"<UserInt 5>\", "
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
              "{ projectInclusion: { m: { dateFromString: { dateStringArg: \"<UserString "
              "2017-02-08T12:10:40.787>\", formatArg: \"<KeyValue absentKey>\", timezoneArg: "
              "\"<UserString America/New_York>\", onErrorArg: \"<KeyValue absentKey>\", onNullArg: "
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
        "{ projectInclusion: { m: { dateToString: { dateArg: \"<UserFieldPath $date>\", formatArg: "
        "\"<UserString %Y-%m-%d>\", timezoneArg: \"<KeyValue absentKey>\", onNullArg: "
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
        "{ projectInclusion: { h: { replaceOne: { inputArg: \"<UserFieldPath $name>\", findArg: "
        "\"<UserString Cafe>\", replacementArg: \"<UserString CAFE>\" } }, i: { replaceAll: "
        "{ inputArg: \"<UserString cafeSeattle>\", findArg: \"<UserString cafe>\", "
        "replacementArg: \"<UserString CAFE>\" } } } }");
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
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ projectInclusion: { d: { ltrim: { inputArg: \"<UserString  ggggoodbyeeeee>\", charsArg: "
        "\"<KeyValue absentKey>\" } }, e: { rtrim: { inputArg: \"<UserString ggggoodbyeeeee  "
        " >\", charsArg: \"<KeyValue absentKey>\" } }, f: { trim: { inputArg: \"<UserString  "
        "   ggggoodbyeeeee>\", charsArg: \"<UserString  ge>\" } } } }");
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
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ projectInclusion: { g: { toUpper: \"<UserString abc>\" }, v: { toLower: \"<UserString "
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
        "{ projectInclusion: { j: { regexFind: { inputArg: \"<UserFieldPath $details>\", regexArg: "
        "\"<UserRegex /^[a-z0-9_.+-]/>\", optionsArg: \"<UserString i>\" } }, k: { regexFindAll: { "
        "inputArg: \"<UserFieldPath $fname>\", regexArg: \"<UserRegex /(C(ar)*)ol/>\", optionsArg: "
        "\"<KeyValue absentKey>\" } }, l: { regexMatch: { inputArg: \"<UserFieldPath "
        "$description>\", "
        "regexArg: \"<UserRegex /lin(e|k)/>\", optionsArg: \"<KeyValue absentKey>\" } } } }");
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
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ projectInclusion: { s: { substr: [ \"<UserFieldPath $quarter>\", \"<UserInt 2>\", "
        "\"<UserInt -1>\" "
        "] }, t: { substrBytes: [ \"<UserFieldPath $name>\", \"<UserInt 0>\", \"<UserInt 3>\" ] }, "
        "u: "
        "{ substrCP: [ \"<UserString Hello World!>\", \"<UserInt 6>\", \"<UserInt 5>\" ] } } }");
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
        "{ projectInclusion: { p: { strLenBytes: \"<UserString cafeteria>\" }, q: { strLenCP: "
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
    ASSERT_EQ(stages[0].toBson().toString(),
              "{ projectInclusion: { o: { split: [ { toUpper: \"<UserString abc>\" }, "
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
    ASSERT_EQ(
        stages[0].toBson().toString(),
        "{ projectInclusion: { r: { strcasecmp: [ \"<UserFieldPath $quarter>\", \"<UserString "
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
              "{ projectInclusion: { a: { concat: [ \"<UserFieldPath $description>\", "
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
