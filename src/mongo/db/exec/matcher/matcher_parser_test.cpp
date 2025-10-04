/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::evaluate_matcher_test {

TEST(MatchExpressionParserTreeTest, OR1) {
    BSONObj query = BSON("$or" << BSON_ARRAY(BSON("x" << 1) << BSON("y" << 2)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("y" << 2)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("y" << 1)));
}

TEST(MatchExpressionParserTreeTest, OREmbedded) {
    BSONObj query1 = BSON("$or" << BSON_ARRAY(BSON("x" << 1) << BSON("y" << 2)));
    BSONObj query2 = BSON("$or" << BSON_ARRAY(query1));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query2, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("y" << 2)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("y" << 1)));
}


TEST(MatchExpressionParserTreeTest, AND1) {
    BSONObj query = BSON("$and" << BSON_ARRAY(BSON("x" << 1) << BSON("y" << 2)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("y" << 2)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("y" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1 << "y" << 2)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2 << "y" << 2)));
}

TEST(MatchExpressionParserTreeTest, NOREmbedded) {
    BSONObj query = BSON("$nor" << BSON_ARRAY(BSON("x" << 1) << BSON("y" << 2)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("y" << 2)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("y" << 1)));
}

TEST(MatchExpressionParserTreeTest, NOT1) {
    BSONObj query = BSON("x" << BSON("$not" << BSON("$gt" << 5)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 8)));
}

TEST(MatchExpressionParserLeafTest, NotRegex1) {
    BSONObjBuilder b;
    b.appendRegex("$not", "abc", "i");
    BSONObj query = BSON("x" << b.obj());
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ABC")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "AC")));
}

TEST(MatchExpressionParserArrayTest, Size1) {
    BSONObj query = BSON("x" << BSON("$size" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        !exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserArrayTest, SizeAsLong) {
    BSONObj query = BSON("x" << BSON("$size" << 2LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        !exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserArrayTest, SizeWithIntegralDouble) {
    BSONObj query = BSON("x" << BSON("$size" << 2.0));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(
        !exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
}

TEST(MatchExpressionParserArrayTest, ElemMatchArr1) {
    BSONObj query = BSON("x" << BSON("$elemMatch" << BSON("x" << 1 << "y" << 2)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("x" << 1)))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSON("x" << 1 << "y" << 2)))));
}

TEST(MatchExpressionParserArrayTest, ElemMatchAnd) {
    BSONObj query =
        BSON("x" << BSON("$elemMatch" << BSON("$and" << BSON_ARRAY(BSON("x" << 1 << "y" << 2)))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("x" << 1)))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSON("x" << 1 << "y" << 2)))));
}

TEST(MatchExpressionParserArrayTest, ElemMatchNor) {
    BSONObj query = BSON("x" << BSON("$elemMatch" << BSON("$nor" << BSON_ARRAY(BSON("x" << 1)))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("x" << 1)))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSON("x" << 2 << "y" << 2)))));
}

TEST(MatchExpressionParserArrayTest, ElemMatchOr) {
    BSONObj query =
        BSON("x" << BSON("$elemMatch" << BSON("$or" << BSON_ARRAY(BSON("x" << 1 << "y" << 2)))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("x" << 1)))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSON("x" << 1 << "y" << 2)))));
}

TEST(MatchExpressionParserArrayTest, ElemMatchVal1) {
    BSONObj query = BSON("x" << BSON("$elemMatch" << BSON("$gt" << 5)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(4))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(6))));
}

// with explicit $eq
TEST(MatchExpressionParserArrayTest, ElemMatchDBRef1) {
    OID oid = OID::gen();
    BSONObj match = BSON("$ref" << "coll"
                                << "$id" << oid << "$db"
                                << "db");
    OID oidx = OID::gen();
    BSONObj notMatch = BSON("$ref" << "coll"
                                   << "$id" << oidx << "$db"
                                   << "db");

    BSONObj query = BSON("x" << BSON("$elemMatch" << BSON("$eq" << match)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << match)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(notMatch))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(match))));
}

TEST(MatchExpressionParserArrayTest, ElemMatchDBRef2) {
    OID oid = OID::gen();
    BSONObj match = BSON("$ref" << "coll"
                                << "$id" << oid << "$db"
                                << "db");
    OID oidx = OID::gen();
    BSONObj notMatch = BSON("$ref" << "coll"
                                   << "$id" << oidx << "$db"
                                   << "db");

    BSONObj query = BSON("x" << BSON("$elemMatch" << match));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << match)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(notMatch))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(match))));
}

// Additional fields after $ref and $id.
TEST(MatchExpressionParserArrayTest, ElemMatchDBRef3) {
    OID oid = OID::gen();
    BSONObj match = BSON("$ref" << "coll"
                                << "$id" << oid << "foo" << 12345);
    OID oidx = OID::gen();
    BSONObj notMatch = BSON("$ref" << "coll"
                                   << "$id" << oidx << "foo" << 12345);

    BSONObj query = BSON("x" << BSON("$elemMatch" << match));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << match)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(notMatch))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(match))));

    // Document contains fields not referred to in $elemMatch query.
    ASSERT(exec::matcher::matchesBSON(
        result.getValue().get(),
        BSON("x" << BSON_ARRAY(BSON("$ref" << "coll"
                                           << "$id" << oid << "foo" << 12345 << "bar" << 678)))));
}

// Query with DBRef fields out of order.
TEST(MatchExpressionParserArrayTest, ElemMatchDBRef4) {
    OID oid = OID::gen();
    BSONObj match = BSON("$ref" << "coll"
                                << "$id" << oid << "$db"
                                << "db");
    BSONObj matchOutOfOrder = BSON("$db" << "db"
                                         << "$id" << oid << "$ref"
                                         << "coll");
    OID oidx = OID::gen();
    BSONObj notMatch = BSON("$ref" << "coll"
                                   << "$id" << oidx << "$db"
                                   << "db");

    BSONObj query = BSON("x" << BSON("$elemMatch" << matchOutOfOrder));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << match)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(notMatch))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(match))));
}

