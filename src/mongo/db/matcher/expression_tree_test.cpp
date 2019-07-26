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

/** Unit tests for MatchMatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/unittest/unittest.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/collation/collator_interface_mock.h"

namespace mongo {

using std::unique_ptr;

TEST(NotMatchExpression, MatchesScalar) {
    BSONObj baseOperand = BSON("$lt" << 5);
    unique_ptr<ComparisonMatchExpression> lt(new LTMatchExpression("a", baseOperand["$lt"]));
    NotMatchExpression notOp(lt.release());
    ASSERT(notOp.matchesBSON(BSON("a" << 6), nullptr));
    ASSERT(!notOp.matchesBSON(BSON("a" << 4), nullptr));
}

TEST(NotMatchExpression, MatchesArray) {
    BSONObj baseOperand = BSON("$lt" << 5);
    unique_ptr<ComparisonMatchExpression> lt(new LTMatchExpression("a", baseOperand["$lt"]));
    NotMatchExpression notOp(lt.release());
    ASSERT(notOp.matchesBSON(BSON("a" << BSON_ARRAY(6)), nullptr));
    ASSERT(!notOp.matchesBSON(BSON("a" << BSON_ARRAY(4)), nullptr));
    // All array elements must match.
    ASSERT(!notOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5 << 6)), nullptr));
}

TEST(NotMatchExpression, ElemMatchKey) {
    BSONObj baseOperand = BSON("$lt" << 5);
    unique_ptr<ComparisonMatchExpression> lt(new LTMatchExpression("a", baseOperand["$lt"]));
    NotMatchExpression notOp(lt.release());
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!notOp.matchesBSON(BSON("a" << BSON_ARRAY(1)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(notOp.matchesBSON(BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(notOp.matchesBSON(BSON("a" << BSON_ARRAY(6)), &details));
    // elemMatchKey is not implemented for negative match operators.
    ASSERT(!details.hasElemMatchKey());
}

TEST(NotMatchExpression, SetCollatorPropagatesToChild) {
    BSONObj baseOperand = BSON("a"
                               << "string");
    unique_ptr<ComparisonMatchExpression> eq(new EqualityMatchExpression("a", baseOperand["a"]));
    NotMatchExpression notOp(eq.release());
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    notOp.setCollator(&collator);
    ASSERT(!notOp.matchesBSON(BSON("a"
                                   << "string2"),
                              nullptr));
}

TEST(AndOp, NoClauses) {
    AndMatchExpression andMatchExpression;
    ASSERT(andMatchExpression.matchesBSON(BSONObj(), nullptr));
}

TEST(AndOp, MatchesElementThreeClauses) {
    BSONObj baseOperand1 = BSON("$lt"
                                << "z1");
    BSONObj baseOperand2 = BSON("$gt"
                                << "a1");
    BSONObj match = BSON("a"
                         << "r1");
    BSONObj notMatch1 = BSON("a"
                             << "z1");
    BSONObj notMatch2 = BSON("a"
                             << "a1");
    BSONObj notMatch3 = BSON("a"
                             << "r");

    unique_ptr<ComparisonMatchExpression> sub1(new LTMatchExpression("a", baseOperand1["$lt"]));
    unique_ptr<ComparisonMatchExpression> sub2(new GTMatchExpression("a", baseOperand2["$gt"]));
    unique_ptr<RegexMatchExpression> sub3(new RegexMatchExpression("a", "1", ""));

    AndMatchExpression andOp;
    andOp.add(sub1.release());
    andOp.add(sub2.release());
    andOp.add(sub3.release());

    ASSERT(andOp.matchesBSON(match));
    ASSERT(!andOp.matchesBSON(notMatch1));
    ASSERT(!andOp.matchesBSON(notMatch2));
    ASSERT(!andOp.matchesBSON(notMatch3));
}

TEST(AndOp, MatchesSingleClause) {
    BSONObj baseOperand = BSON("$ne" << 5);
    unique_ptr<ComparisonMatchExpression> eq(new EqualityMatchExpression("a", baseOperand["$ne"]));
    unique_ptr<NotMatchExpression> ne(new NotMatchExpression(eq.release()));

    AndMatchExpression andOp;
    andOp.add(ne.release());

    ASSERT(andOp.matchesBSON(BSON("a" << 4), nullptr));
    ASSERT(andOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5)), nullptr));
}

TEST(AndOp, MatchesThreeClauses) {
    BSONObj baseOperand1 = BSON("$gt" << 1);
    BSONObj baseOperand2 = BSON("$lt" << 10);
    BSONObj baseOperand3 = BSON("$lt" << 100);

    unique_ptr<ComparisonMatchExpression> sub1(new GTMatchExpression("a", baseOperand1["$gt"]));
    unique_ptr<ComparisonMatchExpression> sub2(new LTMatchExpression("a", baseOperand2["$lt"]));
    unique_ptr<ComparisonMatchExpression> sub3(new LTMatchExpression("b", baseOperand3["$lt"]));

    AndMatchExpression andOp;
    andOp.add(sub1.release());
    andOp.add(sub2.release());
    andOp.add(sub3.release());

    ASSERT(andOp.matchesBSON(BSON("a" << 5 << "b" << 6), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("b" << 6), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("a" << 1 << "b" << 6), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("a" << 10 << "b" << 6), nullptr));
}

TEST(AndOp, ElemMatchKey) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);

    unique_ptr<ComparisonMatchExpression> sub1(new EqualityMatchExpression("a", baseOperand1["a"]));
    unique_ptr<ComparisonMatchExpression> sub2(new EqualityMatchExpression("b", baseOperand2["b"]));

    AndMatchExpression andOp;
    andOp.add(sub1.release());
    andOp.add(sub2.release());

    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!andOp.matchesBSON(BSON("a" << BSON_ARRAY(1)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!andOp.matchesBSON(BSON("b" << BSON_ARRAY(2)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(andOp.matchesBSON(BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(1 << 2)), &details));
    ASSERT(details.hasElemMatchKey());
    // The elem match key for the second $and clause is recorded.
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(OrOp, NoClauses) {
    OrMatchExpression orOp;
    ASSERT(!orOp.matchesBSON(BSONObj(), nullptr));
}

TEST(OrOp, MatchesSingleClause) {
    BSONObj baseOperand = BSON("$ne" << 5);
    unique_ptr<ComparisonMatchExpression> eq(new EqualityMatchExpression("a", baseOperand["$ne"]));
    unique_ptr<NotMatchExpression> ne(new NotMatchExpression(eq.release()));

    OrMatchExpression orOp;
    orOp.add(ne.release());

    ASSERT(orOp.matchesBSON(BSON("a" << 4), nullptr));
    ASSERT(orOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(!orOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(!orOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5)), nullptr));
}

TEST(OrOp, MatchesThreeClauses) {
    BSONObj baseOperand1 = BSON("$gt" << 10);
    BSONObj baseOperand2 = BSON("$lt" << 0);
    BSONObj baseOperand3 = BSON("b" << 100);
    unique_ptr<ComparisonMatchExpression> sub1(new GTMatchExpression("a", baseOperand1["$gt"]));
    unique_ptr<ComparisonMatchExpression> sub2(new LTMatchExpression("a", baseOperand2["$lt"]));
    unique_ptr<ComparisonMatchExpression> sub3(new EqualityMatchExpression("b", baseOperand3["b"]));

    OrMatchExpression orOp;
    orOp.add(sub1.release());
    orOp.add(sub2.release());
    orOp.add(sub3.release());

    ASSERT(orOp.matchesBSON(BSON("a" << -1), nullptr));
    ASSERT(orOp.matchesBSON(BSON("a" << 11), nullptr));
    ASSERT(!orOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(orOp.matchesBSON(BSON("b" << 100), nullptr));
    ASSERT(!orOp.matchesBSON(BSON("b" << 101), nullptr));
    ASSERT(!orOp.matchesBSON(BSONObj(), nullptr));
    ASSERT(orOp.matchesBSON(BSON("a" << 11 << "b" << 100), nullptr));
}

TEST(OrOp, ElemMatchKey) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);
    unique_ptr<ComparisonMatchExpression> sub1(new EqualityMatchExpression("a", baseOperand1["a"]));
    unique_ptr<ComparisonMatchExpression> sub2(new EqualityMatchExpression("b", baseOperand2["b"]));

    OrMatchExpression orOp;
    orOp.add(sub1.release());
    orOp.add(sub2.release());

    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!orOp.matchesBSON(BSONObj(), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!orOp.matchesBSON(BSON("a" << BSON_ARRAY(10) << "b" << BSON_ARRAY(10)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(orOp.matchesBSON(BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(1 << 2)), &details));
    // The elem match key feature is not implemented for $or.
    ASSERT(!details.hasElemMatchKey());
}

TEST(NorOp, NoClauses) {
    NorMatchExpression norOp;
    ASSERT(norOp.matchesBSON(BSONObj(), nullptr));
}

TEST(NorOp, MatchesSingleClause) {
    BSONObj baseOperand = BSON("$ne" << 5);
    unique_ptr<ComparisonMatchExpression> eq(new EqualityMatchExpression("a", baseOperand["$ne"]));
    unique_ptr<NotMatchExpression> ne(new NotMatchExpression(eq.release()));

    NorMatchExpression norOp;
    norOp.add(ne.release());

    ASSERT(!norOp.matchesBSON(BSON("a" << 4), nullptr));
    ASSERT(!norOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(norOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(norOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5)), nullptr));
}

TEST(NorOp, MatchesThreeClauses) {
    BSONObj baseOperand1 = BSON("$gt" << 10);
    BSONObj baseOperand2 = BSON("$lt" << 0);
    BSONObj baseOperand3 = BSON("b" << 100);

    unique_ptr<ComparisonMatchExpression> sub1(new GTMatchExpression("a", baseOperand1["$gt"]));
    unique_ptr<ComparisonMatchExpression> sub2(new LTMatchExpression("a", baseOperand2["$lt"]));
    unique_ptr<ComparisonMatchExpression> sub3(new EqualityMatchExpression("b", baseOperand3["b"]));

    NorMatchExpression norOp;
    norOp.add(sub1.release());
    norOp.add(sub2.release());
    norOp.add(sub3.release());

    ASSERT(!norOp.matchesBSON(BSON("a" << -1), nullptr));
    ASSERT(!norOp.matchesBSON(BSON("a" << 11), nullptr));
    ASSERT(norOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(!norOp.matchesBSON(BSON("b" << 100), nullptr));
    ASSERT(norOp.matchesBSON(BSON("b" << 101), nullptr));
    ASSERT(norOp.matchesBSON(BSONObj(), nullptr));
    ASSERT(!norOp.matchesBSON(BSON("a" << 11 << "b" << 100), nullptr));
}

TEST(NorOp, ElemMatchKey) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);
    unique_ptr<ComparisonMatchExpression> sub1(new EqualityMatchExpression("a", baseOperand1["a"]));
    unique_ptr<ComparisonMatchExpression> sub2(new EqualityMatchExpression("b", baseOperand2["b"]));

    NorMatchExpression norOp;
    norOp.add(sub1.release());
    norOp.add(sub2.release());

    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!norOp.matchesBSON(BSON("a" << 1), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!norOp.matchesBSON(BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(10)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(norOp.matchesBSON(BSON("a" << BSON_ARRAY(3) << "b" << BSON_ARRAY(4)), &details));
    // The elem match key feature is not implemented for $nor.
    ASSERT(!details.hasElemMatchKey());
}


TEST(NorOp, Equivalent) {
    BSONObj baseOperand1 = BSON("a" << 1);
    BSONObj baseOperand2 = BSON("b" << 2);
    EqualityMatchExpression sub1("a", baseOperand1["a"]);
    EqualityMatchExpression sub2("b", baseOperand2["b"]);

    NorMatchExpression e1;
    e1.add(sub1.shallowClone().release());
    e1.add(sub2.shallowClone().release());

    NorMatchExpression e2;
    e2.add(sub1.shallowClone().release());

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
}
}  // namespace mongo
