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
#include "mongo/db/cst/pipeline_parser_gen.hpp"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

void assertTokensMatch(BSONLexer& lexer,
                       std::vector<PipelineParserGen::token::yytokentype> tokens) {
    for (auto&& token : tokens) {
        ASSERT_EQ(lexer.getNext().type_get(), token);
    }
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::END_OF_FILE);
}

TEST(BSONLexerTest, TokenizesOpaqueUserObjects) {
    auto input = fromjson("{pipeline: [{a: 1, b: '1'}]}");
    BSONLexer lexer(input["pipeline"].Array());
    assertTokensMatch(lexer,
                      {PipelineParserGen::token::START_ARRAY,
                       PipelineParserGen::token::START_OBJECT,
                       PipelineParserGen::token::STRING,
                       PipelineParserGen::token::NUMBER_INT,
                       PipelineParserGen::token::STRING,
                       PipelineParserGen::token::STRING,
                       PipelineParserGen::token::END_OBJECT,
                       PipelineParserGen::token::END_ARRAY});
}

TEST(BSONLexerTest, TokenizesReservedKeywords) {
    auto input = fromjson("{pipeline: [{$_internalInhibitOptimization: {}}]}");
    BSONLexer lexer(input["pipeline"].Array());
    assertTokensMatch(lexer,
                      {PipelineParserGen::token::START_ARRAY,
                       PipelineParserGen::token::START_OBJECT,
                       PipelineParserGen::token::STAGE_INHIBIT_OPTIMIZATION,
                       PipelineParserGen::token::START_OBJECT,
                       PipelineParserGen::token::END_OBJECT,
                       PipelineParserGen::token::END_OBJECT,
                       PipelineParserGen::token::END_ARRAY});
}

TEST(BSONLexerTest, TokenizesReservedKeywordsAtAnyDepth) {
    auto input = fromjson("{pipeline: [{a: {$_internalInhibitOptimization: {}}}]}");
    BSONLexer lexer(input["pipeline"].Array());
    assertTokensMatch(lexer,
                      {PipelineParserGen::token::START_ARRAY,
                       PipelineParserGen::token::START_OBJECT,
                       PipelineParserGen::token::STRING,
                       PipelineParserGen::token::START_OBJECT,
                       PipelineParserGen::token::STAGE_INHIBIT_OPTIMIZATION,
                       PipelineParserGen::token::START_OBJECT,
                       PipelineParserGen::token::END_OBJECT,
                       PipelineParserGen::token::END_OBJECT,
                       PipelineParserGen::token::END_OBJECT,
                       PipelineParserGen::token::END_ARRAY});
}

TEST(BSONLexerTest, MidRuleActionToSortNestedObject) {
    auto input = fromjson("{pipeline: [{pipeline: 1, coll: 'test'}]}");
    BSONLexer lexer(input["pipeline"].Array());
    // Iterate until the first object.
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::START_ARRAY);
    // Kick the lexer to sort the object, which should move element 'coll' in front of 'pipeline'.
    // Not that this only works because these are reserved keywords recognized by the lexer,
    // arbitrary string field names with *not* get sorted.
    lexer.sortObjTokens();
    auto expected = {PipelineParserGen::token::START_OBJECT,
                     PipelineParserGen::token::COLL_ARG,
                     PipelineParserGen::token::STRING,
                     PipelineParserGen::token::PIPELINE_ARG,
                     PipelineParserGen::token::NUMBER_INT,
                     PipelineParserGen::token::END_OBJECT,
                     PipelineParserGen::token::END_ARRAY};
    assertTokensMatch(lexer, expected);
}


TEST(BSONLexerTest, MidRuleActionToSortDoesNotSortNestedObjects) {
    auto input = fromjson(
        "{pipeline: [{$unionWith: {pipeline: [{$unionWith: 'inner', a: 1}], coll: 'outer'}}]}");
    BSONLexer lexer(input["pipeline"].Array());
    // Iterate until we reach the $unionWith object.
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::START_ARRAY);
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::START_OBJECT);
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::STAGE_UNION_WITH);
    lexer.sortObjTokens();
    auto expected = {
        PipelineParserGen::token::START_OBJECT,
        PipelineParserGen::token::COLL_ARG,
        PipelineParserGen::token::STRING,        // coll: 'outer'
        PipelineParserGen::token::PIPELINE_ARG,  // inner pipeline
        PipelineParserGen::token::START_ARRAY,
        PipelineParserGen::token::START_OBJECT,
        // The nested pipeline does *not* get sorted, meaning '$unionWith' stays before 'a'.
        PipelineParserGen::token::STAGE_UNION_WITH,
        PipelineParserGen::token::STRING,  // $unionWith: 'inner'
        PipelineParserGen::token::STRING,
        PipelineParserGen::token::NUMBER_INT,  // a: 1
        PipelineParserGen::token::END_OBJECT,
        PipelineParserGen::token::END_ARRAY,
        PipelineParserGen::token::END_OBJECT,
        PipelineParserGen::token::END_OBJECT,
        PipelineParserGen::token::END_ARRAY,
    };
    assertTokensMatch(lexer, expected);
}

