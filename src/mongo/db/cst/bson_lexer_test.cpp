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
#include "mongo/db/cst/parser_gen.hpp"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(BSONLexerTest, TokenizesOpaqueUserObjects) {
    auto input = fromjson("{pipeline: [{a: 2, b: '1', c: \"$path\", d: \"$$NOW\"}]}");
    BSONLexer lexer(input["pipeline"]);
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::INT_OTHER, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STRING, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::DOLLAR_STRING, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::DOLLAR_DOLLAR_STRING, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OF_FILE, lexer.getNext().type_get());
}

TEST(BSONLexerTest, TokenizesReservedKeywords) {
    auto input = fromjson("{pipeline: [{$_internalInhibitOptimization: {}}]}");
    BSONLexer lexer(input["pipeline"]);
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STAGE_INHIBIT_OPTIMIZATION, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
}

TEST(BSONLexerTest, TokenizesReservedKeywordsAtAnyDepth) {
    auto input = fromjson("{pipeline: [{a: {$_internalInhibitOptimization: {}}}]}");
    BSONLexer lexer(input["pipeline"]);
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STAGE_INHIBIT_OPTIMIZATION, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
}

TEST(BSONLexerTest, MidRuleActionToSortNestedObject) {
    auto input = fromjson("{pipeline: [{pipeline: 2.0, coll: 'test'}]}");
    BSONLexer lexer(input["pipeline"]);
    // Iterate until the first object.
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    // Kick the lexer to sort the object, which should move element 'coll' in front of 'pipeline'.
    // Not that this only works because these are reserved keywords recognized by the lexer,
    // arbitrary string field names with *not* get sorted.
    lexer.sortObjTokens();
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::ARG_COLL, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STRING, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::DOUBLE_OTHER, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
}


TEST(BSONLexerTest, MidRuleActionToSortDoesNotSortNestedObjects) {
    auto input = fromjson(
        "{pipeline: [{$unionWith: {pipeline: [{$unionWith: 'inner', a: 3.0}], coll: 'outer'}}]}");
    BSONLexer lexer(input["pipeline"]);
    // Iterate until we reach the $unionWith object.
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STAGE_UNION_WITH, lexer.getNext().type_get());
    lexer.sortObjTokens();
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::ARG_COLL, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STRING, lexer.getNext().type_get());  // coll: 'outer'
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE,
              lexer.getNext().type_get());  // inner pipeline
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    // The nested pipeline does *not* get sorted, meaning '$unionWith' stays before 'a'.
    ASSERT_EQ(ParserGen::token::STAGE_UNION_WITH, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STRING, lexer.getNext().type_get());  // $unionWith: 'inner'
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::DOUBLE_OTHER, lexer.getNext().type_get());  // a: 1.0
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
}

TEST(BSONLexerTest, MultipleNestedObjectsAreReorderedCorrectly) {
    auto input = fromjson(
        "{pipeline: [{$unionWith: {pipeline: [{$unionWith: 'inner', a: 3.0}], coll: [{$unionWith: "
        "'innerB', a: 2.0}]}}]}");
    BSONLexer lexer(input["pipeline"]);
    // Iterate until we reach the $unionWith object.
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STAGE_UNION_WITH, lexer.getNext().type_get());
    lexer.sortObjTokens();
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::ARG_COLL, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    // The nested pipeline does *not* get sorted, meaning '$unionWith' stays before 'a'.
    ASSERT_EQ(ParserGen::token::STAGE_UNION_WITH, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STRING, lexer.getNext().type_get());        // innerb
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());     // a
    ASSERT_EQ(ParserGen::token::DOUBLE_OTHER, lexer.getNext().type_get());  // a: 2.0
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
    // Coll nested object ends here.
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE,
              lexer.getNext().type_get());  // inner pipeline
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    // The nested pipeline does *not* get sorted, meaning '$unionWith' stays before 'a'.
    ASSERT_EQ(ParserGen::token::STAGE_UNION_WITH, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STRING, lexer.getNext().type_get());        // $unionWith: 'inner'
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());     // a
    ASSERT_EQ(ParserGen::token::DOUBLE_OTHER, lexer.getNext().type_get());  // a: 1.0
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
}

TEST(BSONLexerTest, MultiLevelBSONDoesntSortChildren) {
    auto input = fromjson(
        "{pipeline: [{$unionWith: {pipeline: [{$unionWith: {'nested': 3.0, 'apple': 3.0}, a: 3.0}],"
        " coll: 'outer'}}]}");
    BSONLexer lexer(input["pipeline"]);
    // Iterate until we reach the $unionWith object.
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STAGE_UNION_WITH, lexer.getNext().type_get());
    lexer.sortObjTokens();
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::ARG_COLL, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STRING, lexer.getNext().type_get());  // coll: 'outer'
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE,
              lexer.getNext().type_get());  // inner pipeline
    // First nested object
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STAGE_UNION_WITH, lexer.getNext().type_get());
    // Second nested object
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());  // nested: 1.0
    ASSERT_EQ(ParserGen::token::DOUBLE_OTHER, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());  // apple: 1.0
    ASSERT_EQ(ParserGen::token::DOUBLE_OTHER, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    // End second nested object
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::DOUBLE_OTHER, lexer.getNext().type_get());  // a: 1.0
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    // End first nested object
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
}

TEST(BSONLexerTest, EmptyMatchExpressionsAreLexedCorrectly) {
    BSONLexer lexer(fromjson("{filter: {}}").firstElement());
    ASSERT_EQ(ParserGen::token::ARG_FILTER, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
}

TEST(BSONLexerTest, TokenizesObjWithPathCorrectly) {
    auto input = fromjson(
        "{pipeline: [{$project: { m: { $dateToString: { date: '$date', "
        "format: '%Y-%m-%d' } } } } ] }");
    BSONLexer lexer(input["pipeline"]);
    ASSERT_EQ(ParserGen::token::ARG_PIPELINE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_ARRAY, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STAGE_PROJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::DATE_TO_STRING, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::ARG_DATE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::DOLLAR_STRING, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::ARG_FORMAT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::STRING, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_ARRAY, lexer.getNext().type_get());
}

TEST(BSONLexerTest, SortSpecTokensGeneratedCorrectly) {
    auto input = fromjson("{sort: {val: 1, test: -1.0, rand: {$meta: 'textScore'}}}");
    BSONLexer lexer(input["sort"]);
    ASSERT_EQ(ParserGen::token::ARG_SORT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::INT_ONE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::DOUBLE_NEGATIVE_ONE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::FIELDNAME, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::START_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::META, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::TEXT_SCORE, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
    ASSERT_EQ(ParserGen::token::END_OBJECT, lexer.getNext().type_get());
}

}  // namespace
}  // namespace mongo
