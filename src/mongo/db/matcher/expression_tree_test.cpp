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
#include "mongo/unittest/death_test.h"

namespace mongo {

TEST(NotMatchExpression, MatchesScalar) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a", baseOperand["$lt"]);
    auto notOp = NotMatchExpression{lt.release()};
    ASSERT(notOp.matchesBSON(BSON("a" << 6), nullptr));
    ASSERT(!notOp.matchesBSON(BSON("a" << 4), nullptr));
}

TEST(NotMatchExpression, MatchesArray) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a", baseOperand["$lt"]);
    auto notOp = NotMatchExpression{lt.release()};
    ASSERT(notOp.matchesBSON(BSON("a" << BSON_ARRAY(6)), nullptr));
    ASSERT(!notOp.matchesBSON(BSON("a" << BSON_ARRAY(4)), nullptr));
    // All array elements must match.
    ASSERT(!notOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5 << 6)), nullptr));
}

TEST(NotMatchExpression, ElemMatchKey) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a", baseOperand["$lt"]);
    auto notOp = NotMatchExpression{lt.release()};
    auto details = MatchDetails{};
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
    auto baseOperand = BSON("a"
                            << "string");
    auto eq = std::make_unique<EqualityMatchExpression>("a", baseOperand["a"]);
    auto notOp = NotMatchExpression{eq.release()};
    auto collator = CollatorInterfaceMock{CollatorInterfaceMock::MockType::kAlwaysEqual};
    notOp.setCollator(&collator);
    ASSERT(!notOp.matchesBSON(BSON("a"
                                   << "string2"),
                              nullptr));
}

DEATH_TEST_REGEX(NotMatchExpression,
                 GetChildFailsIndexLargerThanOne,
                 "Tripwire assertion.*6400210") {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a", baseOperand["$lt"]);
    auto notOp = NotMatchExpression{lt.release()};

    ASSERT_EQ(notOp.numChildren(), 1);
    ASSERT_THROWS_CODE(notOp.getChild(1), AssertionException, 6400210);
}

TEST(AndOp, NoClauses) {
    auto andMatchExpression = AndMatchExpression{};
    ASSERT(andMatchExpression.matchesBSON(BSONObj{}, nullptr));
}

TEST(AndOp, MatchesElementThreeClauses) {
    auto baseOperand1 = BSON("$lt"
                             << "z1");
    auto baseOperand2 = BSON("$gt"
                             << "a1");
    auto match = BSON("a"
                      << "r1");
    auto notMatch1 = BSON("a"
                          << "z1");
    auto notMatch2 = BSON("a"
                          << "a1");
    auto notMatch3 = BSON("a"
                          << "r");

    auto sub1 = std::make_unique<LTMatchExpression>("a", baseOperand1["$lt"]);
    auto sub2 = std::make_unique<GTMatchExpression>("a", baseOperand2["$gt"]);
    auto sub3 = std::make_unique<RegexMatchExpression>("a", "1", "");

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(sub1));
    andOp.add(std::move(sub2));
    andOp.add(std::move(sub3));

    ASSERT(andOp.matchesBSON(match));
    ASSERT(!andOp.matchesBSON(notMatch1));
    ASSERT(!andOp.matchesBSON(notMatch2));
    ASSERT(!andOp.matchesBSON(notMatch3));
}

TEST(AndOp, MatchesSingleClause) {
    auto baseOperand = BSON("$ne" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("a", baseOperand["$ne"]);
    auto ne = std::make_unique<NotMatchExpression>(eq.release());

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(ne));

    ASSERT(andOp.matchesBSON(BSON("a" << 4), nullptr));
    ASSERT(andOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5)), nullptr));
}

TEST(AndOp, MatchesThreeClauses) {
    auto baseOperand1 = BSON("$gt" << 1);
    auto baseOperand2 = BSON("$lt" << 10);
    auto baseOperand3 = BSON("$lt" << 100);

    auto sub1 = std::make_unique<GTMatchExpression>("a", baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a", baseOperand2["$lt"]);
    auto sub3 = std::make_unique<LTMatchExpression>("b", baseOperand3["$lt"]);

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(sub1));
    andOp.add(std::move(sub2));
    andOp.add(std::move(sub3));

    ASSERT(andOp.matchesBSON(BSON("a" << 5 << "b" << 6), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("b" << 6), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("a" << 1 << "b" << 6), nullptr));
    ASSERT(!andOp.matchesBSON(BSON("a" << 10 << "b" << 6), nullptr));
}

TEST(AndOp, ElemMatchKey) {
    auto baseOperand1 = BSON("a" << 1);
    auto baseOperand2 = BSON("b" << 2);

    auto sub1 = std::make_unique<EqualityMatchExpression>("a", baseOperand1["a"]);
    auto sub2 = std::make_unique<EqualityMatchExpression>("b", baseOperand2["b"]);

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(sub1));
    andOp.add(std::move(sub2));

    auto details = MatchDetails{};
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

