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

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

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

    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x"
                                            << "abc")));
    ASSERT(!exec::matcher::matchesBSON(result.getValue().get(),
                                       BSON("x"
                                            << "ABC")));
    ASSERT(exec::matcher::matchesBSON(result.getValue().get(),
                                      BSON("x"
                                           << "AC")));
}
}  // namespace mongo
