/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

TEST(InternalSchemaAllowedPropertiesMatchExpression, EquivalentToClone) {
    auto filter = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a'], namePlaceholder: 'i',"
        "patternProperties: [{regex: /a/, expression: {i: 1}}], otherwise: {i: 7}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    auto clone = expr.getValue()->clone();
    ASSERT_TRUE(expr.getValue()->equivalent(clone.get()));

    filter = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: [], namePlaceholder: 'i',"
        "patternProperties: [], otherwise: {}}}");
    expr = MatchExpressionParser::parse(filter, expCtx);
    ASSERT_OK(expr.getStatus());
    clone = expr.getValue()->clone();
    ASSERT_TRUE(expr.getValue()->equivalent(clone.get()));
}

TEST(InternalSchemaAllowedPropertiesMatchExpression, HasCorrectNumberOfChilden) {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a'], namePlaceholder: 'i',"
        "patternProperties: [{regex: /a/, expression: {i: 1}}], otherwise: {i: 7}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    ASSERT_EQ(objMatch.getValue()->numChildren(), 2U);
    ASSERT(objMatch.getValue()->getChild(0));
}

DEATH_TEST_REGEX(InternalSchemaAllowedPropertiesMatchExpression,
                 GetChildFailsOnIndexLargerThanChildSet,
                 "Tripwire assertion.*6400212") {
    auto query = fromjson(
        "{$_internalSchemaAllowedProperties: {properties: ['a'], namePlaceholder: 'i',"
        "patternProperties: [{regex: /a/, expression: {i: 1}}], otherwise: {i: 7}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto objMatch = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(objMatch.getStatus());

    const size_t numChildren = 2;
    ASSERT_EQ(objMatch.getValue()->numChildren(), numChildren);
    ASSERT_THROWS_CODE(objMatch.getValue()->getChild(numChildren), AssertionException, 6400212);
}
}  // namespace mongo