// Query with DBRef fields out of order.
// Additional fields besides $ref and $id.
TEST(MatchExpressionParserArrayTest, ElemMatchDBRef5) {
    OID oid = OID::gen();
    BSONObj match = BSON("$ref" << "coll"
                                << "$id" << oid << "foo" << 12345);
    BSONObj matchOutOfOrder = BSON("foo" << 12345 << "$id" << oid << "$ref"
                                         << "coll");
    OID oidx = OID::gen();
    BSONObj notMatch = BSON("$ref" << "coll"
                                   << "$id" << oidx << "foo" << 12345);

    BSONObj query = BSON("x" << BSON("$elemMatch" << matchOutOfOrder));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << match)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(notMatch))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(match))));

    // Document contains fields not referred to in $elemMatch query.
    ASSERT(exec::matcher::matchesBSON(
        result.getValue().get(),
        BSON("x" << BSON_ARRAY(BSON("$ref" << "coll"
                                           << "$id" << oid << "foo" << 12345 << "bar" << 678)))));
}

// Incomplete DBRef - $id missing.
TEST(MatchExpressionParserArrayTest, ElemMatchDBRef6) {
    OID oid = OID::gen();
    BSONObj match = BSON("$ref" << "coll"
                                << "$id" << oid << "foo" << 12345);
    BSONObj matchMissingID = BSON("$ref" << "coll"
                                         << "foo" << 12345);
    BSONObj notMatch = BSON("$ref" << "collx"
                                   << "$id" << oid << "foo" << 12345);

    BSONObj query = BSON("x" << BSON("$elemMatch" << matchMissingID));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << match)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(notMatch))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(match))));

    // Document contains fields not referred to in $elemMatch query.
    ASSERT(exec::matcher::matchesBSON(
        result.getValue().get(),
        BSON("x" << BSON_ARRAY(BSON("$ref" << "coll"
                                           << "$id" << oid << "foo" << 12345 << "bar" << 678)))));
}

// Incomplete DBRef - $ref missing.
TEST(MatchExpressionParserArrayTest, ElemMatchDBRef7) {
    OID oid = OID::gen();
    BSONObj match = BSON("$ref" << "coll"
                                << "$id" << oid << "foo" << 12345);
    BSONObj matchMissingRef = BSON("$id" << oid << "foo" << 12345);
    OID oidx = OID::gen();
    BSONObj notMatch = BSON("$ref" << "coll"
                                   << "$id" << oidx << "foo" << 12345);

    BSONObj query = BSON("x" << BSON("$elemMatch" << matchMissingRef));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << match)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(notMatch))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(match))));

    // Document contains fields not referred to in $elemMatch query.
    ASSERT(exec::matcher::matchesBSON(
        result.getValue().get(),
        BSON("x" << BSON_ARRAY(BSON("$ref" << "coll"
                                           << "$id" << oid << "foo" << 12345 << "bar" << 678)))));
}

// Incomplete DBRef - $db only.
TEST(MatchExpressionParserArrayTest, ElemMatchDBRef8) {
    OID oid = OID::gen();
    BSONObj match = BSON("$ref" << "coll"
                                << "$id" << oid << "$db"
                                << "db"
                                << "foo" << 12345);
    BSONObj matchDBOnly = BSON("$db" << "db"
                                     << "foo" << 12345);
    BSONObj notMatch = BSON("$ref" << "coll"
                                   << "$id" << oid << "$db"
                                   << "dbx"
                                   << "foo" << 12345);

    BSONObj query = BSON("x" << BSON("$elemMatch" << matchDBOnly));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << match)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(notMatch))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(match))));

    // Document contains fields not referred to in $elemMatch query.
    ASSERT(exec::matcher::matchesBSON(
        result.getValue().get(),
        BSON("x" << BSON_ARRAY(BSON("$ref" << "coll"
                                           << "$id" << oid << "$db"
                                           << "db"
                                           << "foo" << 12345 << "bar" << 678)))));
}

TEST(MatchExpressionParserArrayTest, All1) {
    BSONObj query = BSON("x" << BSON("$all" << BSON_ARRAY(1 << 2)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    // Verify that the $all got parsed to AND.
    ASSERT_EQUALS(MatchExpression::AND, result.getValue()->matchType());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(2))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(
        exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2 << 3))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(2 << 3))));
}

TEST(MatchExpressionParserArrayTest, AllNull) {
    BSONObj query = BSON("x" << BSON("$all" << BSON_ARRAY(BSONNULL)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    // Verify that the $all got parsed to AND.
    ASSERT_EQUALS(MatchExpression::AND, result.getValue()->matchType());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSONNULL)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(BSONNULL))));
}