DEATH_TEST_REGEX(AndOp, GetChildFailsOnIndexLargerThanChildren, "Tripwire assertion.*6400201") {
    auto baseOperand1 = BSON("$gt" << 1);
    auto baseOperand2 = BSON("$lt" << 10);
    auto baseOperand3 = BSON("$lt" << 100);

    auto sub1 = std::make_unique<GTMatchExpression>("a", baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a", baseOperand2["$lt"]);
    auto sub3 = std::make_unique<LTMatchExpression>("b", baseOperand3["$lt"]);

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(sub1));
    andOp.add(std::move(sub2));
    andOp.add(std::move(sub3));

    const size_t numChildren = 3;
    ASSERT_EQ(andOp.numChildren(), numChildren);
    ASSERT_THROWS_CODE(andOp.getChild(numChildren), AssertionException, 6400201);
}

TEST(OrOp, NoClauses) {
    auto orOp = OrMatchExpression{};
    ASSERT(!orOp.matchesBSON(BSONObj{}, nullptr));
}

TEST(OrOp, MatchesSingleClause) {
    auto baseOperand = BSON("$ne" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("a", baseOperand["$ne"]);
    auto ne = std::make_unique<NotMatchExpression>(eq.release());

    auto orOp = OrMatchExpression{};
    orOp.add(std::move(ne));

    ASSERT(orOp.matchesBSON(BSON("a" << 4), nullptr));
    ASSERT(orOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(!orOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(!orOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5)), nullptr));
}

TEST(OrOp, MatchesTwoClauses) {
    auto clauseObj1 = fromjson("{i: 5}");
    auto clauseObj2 = fromjson("{'i.a': 6}");
    auto clause1 = std::make_unique<EqualityMatchExpression>("i", clauseObj1["i"]);
    auto clause2 = std::make_unique<EqualityMatchExpression>("i.a", clauseObj2["i.a"]);

    auto filter = OrMatchExpression{};
    filter.add(std::move(clause1));
    filter.add(std::move(clause2));

    auto aClause1 = fromjson("{a: 5}");
    auto iClause1 = fromjson("{i: 5}");
    ASSERT_TRUE(filter.matchesBSONElement(aClause1["a"]));
    ASSERT_TRUE(filter.matchesBSON(iClause1));

    auto aClause2 = fromjson("{a: {a: 6}}");
    auto iClause2 = fromjson("{i: {a: 6}}");
    ASSERT_TRUE(filter.matchesBSONElement(aClause2["a"]));
    ASSERT_TRUE(filter.matchesBSON(iClause2));

    auto aNoMatch1 = fromjson("{a: 6}");
    auto iNoMatch1 = fromjson("{i: 6}");
    ASSERT_FALSE(filter.matchesBSONElement(aNoMatch1["a"]));
    ASSERT_FALSE(filter.matchesBSON(iNoMatch1));

    auto aNoMatch2 = fromjson("{a: {a: 5}}");
    auto iNoMatch2 = fromjson("{i: {a: 5}}");
    ASSERT_FALSE(filter.matchesBSONElement(aNoMatch2["a"]));
    ASSERT_FALSE(filter.matchesBSON(iNoMatch2));
}

