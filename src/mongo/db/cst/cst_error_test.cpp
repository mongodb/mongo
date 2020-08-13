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

TEST(CstErrorTest, EmptyStageSpec) {
    auto input = fromjson("{pipeline: [{}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    ASSERT_THROWS_CODE_AND_WHAT(PipelineParserGen(lexer, nullptr).parse(),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "syntax error, unexpected end of object at element 'end object' "
                                "within array at index 0 of input pipeline");
}

TEST(CstErrorTest, UnknownStageName) {
    // First stage.
    {
        auto input = fromjson("{pipeline: [{$unknownStage: {}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        ASSERT_THROWS_CODE_AND_WHAT(PipelineParserGen(lexer, nullptr).parse(),
                                    AssertionException,
                                    ErrorCodes::FailedToParse,
                                    "syntax error, unexpected $-prefixed fieldname at element "
                                    "'$unknownStage' within array at index 0 of input pipeline");
    }
    // Subsequent stage.
    {
        auto input = fromjson("{pipeline: [{$limit: 1}, {$unknownStage: {}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        ASSERT_THROWS_CODE_AND_WHAT(PipelineParserGen(lexer, nullptr).parse(),
                                    AssertionException,
                                    ErrorCodes::FailedToParse,
                                    "syntax error, unexpected $-prefixed fieldname at element "
                                    "'$unknownStage' within array at index 1 of input pipeline");
    }
}

TEST(CstErrorTest, InvalidStageArgument) {
    {
        auto input = fromjson("{pipeline: [{$sample: 1}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        ASSERT_THROWS_CODE_AND_WHAT(
            PipelineParserGen(lexer, nullptr).parse(),
            AssertionException,
            ErrorCodes::FailedToParse,
            "syntax error, unexpected non-zero integer, expecting object at element '1' within "
            "'$sample' within array at index 0 of input pipeline");
    }
    {
        auto input = fromjson("{pipeline: [{$project: {a: 1}}, {$limit: {}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        ASSERT_THROWS_CODE_AND_WHAT(PipelineParserGen(lexer, nullptr).parse(),
                                    AssertionException,
                                    ErrorCodes::FailedToParse,
                                    "syntax error, unexpected object at element 'start object' "
                                    "within '$limit' within array at index 1 of input pipeline");
    }
}

TEST(CstErrorTest, UnknownArgumentInStageSpec) {
    {
        auto input = fromjson("{pipeline: [{$sample: {huh: 1}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        ASSERT_THROWS_CODE_AND_WHAT(
            PipelineParserGen(lexer, nullptr).parse(),
            AssertionException,
            ErrorCodes::FailedToParse,
            "syntax error, unexpected fieldname, expecting size argument at element 'huh' within "
            "'$sample' within array at index 0 of input pipeline");
    }
    {
        auto input = fromjson("{pipeline: [{$project: {a: 1}}, {$limit: 1}, {$sample: {huh: 1}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        ASSERT_THROWS_CODE_AND_WHAT(
            PipelineParserGen(lexer, nullptr).parse(),
            AssertionException,
            ErrorCodes::FailedToParse,
            "syntax error, unexpected fieldname, expecting size argument at element 'huh' within "
            "'$sample' within array at index 2 of input pipeline");
    }
}

TEST(CstErrorTest, InvalidArgumentTypeWithinStageSpec) {
    {
        auto input = fromjson("{pipeline: [{$sample: {size: 'cmon'}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        ASSERT_THROWS_CODE_AND_WHAT(
            PipelineParserGen(lexer, nullptr).parse(),
            AssertionException,
            ErrorCodes::FailedToParse,
            "syntax error, unexpected string at element 'cmon' within 'size' within '$sample' "
            "within array at index 0 of input pipeline");
    }
    {
        auto input = fromjson("{pipeline: [{$project: {a: 1}}, {$sample: {size: true}}]}");
        BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
        ASSERT_THROWS_CODE_AND_WHAT(PipelineParserGen(lexer, nullptr).parse(),
                                    AssertionException,
                                    ErrorCodes::FailedToParse,
                                    "syntax error, unexpected true at element 'true' within 'size' "
                                    "within '$sample' within array at index 1 of input pipeline");
    }
}

TEST(CstErrorTest, MissingRequiredArgument) {
    auto input = fromjson("{pipeline: [{$sample: {}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    ASSERT_THROWS_CODE_AND_WHAT(
        PipelineParserGen(lexer, nullptr).parse(),
        AssertionException,
        ErrorCodes::FailedToParse,
        "syntax error, unexpected end of object, expecting size argument at element 'end object' "
        "within '$sample' within array at index 0 of input pipeline");
}

TEST(CstErrorTest, MissingRequiredArgumentOfMultiArgStage) {
    auto input = fromjson("{pipeline: [{$unionWith: {pipeline: 0.0}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    ASSERT_THROWS_CODE_AND_WHAT(
        PipelineParserGen(lexer, nullptr).parse(),
        AssertionException,
        ErrorCodes::FailedToParse,
        "syntax error, unexpected pipeline argument, expecting coll argument at element 'pipeline' "
        "within '$unionWith' within array at index 0 of input pipeline");
}

TEST(CstErrorTest, InvalidArgumentTypeForProjectionExpression) {
    auto input = fromjson("{pipeline: [{$project: {a: {$eq: '$b'}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    ASSERT_THROWS_CODE_AND_WHAT(PipelineParserGen(lexer, nullptr).parse(),
                                AssertionException,
                                ErrorCodes::FailedToParse,
                                "syntax error, unexpected $-prefixed string, expecting array at "
                                "element '$b' within '$eq' within "
                                "'$project' within array at index 0 of input pipeline");
}

TEST(CstErrorTest, MixedProjectionTypes) {
    auto input = fromjson("{pipeline: [{$project: {a: 1, b: 0}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    ASSERT_THROWS_CODE_AND_WHAT(
        PipelineParserGen(lexer, nullptr).parse(),
        AssertionException,
        ErrorCodes::FailedToParse,
        "$project containing inclusion and/or computed fields must contain no exclusion fields at "
        "element '$project' within array at index 0 of input pipeline");
}

TEST(CstErrorTest, DeeplyNestedSyntaxError) {
    auto input = fromjson("{pipeline: [{$project: {a: {$and: [1, {$or: [{$eq: '$b'}]}]}}}]}");
    BSONLexer lexer(input["pipeline"].Array(), PipelineParserGen::token::START_PIPELINE);
    ASSERT_THROWS_CODE_AND_WHAT(
        PipelineParserGen(lexer, nullptr).parse(),
        AssertionException,
        ErrorCodes::FailedToParse,
        "syntax error, unexpected $-prefixed string, expecting array at element '$b' within '$eq' "
        "within "
        "array at index 0 within '$or' within array at index 1 within '$and' within '$project' "
        "within array at index 0 of input pipeline");
}

}  // namespace
}  // namespace mongo