TEST(BSONLexerTest, MultipleNestedObjectsAreReorderedCorrectly) {
    auto input = fromjson(
        "{pipeline: [{$unionWith: {pipeline: [{$unionWith: 'inner', a: 1}], coll: [{$unionWith: "
        "'innerB', a: 2}]}}]}");
    BSONLexer lexer(input["pipeline"].Array());
    // Iterate until we reach the $unionWith object.
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::START_ARRAY);
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::START_OBJECT);
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::STAGE_UNION_WITH);
    lexer.sortObjTokens();
    auto expected = {
        PipelineParserGen::token::START_OBJECT,
        PipelineParserGen::token::COLL_ARG,
        PipelineParserGen::token::START_ARRAY,
        PipelineParserGen::token::START_OBJECT,
        // The nested pipeline does *not* get sorted, meaning '$unionWith' stays before 'a'.
        PipelineParserGen::token::STAGE_UNION_WITH,
        PipelineParserGen::token::STRING,      // innerb
        PipelineParserGen::token::STRING,      // a
        PipelineParserGen::token::NUMBER_INT,  // a: 2
        PipelineParserGen::token::END_OBJECT,
        PipelineParserGen::token::END_ARRAY,
        // Coll nested object ends here.
        PipelineParserGen::token::PIPELINE_ARG,  // inner pipeline
        PipelineParserGen::token::START_ARRAY,
        PipelineParserGen::token::START_OBJECT,
        // The nested pipeline does *not* get sorted, meaning '$unionWith' stays before 'a'.
        PipelineParserGen::token::STAGE_UNION_WITH,
        PipelineParserGen::token::STRING,      // $unionWith: 'inner'
        PipelineParserGen::token::STRING,      // a
        PipelineParserGen::token::NUMBER_INT,  // a: 1
        PipelineParserGen::token::END_OBJECT,
        PipelineParserGen::token::END_ARRAY,
        PipelineParserGen::token::END_OBJECT,
        PipelineParserGen::token::END_OBJECT,
        PipelineParserGen::token::END_ARRAY,
    };
    assertTokensMatch(lexer, expected);
}
TEST(BSONLexerTest, MultiLevelBSONDoesntSortChildren) {
    auto input = fromjson(
        "{pipeline: [{$unionWith: {pipeline: [{$unionWith: {'nested': 1, 'apple': 1}, a: 1}], "
        "coll: 'outer'}}]}");
    BSONLexer lexer(input["pipeline"].Array());
    // Iterate until we reach the $unionWith object.
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::START_ARRAY);
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::START_OBJECT);
    ASSERT_EQ(lexer.getNext().type_get(), PipelineParserGen::token::STAGE_UNION_WITH);
    lexer.sortObjTokens();
    auto expected = {
        PipelineParserGen::token::START_OBJECT,
        PipelineParserGen::token::COLL_ARG,
        PipelineParserGen::token::STRING,        // coll: 'outer'
        PipelineParserGen::token::PIPELINE_ARG,  // inner pipeline
        // First nested object
        PipelineParserGen::token::START_ARRAY,
        PipelineParserGen::token::START_OBJECT,
        PipelineParserGen::token::STAGE_UNION_WITH,
        // Second nested object
        PipelineParserGen::token::START_OBJECT,
        PipelineParserGen::token::STRING,  // nested: 1
        PipelineParserGen::token::NUMBER_INT,
        PipelineParserGen::token::STRING,  // apple: 1
        PipelineParserGen::token::NUMBER_INT,
        PipelineParserGen::token::END_OBJECT,
        // End second nested object
        PipelineParserGen::token::STRING,
        PipelineParserGen::token::NUMBER_INT,  // a: 1
        PipelineParserGen::token::END_OBJECT,
        // End first nested object
        PipelineParserGen::token::END_ARRAY,
        PipelineParserGen::token::END_OBJECT,
        PipelineParserGen::token::END_OBJECT,
        PipelineParserGen::token::END_ARRAY,
    };
    assertTokensMatch(lexer, expected);
}

}  // namespace
}  // namespace mongo