TEST(MatchExpressionParserArrayTest, AllRegex1) {
    BSONObjBuilder allArray;
    allArray.appendRegex("0", "^a", "");
    allArray.appendRegex("1", "B", "i");
    BSONObjBuilder all;
    all.appendArray("$all", allArray.obj());
    BSONObj query = BSON("a" << all.obj());

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    // Verify that the $all got parsed to AND.
    ASSERT_EQUALS(MatchExpression::AND, result.getValue()->matchType());

    BSONObj notMatchFirst = BSON("a" << "ax");
    BSONObj notMatchSecond = BSON("a" << "qqb");
    BSONObj matchesBoth = BSON("a" << "ab");

    ASSERT(!exec::matcher::matchesSingleElement(result.getValue().get(), notMatchFirst["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(result.getValue().get(), notMatchSecond["a"]));
    ASSERT(exec::matcher::matchesSingleElement(result.getValue().get(), matchesBoth["a"]));
}

TEST(MatchExpressionParserArrayTest, AllRegex2) {
    BSONObjBuilder allArray;
    allArray.appendRegex("0", "^a", "");
    allArray.append("1", "abc");
    BSONObjBuilder all;
    all.appendArray("$all", allArray.obj());
    BSONObj query = BSON("a" << all.obj());

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    // Verify that the $all got parsed to AND.
    ASSERT_EQUALS(MatchExpression::AND, result.getValue()->matchType());

    BSONObj notMatchFirst = BSON("a" << "ax");
    BSONObj matchesBoth = BSON("a" << "abc");

    ASSERT(!exec::matcher::matchesSingleElement(result.getValue().get(), notMatchFirst["a"]));
    ASSERT(exec::matcher::matchesSingleElement(result.getValue().get(), matchesBoth["a"]));
}

TEST(MatchExpressionParserArrayTest, AllNonArray) {
    BSONObj query = BSON("x" << BSON("$all" << BSON_ARRAY(5)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    // Verify that the $all got parsed to AND.
    ASSERT_EQUALS(MatchExpression::AND, result.getValue()->matchType());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(5))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 4)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(4))));
}


TEST(MatchExpressionParserArrayTest, AllElemMatch1) {
    BSONObj internal = BSON("x" << 1 << "y" << 2);
    BSONObj query = BSON("x" << BSON("$all" << BSON_ARRAY(BSON("$elemMatch" << internal))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    // Verify that the $all got parsed to an AND with a single ELEM_MATCH_OBJECT child.
    ASSERT_EQUALS(MatchExpression::AND, result.getValue()->matchType());
    ASSERT_EQUALS(1U, result.getValue()->numChildren());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::ELEM_MATCH_OBJECT, child->matchType());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON_ARRAY(1 << 2))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("x" << 1)))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSON("x" << 1 << "y" << 2)))));
}

// $all and $elemMatch on dotted field.
// Top level field can be either document or array.
TEST(MatchExpressionParserArrayTest, AllElemMatch2) {
    BSONObj internal = BSON("z" << 1);
    BSONObj query = BSON("x.y" << BSON("$all" << BSON_ARRAY(BSON("$elemMatch" << internal))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    // Verify that the $all got parsed to an AND with a single ELEM_MATCH_OBJECT child.
    ASSERT_EQUALS(MatchExpression::AND, result.getValue()->matchType());
    ASSERT_EQUALS(1U, result.getValue()->numChildren());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::ELEM_MATCH_OBJECT, child->matchType());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSON("y" << 1))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("y" << BSON_ARRAY(1 << 2)))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("y" << BSON_ARRAY(BSON("x" << 1))))));
    // x is a document. Internal document does not contain z.
    ASSERT(!exec::matcher::matchesBSON(
        result.getValue().get(), BSON("x" << BSON("y" << BSON_ARRAY(BSON("x" << 1 << "y" << 1))))));
    // x is an array. Internal document does not contain z.
    ASSERT(!exec::matcher::matchesBSON(
        result.getValue().get(),
        BSON("x" << BSON_ARRAY(BSON("y" << BSON_ARRAY(BSON("x" << 1 << "y" << 1)))))));
    // x is a document but y is not an array.
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("y" << BSON("x" << 1 << "z" << 1)))));
    // x is an array but y is not an array.
    ASSERT(!exec::matcher::matchesBSON(
        result.getValue().get(), BSON("x" << BSON_ARRAY(BSON("y" << BSON("x" << 1 << "z" << 1))))));
    // x is a document.
    ASSERT(exec::matcher::matchesBSON(
        result.getValue().get(), BSON("x" << BSON("y" << BSON_ARRAY(BSON("x" << 1 << "z" << 1))))));
    // x is an array.
    ASSERT(exec::matcher::matchesBSON(
        result.getValue().get(),
        BSON("x" << BSON_ARRAY(BSON("y" << BSON_ARRAY(BSON("x" << 1 << "z" << 1)))))));
}

// $all with empty string.
TEST(MatchExpressionParserArrayTest, AllEmptyString) {
    BSONObj query = BSON("x" << BSON("$all" << BSON_ARRAY("")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSONNULL << "a"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSONObj() << "a"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSONArray())));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSONNULL << ""))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSONObj() << ""))));
}

// $all with ISO date.
TEST(MatchExpressionParserArrayTest, AllISODate) {
    StatusWith<Date_t> matchResult = dateFromISOString("2014-12-31T00:00:00.000Z");
    ASSERT_TRUE(matchResult.isOK());
    const Date_t& match = matchResult.getValue();
    StatusWith<Date_t> notMatchResult = dateFromISOString("2014-12-30T00:00:00.000Z");
    ASSERT_TRUE(notMatchResult.isOK());
    const Date_t& notMatch = notMatchResult.getValue();

    BSONObj query = BSON("x" << BSON("$all" << BSON_ARRAY(match)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << notMatch)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSONNULL << notMatch))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSONObj() << notMatch))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSONArray())));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << match)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSONNULL << match))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSONObj() << match))));
}

// $all on array element with empty string.
TEST(MatchExpressionParserArrayTest, AllDottedEmptyString) {
    BSONObj query = BSON("x.1" << BSON("$all" << BSON_ARRAY("")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSONNULL << "a"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSONObj() << "a"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY("" << BSONNULL))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY("" << BSONObj()))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSONArray())));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSONNULL << ""))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSONObj() << ""))));
}

