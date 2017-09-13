/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/db/matcher/expression_type.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ExpressionTypeTest, MatchesElementStringType) {
    BSONObj match = BSON("a"
                         << "abc");
    BSONObj notMatch = BSON("a" << 5);
    TypeMatchExpression type;
    ASSERT(type.init("", String).isOK());
    ASSERT(type.matchesSingleElement(match["a"]));
    ASSERT(!type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionTypeTest, MatchesElementNullType) {
    BSONObj match = BSON("a" << BSONNULL);
    BSONObj notMatch = BSON("a"
                            << "abc");
    TypeMatchExpression type;
    ASSERT(type.init("", jstNULL).isOK());
    ASSERT(type.matchesSingleElement(match["a"]));
    ASSERT(!type.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionTypeTest, MatchesElementNumber) {
    BSONObj match1 = BSON("a" << 1);
    BSONObj match2 = BSON("a" << 1LL);
    BSONObj match3 = BSON("a" << 2.5);
    BSONObj notMatch = BSON("a"
                            << "abc");
    ASSERT_EQ(BSONType::NumberInt, match1["a"].type());
    ASSERT_EQ(BSONType::NumberLong, match2["a"].type());
    ASSERT_EQ(BSONType::NumberDouble, match3["a"].type());

    TypeMatchExpression typeExpr;
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    ASSERT_OK(typeExpr.init("a", std::move(typeSet)));
    ASSERT_EQ("a", typeExpr.path());
    ASSERT_TRUE(typeExpr.matchesSingleElement(match1["a"]));
    ASSERT_TRUE(typeExpr.matchesSingleElement(match2["a"]));
    ASSERT_TRUE(typeExpr.matchesSingleElement(match3["a"]));
    ASSERT_FALSE(typeExpr.matchesSingleElement(notMatch["a"]));
}

TEST(ExpressionTypeTest, MatchesScalar) {
    TypeMatchExpression type;
    ASSERT(type.init("a", Bool).isOK());
    ASSERT(type.matchesBSON(BSON("a" << true), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << 1), NULL));
}

TEST(ExpressionTypeTest, MatchesArray) {
    TypeMatchExpression type;
    ASSERT(type.init("a", NumberInt).isOK());
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(4)), NULL));
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(4 << "a")), NULL));
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY("a" << 4)), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON_ARRAY("a")), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), NULL));
}

TEST(ExpressionTypeTest, TypeArrayMatchesOuterAndInnerArray) {
    TypeMatchExpression type;
    ASSERT(type.init("a", Array).isOK());
    ASSERT(type.matchesBSON(BSON("a" << BSONArray()), nullptr));
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(4 << "a")), nullptr));
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(BSONArray() << 2)), nullptr));
    ASSERT(!type.matchesBSON(BSON("a"
                                  << "bar"),
                             nullptr));
}

TEST(ExpressionTypeTest, MatchesObject) {
    TypeMatchExpression type;
    ASSERT(type.init("a", Object).isOK());
    ASSERT(type.matchesBSON(BSON("a" << BSON("b" << 1)), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << 1), NULL));
}

TEST(ExpressionTypeTest, MatchesDotNotationFieldObject) {
    TypeMatchExpression type;
    ASSERT(type.init("a.b", Object).isOK());
    ASSERT(type.matchesBSON(BSON("a" << BSON("b" << BSON("c" << 1))), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON("b" << 1)), NULL));
}

TEST(ExpressionTypeTest, MatchesDotNotationArrayElementArray) {
    TypeMatchExpression type;
    ASSERT(type.init("a.0", Array).isOK());
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(BSON_ARRAY(1))), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON_ARRAY("b")), NULL));
}

TEST(ExpressionTypeTest, MatchesDotNotationArrayElementScalar) {
    TypeMatchExpression type;
    ASSERT(type.init("a.0", String).isOK());
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY("b")), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON_ARRAY(1)), NULL));
}

TEST(ExpressionTypeTest, MatchesDotNotationArrayElementObject) {
    TypeMatchExpression type;
    ASSERT(type.init("a.0", Object).isOK());
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 1))), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << BSON_ARRAY(1)), NULL));
}

TEST(ExpressionTypeTest, MatchesNull) {
    TypeMatchExpression type;
    ASSERT(type.init("a", jstNULL).isOK());
    ASSERT(type.matchesBSON(BSON("a" << BSONNULL), NULL));
    ASSERT(!type.matchesBSON(BSON("a" << 4), NULL));
    ASSERT(!type.matchesBSON(BSONObj(), NULL));
}

TEST(ExpressionTypeTest, ElemMatchKey) {
    TypeMatchExpression type;
    ASSERT(type.init("a.b", String).isOK());
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!type.matchesBSON(BSON("a" << 1), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(type.matchesBSON(BSON("a" << BSON("b"
                                             << "string")),
                            &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(type.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY("string"))), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("0", details.elemMatchKey());
    ASSERT(type.matchesBSON(BSON("a" << BSON_ARRAY(2 << BSON("b" << BSON_ARRAY("string")))),
                            &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(ExpressionTypeTest, Equivalent) {
    TypeMatchExpression e1;
    TypeMatchExpression e2;
    TypeMatchExpression e3;
    ASSERT_OK(e1.init("a", BSONType::String));
    ASSERT_OK(e2.init("a", BSONType::NumberDouble));
    ASSERT_OK(e3.init("b", BSONType::String));

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
    ASSERT(!e1.equivalent(&e3));
}

TEST(ExpressionTypeTest, InternalSchemaTypeArrayOnlyMatchesArrays) {
    InternalSchemaTypeExpression expr;
    ASSERT_OK(expr.init("a", BSONType::Array));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: []}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: [1]}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: 1}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: {b: []}}")));
}

TEST(ExpressionTypeTest, InternalSchemaTypeNumberDoesNotMatchArrays) {
    InternalSchemaTypeExpression expr;
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    ASSERT_OK(expr.init("a", std::move(typeSet)));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: []}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: [1]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: ['b', 2, 3]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: {b: []}}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: 1}")));
}

TEST(ExpressionTypeTest, TypeExprWithMultipleTypesMatchesAllSuchTypes) {
    TypeMatchExpression expr;
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    typeSet.bsonTypes.insert(BSONType::String);
    typeSet.bsonTypes.insert(BSONType::Object);
    ASSERT_OK(expr.init("a", std::move(typeSet)));

    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: []}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: 1}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: [1]}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: null}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: 'str'}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: ['str']}")));
}

TEST(ExpressionTypeTest, InternalSchemaTypeExprWithMultipleTypesMatchesAllSuchTypes) {
    InternalSchemaTypeExpression expr;
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    typeSet.bsonTypes.insert(BSONType::String);
    typeSet.bsonTypes.insert(BSONType::Object);
    ASSERT_OK(expr.init("a", std::move(typeSet)));

    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: []}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: 1}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: [1]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: null}")));
    ASSERT_TRUE(expr.matchesBSON(fromjson("{a: 'str'}")));
    ASSERT_FALSE(expr.matchesBSON(fromjson("{a: ['str']}")));
}

}  // namespace
}  // namepace mongo