TEST(OrOp, MatchesThreeClauses) {
    auto baseOperand1 = BSON("$gt" << 10);
    auto baseOperand2 = BSON("$lt" << 0);
    auto baseOperand3 = BSON("b" << 100);
    auto sub1 = std::make_unique<GTMatchExpression>("a", baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a", baseOperand2["$lt"]);
    auto sub3 = std::make_unique<EqualityMatchExpression>("b", baseOperand3["b"]);

    auto orOp = OrMatchExpression{};
    orOp.add(std::move(sub1));
    orOp.add(std::move(sub2));
    orOp.add(std::move(sub3));

    ASSERT(orOp.matchesBSON(BSON("a" << -1), nullptr));
    ASSERT(orOp.matchesBSON(BSON("a" << 11), nullptr));
    ASSERT(!orOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(orOp.matchesBSON(BSON("b" << 100), nullptr));
    ASSERT(!orOp.matchesBSON(BSON("b" << 101), nullptr));
    ASSERT(!orOp.matchesBSON(BSONObj(), nullptr));
    ASSERT(orOp.matchesBSON(BSON("a" << 11 << "b" << 100), nullptr));
}

TEST(OrOp, ElemMatchKey) {
    auto baseOperand1 = BSON("a" << 1);
    auto baseOperand2 = BSON("b" << 2);
    auto sub1 = std::make_unique<EqualityMatchExpression>("a", baseOperand1["a"]);
    auto sub2 = std::make_unique<EqualityMatchExpression>("b", baseOperand2["b"]);

    auto orOp = OrMatchExpression{};
    orOp.add(std::move(sub1));
    orOp.add(std::move(sub2));

    auto details = MatchDetails{};
    details.requestElemMatchKey();
    ASSERT(!orOp.matchesBSON(BSONObj{}, &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!orOp.matchesBSON(BSON("a" << BSON_ARRAY(10) << "b" << BSON_ARRAY(10)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(orOp.matchesBSON(BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(1 << 2)), &details));
    // The elem match key feature is not implemented for $or.
    ASSERT(!details.hasElemMatchKey());
}

DEATH_TEST_REGEX(OrOp, GetChildFailsOnIndexLargerThanChildren, "Tripwire assertion.*6400201") {
    auto baseOperand1 = BSON("$gt" << 10);
    auto baseOperand2 = BSON("$lt" << 0);
    auto baseOperand3 = BSON("b" << 100);
    auto sub1 = std::make_unique<GTMatchExpression>("a", baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a", baseOperand2["$lt"]);
    auto sub3 = std::make_unique<EqualityMatchExpression>("b", baseOperand3["b"]);

    auto orOp = OrMatchExpression{};
    orOp.add(std::move(sub1));
    orOp.add(std::move(sub2));
    orOp.add(std::move(sub3));

    const size_t numChildren = 3;
    ASSERT_EQ(orOp.numChildren(), numChildren);
    ASSERT_THROWS_CODE(orOp.getChild(numChildren), AssertionException, 6400201);
}

TEST(NorOp, NoClauses) {
    auto norOp = NorMatchExpression{};
    ASSERT(norOp.matchesBSON(BSONObj{}, nullptr));
}

TEST(NorOp, MatchesSingleClause) {
    auto baseOperand = BSON("$ne" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("a", baseOperand["$ne"]);
    auto ne = std::make_unique<NotMatchExpression>(eq.release());

    auto norOp = NorMatchExpression{};
    norOp.add(std::move(ne));

    ASSERT(!norOp.matchesBSON(BSON("a" << 4), nullptr));
    ASSERT(!norOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(norOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(norOp.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5)), nullptr));
}

TEST(NorOp, MatchesThreeClauses) {
    auto baseOperand1 = BSON("$gt" << 10);
    auto baseOperand2 = BSON("$lt" << 0);
    auto baseOperand3 = BSON("b" << 100);

    auto sub1 = std::make_unique<GTMatchExpression>("a", baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a", baseOperand2["$lt"]);
    auto sub3 = std::make_unique<EqualityMatchExpression>("b", baseOperand3["b"]);

    auto norOp = NorMatchExpression{};
    norOp.add(std::move(sub1));
    norOp.add(std::move(sub2));
    norOp.add(std::move(sub3));

    ASSERT(!norOp.matchesBSON(BSON("a" << -1), nullptr));
    ASSERT(!norOp.matchesBSON(BSON("a" << 11), nullptr));
    ASSERT(norOp.matchesBSON(BSON("a" << 5), nullptr));
    ASSERT(!norOp.matchesBSON(BSON("b" << 100), nullptr));
    ASSERT(norOp.matchesBSON(BSON("b" << 101), nullptr));
    ASSERT(norOp.matchesBSON(BSONObj{}, nullptr));
    ASSERT(!norOp.matchesBSON(BSON("a" << 11 << "b" << 100), nullptr));
}

TEST(NorOp, ElemMatchKey) {
    auto baseOperand1 = BSON("a" << 1);
    auto baseOperand2 = BSON("b" << 2);
    auto sub1 = std::make_unique<EqualityMatchExpression>("a", baseOperand1["a"]);
    auto sub2 = std::make_unique<EqualityMatchExpression>("b", baseOperand2["b"]);

    auto norOp = NorMatchExpression{};
    norOp.add(std::move(sub1));
    norOp.add(std::move(sub2));

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
    auto baseOperand1 = BSON("a" << 1);
    auto baseOperand2 = BSON("b" << 2);
    auto sub1 = EqualityMatchExpression{"a", baseOperand1["a"]};
    auto sub2 = EqualityMatchExpression{"b", baseOperand2["b"]};

    auto e1 = NorMatchExpression{};
    e1.add(sub1.shallowClone());
    e1.add(sub2.shallowClone());

    auto e2 = NorMatchExpression{};
    e2.add(sub1.shallowClone());

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
}

DEATH_TEST_REGEX(NorOp, GetChildFailsOnIndexLargerThanChildren, "Tripwire assertion.*6400201") {
    auto baseOperand1 = BSON("$gt" << 10);
    auto baseOperand2 = BSON("$lt" << 0);
    auto baseOperand3 = BSON("b" << 100);

    auto sub1 = std::make_unique<GTMatchExpression>("a", baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a", baseOperand2["$lt"]);
    auto sub3 = std::make_unique<EqualityMatchExpression>("b", baseOperand3["b"]);

    auto norOp = NorMatchExpression{};
    norOp.add(std::move(sub1));
    norOp.add(std::move(sub2));
    norOp.add(std::move(sub3));

    const size_t numChildren = 3;
    ASSERT_EQ(norOp.numChildren(), numChildren);
    ASSERT_THROWS_CODE(norOp.getChild(numChildren), AssertionException, 6400201);
}

}  // namespace mongo