// $all on array element with ISO date.
TEST(MatchExpressionParserArrayTest, AllDottedISODate) {
    StatusWith<Date_t> matchResult = dateFromISOString("2014-12-31T00:00:00.000Z");
    ASSERT_TRUE(matchResult.isOK());
    const Date_t& match = matchResult.getValue();
    StatusWith<Date_t> notMatchResult = dateFromISOString("2014-12-30T00:00:00.000Z");
    ASSERT_TRUE(notMatchResult.isOK());
    const Date_t& notMatch = notMatchResult.getValue();

    BSONObj query = BSON("x.1" << BSON("$all" << BSON_ARRAY(match)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << notMatch)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSONNULL << notMatch))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSONObj() << notMatch))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(match << BSONNULL))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(match << BSONObj()))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << BSONArray())));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << match)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSONNULL << match))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSONObj() << match))));
}

TEST(MatchExpressionParserLeafTest, SimpleEQ2) {
    BSONObj query = BSON("x" << BSON("$eq" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, SimpleGT1) {
    BSONObj query = BSON("x" << BSON("$gt" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, SimpleLT1) {
    BSONObj query = BSON("x" << BSON("$lt" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, SimpleLTE1) {
    BSONObj query = BSON("x" << BSON("$lte" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, SimpleGTE1) {
    BSONObj query = BSON("x" << BSON("$gte" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, INSingleDBRef) {
    OID oid = OID::gen();
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref" << "coll"
                                                                     << "$id" << oid << "$db"
                                                                     << "db"))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    OID oidx = OID::gen();
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("$ref" << "collx"
                                                               << "$id" << oidx << "$db"
                                                               << "db"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("$ref" << "coll"
                                                               << "$id" << oidx << "$db"
                                                               << "db"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("$id" << oid << "$ref"
                                                              << "coll"
                                                              << "$db"
                                                              << "db"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("$id" << oid << "$ref"
                                                              << "coll"
                                                              << "$db"
                                                              << "db"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("$id" << oid << "$ref"
                                                                         << "coll"
                                                                         << "$db"
                                                                         << "db")))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("$ref" << "coll"
                                                               << "$id" << oid << "$db"
                                                               << "dbx"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("$db" << "db"
                                                              << "$ref"
                                                              << "coll"
                                                              << "$id" << oid))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON("$ref" << "coll"
                                                              << "$id" << oid << "$db"
                                                              << "db"))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSON("$ref" << "coll"
                                                                         << "$id" << oid << "$db"
                                                                         << "db")))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSON("$ref" << "collx"
                                                                         << "$id" << oidx << "$db"
                                                                         << "db")
                                                             << BSON("$ref" << "coll"
                                                                            << "$id" << oid << "$db"
                                                                            << "db")))));
}

TEST(MatchExpressionParserLeafTest, INMultipleDBRef) {
    OID oid = OID::gen();
    OID oidy = OID::gen();
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref" << "colly"
                                                                     << "$id" << oidy << "$db"
                                                                     << "db")
                                                         << BSON("$ref" << "coll"
                                                                        << "$id" << oid << "$db"
                                                                        << "db"))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    OID oidx = OID::gen();
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("$ref" << "collx"
                                                               << "$id" << oidx << "$db"
                                                               << "db"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("$ref" << "coll"
                                                               << "$id" << oidx << "$db"
                                                               << "db"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("$id" << oid << "$ref"
                                                              << "coll"
                                                              << "$db"
                                                              << "db"))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("$ref" << "coll"
                                                                          << "$id" << oidy << "$db"
                                                                          << "db")))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("$ref" << "colly"
                                                                          << "$id" << oid << "$db"
                                                                          << "db")))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("$id" << oid << "$ref"
                                                                         << "coll"
                                                                         << "$db"
                                                                         << "db")))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("$ref" << "coll"
                                                                          << "$id" << oid << "$db"
                                                                          << "dbx")))));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON_ARRAY(BSON("$id" << oidy << "$ref"
                                                                         << "colly"
                                                                         << "$db"
                                                                         << "db")))));
    ASSERT(
        !exec::matcher::matchesBSON(result.getValue().get(),
                                    BSON("x" << BSON_ARRAY(BSON("$ref" << "collx"
                                                                       << "$id" << oidx << "$db"
                                                                       << "db")
                                                           << BSON("$ref" << "coll"
                                                                          << "$id" << oidx << "$db"
                                                                          << "db")))));
    ASSERT(
        !exec::matcher::matchesBSON(result.getValue().get(),
                                    BSON("x" << BSON_ARRAY(BSON("$ref" << "collx"
                                                                       << "$id" << oidx << "$db"
                                                                       << "db")
                                                           << BSON("$ref" << "colly"
                                                                          << "$id" << oidx << "$db"
                                                                          << "db")))));
    ASSERT(
        !exec::matcher::matchesBSON(result.getValue().get(),
                                    BSON("x" << BSON_ARRAY(BSON("$ref" << "collx"
                                                                       << "$id" << oidx << "$db"
                                                                       << "db")
                                                           << BSON("$ref" << "coll"
                                                                          << "$id" << oid << "$db"
                                                                          << "dbx")))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON("$ref" << "coll"
                                                              << "$id" << oid << "$db"
                                                              << "db"))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON("$ref" << "colly"
                                                              << "$id" << oidy << "$db"
                                                              << "db"))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSON("$ref" << "coll"
                                                                         << "$id" << oid << "$db"
                                                                         << "db")))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSON("$ref" << "colly"
                                                                         << "$id" << oidy << "$db"
                                                                         << "db")))));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x" << BSON_ARRAY(BSON("$ref" << "collx"
                                                                         << "$id" << oidx << "$db"
                                                                         << "db")
                                                             << BSON("$ref" << "coll"
                                                                            << "$id" << oid << "$db"
                                                                            << "db")))));
    ASSERT(
        exec::matcher::matchesBSON(result.getValue().get(),
                                   BSON("x" << BSON_ARRAY(BSON("$ref" << "collx"
                                                                      << "$id" << oidx << "$db"
                                                                      << "db")
                                                          << BSON("$ref" << "colly"
                                                                         << "$id" << oidy << "$db"
                                                                         << "db")))));
}

