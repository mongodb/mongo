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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/time_support.h"

#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using std::string;

TEST(MatchExpressionParserArrayTest, SizeAsNegativeLong) {
    BSONObj query = BSON("x" << BSON("$size" << -2LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserArrayTest, SizeTooLarge) {
    BSONObj query = BSON("x" << BSON("$size" << std::numeric_limits<long long>::max()));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserArrayTest, SizeAsString) {
    BSONObj query = BSON("x" << BSON("$size" << "a"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserArrayTest, SizeWithNegativeIntegralDouble) {
    BSONObj query = BSON("x" << BSON("$size" << -2.0));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserArrayTest, SizeWithTooLargeDouble) {
    BSONObj query = BSON("x" << BSON("$size" << 1e12));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserArrayTest, SizeWithDouble) {
    BSONObj query = BSON("x" << BSON("$size" << 2.5));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserArrayTest, SizeWithNegative) {
    BSONObj query = BSON("x" << BSON("$size" << -2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserArrayTest, SizeBad) {
    BSONObj query = BSON("x" << BSON("$size" << BSONNULL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserArrayTest, AllBadArg) {
    BSONObj query = BSON("x" << BSON("$all" << 1));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserArrayTest, AllBadRegexArg) {
    string tooLargePattern(50 * 1000, 'z');
    BSONObjBuilder allArray;
    allArray.appendRegex("0", tooLargePattern, "");
    BSONObjBuilder operand;
    operand.appendArray("$all", allArray.obj());

    BSONObj query = BSON("x" << operand.obj());

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

// Check the structure of the resulting MatchExpression, and make sure that the paths
// are correct.
TEST(MatchExpressionParserArrayTest, AllElemMatch3) {
    BSONObj query = fromjson("{x: {$all: [{$elemMatch: {y: 1, z: 1}}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    std::unique_ptr<MatchExpression> expr = std::move(result.getValue());

    // Root node should be an AND with one child.
    ASSERT_EQUALS(MatchExpression::AND, expr->matchType());
    ASSERT_EQUALS(1U, expr->numChildren());

    // Child should be an ELEM_MATCH_OBJECT with one child and path "x".
    MatchExpression* emObject = expr->getChild(0);
    ASSERT_EQUALS(MatchExpression::ELEM_MATCH_OBJECT, emObject->matchType());
    ASSERT_EQUALS(1U, emObject->numChildren());
    ASSERT_EQUALS("x", emObject->path());

    // Child should be another AND with two children.
    MatchExpression* and2 = emObject->getChild(0);
    ASSERT_EQUALS(MatchExpression::AND, and2->matchType());
    ASSERT_EQUALS(2U, and2->numChildren());

    // Both children should be equalites, with paths "y" and "z".
    MatchExpression* leaf1 = and2->getChild(0);
    ASSERT_EQUALS(MatchExpression::EQ, leaf1->matchType());
    ASSERT_EQUALS(0U, leaf1->numChildren());
    ASSERT_EQUALS("y", leaf1->path());
    MatchExpression* leaf2 = and2->getChild(1);
    ASSERT_EQUALS(MatchExpression::EQ, leaf2->matchType());
    ASSERT_EQUALS(0U, leaf2->numChildren());
    ASSERT_EQUALS("z", leaf2->path());
}

TEST(MatchExpressionParserArrayTest, AllElemMatchBad) {
    BSONObj internal = BSON("x" << 1 << "y" << 2);

    BSONObj query = BSON("x" << BSON("$all" << BSON_ARRAY(BSON("$elemMatch" << internal) << 5)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());

    query = BSON("x" << BSON("$all" << BSON_ARRAY(5 << BSON("$elemMatch" << internal))));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_FALSE(result.isOK());
}

// You can't mix $elemMatch and regular equality inside $all.
TEST(MatchExpressionParserArrayTest, AllElemMatchBadMixed) {
    // $elemMatch first, equality second.
    BSONObj bad1 = fromjson("{x: {$all: [{$elemMatch: {y: 1}}, 3]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result1 = MatchExpressionParser::parse(bad1, expCtx);
    ASSERT_FALSE(result1.isOK());

    // equality first, $elemMatch second
    BSONObj bad2 = fromjson("{x: {$all: [3, {$elemMatch: {y: 1}}]}}");
    StatusWithMatchExpression result2 = MatchExpressionParser::parse(bad2, expCtx);
    ASSERT_FALSE(result1.isOK());

    // $elemMatch first, object second
    BSONObj bad3 = fromjson("{x: {$all: [{$elemMatch: {y: 1}}, {z: 1}]}}");
    StatusWithMatchExpression result3 = MatchExpressionParser::parse(bad3, expCtx);
    ASSERT_FALSE(result3.isOK());

    // object first, $elemMatch second
    BSONObj bad4 = fromjson("{x: {$all: [{z: 1}, {$elemMatch: {y: 1}}]}}");
    StatusWithMatchExpression result4 = MatchExpressionParser::parse(bad4, expCtx);
    ASSERT_FALSE(result4.isOK());
}

TEST(MatchExpressionParserArrayTest, AllStringNullCollation) {
    BSONObj query = BSON("x" << BSON("$all" << BSON_ARRAY("string")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::AND, result.getValue()->matchType());
    ASSERT_EQUALS(1U, result.getValue()->numChildren());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::EQ, child->matchType());
    EqualityMatchExpression* eqMatch = static_cast<EqualityMatchExpression*>(child);
    ASSERT_TRUE(eqMatch->getCollator() == nullptr);
}

TEST(MatchExpressionParserArrayTest, AllStringCollation) {
    BSONObj query = BSON("x" << BSON("$all" << BSON_ARRAY("string")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::AND, result.getValue()->matchType());
    ASSERT_EQUALS(1U, result.getValue()->numChildren());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::EQ, child->matchType());
    EqualityMatchExpression* eqMatch = static_cast<EqualityMatchExpression*>(child);
    ASSERT_TRUE(eqMatch->getCollator() == expCtx->getCollator());
}
}  // namespace mongo