TEST(MatchExpressionParserLeafTest, INDBRefWithOptionalField1) {
    OID oid = OID::gen();
    BSONObj query =
        BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref" << "coll"
                                                         << "$id" << oid << "foo" << 12345))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    OID oidx = OID::gen();
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x" << BSON("$ref" << "coll"
                                                               << "$id" << oidx << "$db"
                                                               << "db"))));
    ASSERT(exec::matcher::matchesBSON(
        result.getValue().get(),
        BSON("x" << BSON_ARRAY(BSON("$ref" << "coll"
                                           << "$id" << oid << "foo" << 12345)))));
    ASSERT(exec::matcher::matchesBSON(
        result.getValue().get(),
        BSON("x" << BSON_ARRAY(BSON("$ref" << "collx"
                                           << "$id" << oidx << "foo" << 12345)
                               << BSON("$ref" << "coll"
                                              << "$id" << oid << "foo" << 12345)))));
}

TEST(MatchExpressionParserLeafTest, INRegexStuff) {
    BSONObjBuilder inArray;
    inArray.appendRegex("0", "^a", "");
    inArray.appendRegex("1", "B", "i");
    inArray.append("2", 4);
    BSONObjBuilder operand;
    operand.appendArray("$in", inArray.obj());

    BSONObj query = BSON("a" << operand.obj());
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    BSONObj matchFirst = BSON("a" << "ax");
    BSONObj matchFirstRegex = BSONObjBuilder().appendRegex("a", "^a", "").obj();
    BSONObj matchSecond = BSON("a" << "qqb");
    BSONObj matchSecondRegex = BSONObjBuilder().appendRegex("a", "B", "i").obj();
    BSONObj matchThird = BSON("a" << 4);
    BSONObj notMatch = BSON("a" << "l");
    BSONObj notMatchRegex = BSONObjBuilder().appendRegex("a", "B", "").obj();

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), matchFirst));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), matchFirstRegex));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), matchSecond));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), matchSecondRegex));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), matchThird));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), notMatch));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), notMatchRegex));
}

TEST(MatchExpressionParserLeafTest, SimpleNIN1) {
    BSONObj query = BSON("x" << BSON("$nin" << BSON_ARRAY(2 << 3)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, Regex1) {
    BSONObjBuilder b;
    b.appendRegex("x", "abc", "i");
    BSONObj query = b.obj();
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ABC")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "AC")));
}

TEST(MatchExpressionParserLeafTest, Regex2) {
    BSONObj query = BSON("x" << BSON("$regex" << "abc"
                                              << "$options"
                                              << "i"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ABC")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "AC")));
}

TEST(MatchExpressionParserLeafTest, Regex3) {
    BSONObj query = BSON("x" << BSON("$options" << "i"
                                                << "$regex"
                                                << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "ABC")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "AC")));
}

TEST(MatchExpressionParserLeafTest, RegexEmbeddedNULByte) {
    BSONObj query = BSON("x" << BSON("$regex" << "^a\\x00b"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    const auto value = "a\0b"_sd;
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << value)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "a")));
}

TEST(MatchExpressionParserLeafTest, ExistsYes1) {
    BSONObjBuilder b;
    b.appendBool("$exists", true);
    BSONObj query = BSON("x" << b.obj());
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("y" << "AC")));
}

TEST(MatchExpressionParserLeafTest, ExistsNO1) {
    BSONObjBuilder b;
    b.appendBool("$exists", false);
    BSONObj query = BSON("x" << b.obj());
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("y" << "AC")));
}

TEST(MatchExpressionParserLeafTest, Type1) {
    BSONObj query = BSON("x" << BSON("$type" << BSONType::string));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << "abc")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5)));
}

TEST(MatchExpressionParserLeafTest, Type2) {
    BSONObj query = BSON(
        "x" << BSON("$type" << static_cast<double>(stdx::to_underlying(BSONType::numberDouble))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5.3)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5)));
}

TEST(MatchExpressionParserLeafTest, TypeDecimalOperator) {
    BSONObj query = BSON("x" << BSON("$type" << BSONType::numberDecimal));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT_FALSE(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5.3)));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << mongo::Decimal128("1"))));
}

TEST(MatchExpressionParserLeafTest, TypeNull) {
    BSONObj query = BSON("x" << BSON("$type" << BSONType::null));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSONObj()));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5)));
    BSONObjBuilder b;
    b.appendNull("x");
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), b.obj()));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameDouble) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumberDouble =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'double'}}"), expCtx);
    ASSERT_OK(typeNumberDouble.getStatus());
    TypeMatchExpression* tmeNumberDouble =
        static_cast<TypeMatchExpression*>(typeNumberDouble.getValue().get());
    ASSERT_FALSE(tmeNumberDouble->typeSet().allNumbers);
    ASSERT_EQ(tmeNumberDouble->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeNumberDouble->typeSet().hasType(BSONType::numberDouble));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeNumberDouble, fromjson("{a: 5.4}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmeNumberDouble, fromjson("{a: NumberInt(5)}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringNameNumberDecimal) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumberDecimal =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'decimal'}}"), expCtx);
    ASSERT_OK(typeNumberDecimal.getStatus());
    TypeMatchExpression* tmeNumberDecimal =
        static_cast<TypeMatchExpression*>(typeNumberDecimal.getValue().get());
    ASSERT_FALSE(tmeNumberDecimal->typeSet().allNumbers);
    ASSERT_EQ(tmeNumberDecimal->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeNumberDecimal->typeSet().hasType(BSONType::numberDecimal));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeNumberDecimal, BSON("a" << mongo::Decimal128("1"))));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmeNumberDecimal, fromjson("{a: true}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameNumberInt) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumberInt =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'int'}}"), expCtx);
    ASSERT_OK(typeNumberInt.getStatus());
    TypeMatchExpression* tmeNumberInt =
        static_cast<TypeMatchExpression*>(typeNumberInt.getValue().get());
    ASSERT_FALSE(tmeNumberInt->typeSet().allNumbers);
    ASSERT_EQ(tmeNumberInt->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeNumberInt->typeSet().hasType(BSONType::numberInt));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeNumberInt, fromjson("{a: NumberInt(5)}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmeNumberInt, fromjson("{a: 5.4}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameNumberLong) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumberLong =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'long'}}"), expCtx);
    ASSERT_OK(typeNumberLong.getStatus());
    TypeMatchExpression* tmeNumberLong =
        static_cast<TypeMatchExpression*>(typeNumberLong.getValue().get());
    ASSERT_FALSE(tmeNumberLong->typeSet().allNumbers);
    ASSERT_EQ(tmeNumberLong->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeNumberLong->typeSet().hasType(BSONType::numberLong));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeNumberLong, BSON("a" << -1LL)));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmeNumberLong, fromjson("{a: true}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameString) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeString =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'string'}}"), expCtx);
    ASSERT_OK(typeString.getStatus());
    TypeMatchExpression* tmeString = static_cast<TypeMatchExpression*>(typeString.getValue().get());
    ASSERT_FALSE(tmeString->typeSet().allNumbers);
    ASSERT_EQ(tmeString->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeString->typeSet().hasType(BSONType::string));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeString, fromjson("{a: 'hello world'}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmeString, fromjson("{a: 5.4}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnamejstOID) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typejstOID =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'objectId'}}"), expCtx);
    ASSERT_OK(typejstOID.getStatus());
    TypeMatchExpression* tmejstOID = static_cast<TypeMatchExpression*>(typejstOID.getValue().get());
    ASSERT_FALSE(tmejstOID->typeSet().allNumbers);
    ASSERT_EQ(tmejstOID->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmejstOID->typeSet().hasType(BSONType::oid));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmejstOID,
                                           fromjson("{a: ObjectId('000000000000000000000000')}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmejstOID, fromjson("{a: 'hello world'}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnamejstNULL) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typejstNULL =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'null'}}"), expCtx);
    ASSERT_OK(typejstNULL.getStatus());
    TypeMatchExpression* tmejstNULL =
        static_cast<TypeMatchExpression*>(typejstNULL.getValue().get());
    ASSERT_FALSE(tmejstNULL->typeSet().allNumbers);
    ASSERT_EQ(tmejstNULL->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmejstNULL->typeSet().hasType(BSONType::null));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmejstNULL, fromjson("{a: null}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmejstNULL, fromjson("{a: true}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameBool) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeBool =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'bool'}}"), expCtx);
    ASSERT_OK(typeBool.getStatus());
    TypeMatchExpression* tmeBool = static_cast<TypeMatchExpression*>(typeBool.getValue().get());
    ASSERT_FALSE(tmeBool->typeSet().allNumbers);
    ASSERT_EQ(tmeBool->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeBool->typeSet().hasType(BSONType::boolean));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeBool, fromjson("{a: true}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmeBool, fromjson("{a: null}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameObject) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeObject =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'object'}}"), expCtx);
    ASSERT_OK(typeObject.getStatus());
    TypeMatchExpression* tmeObject = static_cast<TypeMatchExpression*>(typeObject.getValue().get());
    ASSERT_FALSE(tmeObject->typeSet().allNumbers);
    ASSERT_EQ(tmeObject->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeObject->typeSet().hasType(BSONType::object));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeObject, fromjson("{a: {}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmeObject, fromjson("{a: []}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameArray) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeArray =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'array'}}"), expCtx);
    ASSERT_OK(typeArray.getStatus());
    TypeMatchExpression* tmeArray = static_cast<TypeMatchExpression*>(typeArray.getValue().get());
    ASSERT_FALSE(tmeArray->typeSet().allNumbers);
    ASSERT_EQ(tmeArray->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeArray->typeSet().hasType(BSONType::array));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeArray, fromjson("{a: [[]]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmeArray, fromjson("{a: {}}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameNumber) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumber =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'number'}}"), expCtx);
    ASSERT_OK(typeNumber.getStatus());
    TypeMatchExpression* tmeNumber = static_cast<TypeMatchExpression*>(typeNumber.getValue().get());
    ASSERT_TRUE(tmeNumber->typeSet().allNumbers);
    ASSERT_EQ(tmeNumber->typeSet().bsonTypes.size(), 0u);
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeNumber, fromjson("{a: 5.4}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeNumber, fromjson("{a: NumberInt(5)}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(tmeNumber, BSON("a" << -1LL)));
    ASSERT_FALSE(exec::matcher::matchesBSON(tmeNumber, fromjson("{a: ''}")));
}

TEST(MatchExpressionParserLeafTest, SimpleNE1) {
    BSONObj query = BSON("x" << BSON("$ne" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, SimpleMod1) {
    BSONObj query = BSON("x" << BSON("$mod" << BSON_ARRAY(3 << 2)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 4)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 8)));
}

TEST(MatchExpressionParserLeafTest, SimpleIN1) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(2 << 3)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 1)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
}

TEST(MatchExpressionParserTest, SimpleEQ1) {
    BSONObj query = BSON("x" << 2);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 2)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 3)));
}

TEST(MatchExpressionParserTest, Multiple1) {
    BSONObj query = BSON("x" << 5 << "y" << BSON("$gt" << 5 << "$lt" << 8));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5 << "y" << 7)));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5 << "y" << 6)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 6 << "y" << 7)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5 << "y" << 9)));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(), BSON("x" << 5 << "y" << 4)));
}

TEST(MatchExpressionParserTest, AlwaysFalseParsesIntegerArgument) {
    auto query = BSON(AlwaysFalseMatchExpression::kName << 1);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: 1}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: 'blah'}")));
}

TEST(MatchExpressionParserTest, AlwaysTrueParsesIntegerArgument) {
    auto query = BSON(AlwaysTrueMatchExpression::kName << 1);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(expr.getStatus());

    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: 1}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), fromjson("{x: 'blah'}")));
}

TEST(MatchExpressionParserTest, InternalExprEqParsesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());

    auto query = fromjson("{a: {$_internalExprEq: 'foo'}}");
    auto statusWith = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWith.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(statusWith.getValue().get(), fromjson("{a: 'foo'}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(statusWith.getValue().get(), fromjson("{a: ['foo']}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(statusWith.getValue().get(), fromjson("{a: ['bar']}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(statusWith.getValue().get(), fromjson("{a: 'bar'}")));

    query = fromjson("{'a.b': {$_internalExprEq: 5}}");
    statusWith = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(statusWith.getStatus());
    ASSERT_TRUE(exec::matcher::matchesBSON(statusWith.getValue().get(), fromjson("{a: {b: 5}}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(statusWith.getValue().get(), fromjson("{a: {b: [5]}}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(statusWith.getValue().get(), fromjson("{a: {b: [6]}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(statusWith.getValue().get(), fromjson("{a: {b: 6}}")));
}

TEST(MatchExpressionParserTest, SampleRateMatchingBehaviorStats) {
    // Test that the average number of sampled docs is within 10 standard deviations using the
    // binomial distribution over k runs, 10 * sqrt(N * p * (1 - p) / k).
    constexpr double p = 0.5;
    constexpr int k = 1000;
    constexpr int N = 3000;  // Simulated collection size.

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression expr = MatchExpressionParser::parse(BSON("$sampleRate" << p), expCtx);

    // Average the number of docs sampled over k iterations.
    int sum = 0;
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < N; j++) {
            if (exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 1))) {
                sum++;
            }
        }
    }
    const double avg = static_cast<double>(sum) / k;

    const double mu = p * N;
    const double err = 10.0 * std::sqrt(mu * (1 - p) / k);
    ASSERT_TRUE(mu - err <= avg && mu + err >= avg);

    // Test that $sampleRate args 0.0 and 1.0 return 0 and all hits, respectively.
    expr = MatchExpressionParser::parse(BSON("$sampleRate" << 0.0), expCtx);
    for (int j = 0; j < N; j++) {
        ASSERT_FALSE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 1)));
    }

    expr = MatchExpressionParser::parse(BSON("$sampleRate" << 1.0), expCtx);
    for (int j = 0; j < N; j++) {
        ASSERT_TRUE(exec::matcher::matchesBSON(expr.getValue().get(), BSON("a" << 1)));
    }
}

namespace {

BSONObj serialize(MatchExpression* match) {
    return match->serialize();
}

}  // namespace

TEST(SerializeBasic, ExpressionNotWithDirectPathExpSerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // At the time of this writing, the MatchExpression parser does not ever create a NOT with a
    // direct path expression child, instead creating a NOT -> AND -> path expression. This test
    // manually constructs such an expression in case it ever turns up, since that should still be
    // able to serialize.
    auto originalBSON = fromjson("{a: {$not: {$eq: 2}}}");
    auto equalityRHSElem = originalBSON["a"]["$not"]["$eq"];
    auto equalityExpression = std::make_unique<EqualityMatchExpression>("a"_sd, equalityRHSElem);

    auto notExpression = std::make_unique<NotMatchExpression>(equalityExpression.release());
    Matcher reserialized(serialize(notExpression.get()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), originalBSON);

    auto obj = fromjson("{a: 2}");
    ASSERT_EQ(exec::matcher::matchesBSON(notExpression.get(), obj),
              exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNotNotDirectlySerializesCorrectly) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // At the time of this writing, the MatchExpression parser does not ever create a NOT with a
    // direct NOT child, instead creating a NOT -> AND -> NOT. This test manually constructs such an
    // expression in case it ever turns up, since that should still be able to serialize to
    // {$not: {$not: ...}}.
    auto originalBSON = fromjson("{a: {$not: {$not: {$eq: 2}}}}");
    auto equalityRHSElem = originalBSON["a"]["$not"]["$not"]["$eq"];
    auto equalityExpression = std::make_unique<EqualityMatchExpression>("a"_sd, equalityRHSElem);

    auto nestedNot = std::make_unique<NotMatchExpression>(equalityExpression.release());
    auto topNot = std::make_unique<NotMatchExpression>(nestedNot.release());
    Matcher reserialized(serialize(topNot.get()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), fromjson("{$nor: [{a: {$not: {$eq: 2}}}]}"));

    auto obj = fromjson("{a: 2}");
    ASSERT_EQ(exec::matcher::matchesBSON(topNot.get(), obj),
              exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionNotWithoutPathChildrenSerializesCorrectly) {
    // The grammar only permits a $not under a given path. For example, {a: {$not: {$eq: 4}}} is OK
    // but {$not: {a: 4}} is not OK). However, we sometimes use the NOT MatchExpression to negate
    // clauses within a JSONSchema. In such circumstances we need to be able to serialize the tree
    // and re-parse it but the parser will reject the NOT in the place it's in. As a result, we need
    // to translate the NOT to a $nor.

    // MatchExpression tree expected:
    //  {$or: [
    //    {$and: [
    //      {foo: {$_internalSchemaType: [2]}},
    //      {foo: {$not: {
    //        // This whole $or represents the {maxLength: 4}, since the restriction only applies if
    //        // the element is the right type.
    //        $or: [
    //          {$_internalSchemaMaxLength: 4},
    //          {foo: {$not: {$_internalSchemaType: [2]}}}
    //        ]
    //      }}}
    //    ]},
    //    {foo: {$not: {$exists: true}}}
    //  ]}
    BSONObj query =
        fromjson("{$jsonSchema: {properties: {foo: {type: 'string', not: {maxLength: 4}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expression = unittest::assertGet(MatchExpressionParser::parse(query, expCtx));

    Matcher reserialized(serialize(expression.get()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$and: ["
                               "  {$and: ["
                               "    {$or: ["
                               "      {foo: {$not: {$exists: true}}},"
                               "      {$and: ["
                               "        {$nor: ["  // <-- This is the interesting part of this test.
                               "          {$or: ["
                               "            {foo: {$not: {$_internalSchemaType: [2]}}},"
                               "            {foo: {$_internalSchemaMaxLength: 4}}"
                               "          ]}"
                               "        ]},"
                               "        {foo: {$_internalSchemaType: [2]}}"
                               "      ]}"
                               "    ]}"
                               "  ]}"
                               "]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{foo: 'abc'}");
    ASSERT_EQ(exec::matcher::matchesBSON(expression.get(), obj),
              exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{foo: 'acbdf'}");
    ASSERT_EQ(exec::matcher::matchesBSON(expression.get(), obj),
              exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionJsonSchemaWithDollarFieldSerializesCorrectly) {
    BSONObj query = fromjson("{$jsonSchema: {properties: {$stdDevPop: {type: 'array'}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expression = unittest::assertGet(MatchExpressionParser::parse(query, expCtx));

    Matcher reserialized(serialize(expression.get()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$and: ["
                               "  {$and: ["
                               "    {$or: ["
                               "      {$nor: [{$_internalPath:{$stdDevPop: {$exists: true}}}]},"
                               "      {$and: ["
                               "        {$_internalPath:"
                               "          {$stdDevPop: {$_internalSchemaType: [4]}}"
                               "        }"
                               "      ]}"
                               "    ]}"
                               "  ]}"
                               "]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{$stdDevPop: [1, 2, 3]}");
    ASSERT_EQ(exec::matcher::matchesBSON(expression.get(), obj),
              exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{$stdDevPop: []}");
    ASSERT_EQ(exec::matcher::matchesBSON(expression.get(), obj),
              exec::matcher::matches(&reserialized, obj));
}

TEST(SerializeBasic, ExpressionJsonSchemaWithKeywordDollarFieldSerializesCorrectly) {
    BSONObj query = fromjson("{$jsonSchema: {properties: {$or: {type: 'string'}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expression = unittest::assertGet(MatchExpressionParser::parse(query, expCtx));

    Matcher reserialized(serialize(expression.get()),
                         expCtx,
                         ExtensionsCallbackNoop(),
                         MatchExpressionParser::kAllowAllSpecialFeatures);

    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(),
                      fromjson("{$and: ["
                               "  {$and: ["
                               "    {$or: ["
                               "      {$nor: [{$_internalPath:{$or: {$exists: true}}}]},"
                               "      {$and: ["
                               "        {$_internalPath: {$or: {$_internalSchemaType: [2]}}}"
                               "      ]}"
                               "    ]}"
                               "  ]}"
                               "]}"));
    ASSERT_BSONOBJ_EQ(*reserialized.getQuery(), serialize(reserialized.getMatchExpression()));

    BSONObj obj = fromjson("{$or: 'abcd'}");
    ASSERT_EQ(exec::matcher::matchesBSON(expression.get(), obj),
              exec::matcher::matches(&reserialized, obj));

    obj = fromjson("{$or: ''}");
    ASSERT_EQ(exec::matcher::matchesBSON(expression.get(), obj),
              exec::matcher::matches(&reserialized, obj));
}

TEST(ExpressionWithPlaceholderTest, ParseBasic) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{i: 0}");
    auto parsedFilter = unittest::assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = unittest::assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{i: 0}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{i: 1}")));
}

TEST(ExpressionWithPlaceholderTest, ParseDottedField) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{'i.a': 0, 'i.b': 1}");
    auto parsedFilter = unittest::assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = unittest::assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{i: {a: 0, b: 1}}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{i: {a: 0, b: 0}}")));
}

TEST(ExpressionWithPlaceholderTest, ParseLogicalQuery) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{$and: [{i: {$gte: 0}}, {i: {$lte: 0}}]}");
    auto parsedFilter = unittest::assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = unittest::assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{i: 0}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{i: 1}")));
}

TEST(ExpressionWithPlaceholderTest, ParseElemMatch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{i: {$elemMatch: {a: 0}}}");
    auto parsedFilter = unittest::assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = unittest::assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{i: [{a: 0}]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{i: [{a: 1}]}")));
}

TEST(ExpressionWithPlaceholderTest, ParseCollation) {
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    auto rawFilter = fromjson("{i: 'abc'}");
    auto parsedFilter = unittest::assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = unittest::assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "i");
    ASSERT_TRUE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{i: 'cba'}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{i: 0}")));
}

TEST(ExpressionWithPlaceholderTest, ParseIdContainsNumbersAndCapitals) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto rawFilter = fromjson("{iA3: 0}");
    auto parsedFilter = unittest::assertGet(MatchExpressionParser::parse(rawFilter, expCtx));
    auto filter = unittest::assertGet(ExpressionWithPlaceholder::make(std::move(parsedFilter)));
    ASSERT(filter->getPlaceholder());
    ASSERT_EQ(*filter->getPlaceholder(), "iA3");
    ASSERT_TRUE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{'iA3': 0}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(filter->getFilter(), fromjson("{'iA3': 1}")));
}

}  // namespace mongo::evaluate_matcher_test
