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

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(ElemMatchObjectMatchExpression, MatchesElementSingle) {
    auto baseOperand = BSON("b" << 5);
    auto match = BSON("a" << BSON_ARRAY(BSON("b" << 5.0)));
    auto notMatch = BSON("a" << BSON_ARRAY(BSON("b" << 6)));
    auto eq = std::make_unique<EqualityMatchExpression>("b", baseOperand["b"]);
    auto op = ElemMatchObjectMatchExpression{"a", std::move(eq)};
    ASSERT(op.matchesSingleElement(match["a"]));
    ASSERT(!op.matchesSingleElement(notMatch["a"]));
}

TEST(ElemMatchObjectMatchExpression, MatchesElementArray) {
    auto baseOperand = BSON("1" << 5);
    auto match = BSON("a" << BSON_ARRAY(BSON_ARRAY('s' << 5.0)));
    auto notMatch = BSON("a" << BSON_ARRAY(BSON_ARRAY(5 << 6)));
    auto eq = std::make_unique<EqualityMatchExpression>("1", baseOperand["1"]);
    auto op = ElemMatchObjectMatchExpression{"a", std::move(eq)};
    ASSERT(op.matchesSingleElement(match["a"]));
    ASSERT(!op.matchesSingleElement(notMatch["a"]));
}

TEST(ElemMatchObjectMatchExpression, MatchesElementMultiple) {
    auto baseOperand1 = BSON("b" << 5);
    auto baseOperand2 = BSON("b" << 6);
    auto baseOperand3 = BSON("c" << 7);
    auto notMatch1 = BSON("a" << BSON_ARRAY(BSON("b" << 5 << "c" << 7)));
    auto notMatch2 = BSON("a" << BSON_ARRAY(BSON("b" << 6 << "c" << 7)));
    auto notMatch3 = BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(5 << 6))));
    auto match = BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(5 << 6) << "c" << 7)));
    auto eq1 = std::make_unique<EqualityMatchExpression>("b", baseOperand1["b"]);
    auto eq2 = std::make_unique<EqualityMatchExpression>("b", baseOperand2["b"]);
    auto eq3 = std::make_unique<EqualityMatchExpression>("c", baseOperand3["c"]);

    auto andOp = std::make_unique<AndMatchExpression>();
    andOp->add(std::move(eq1));
    andOp->add(std::move(eq2));
    andOp->add(std::move(eq3));

    auto op = ElemMatchObjectMatchExpression{"a", std::move(andOp)};
    ASSERT(!op.matchesSingleElement(notMatch1["a"]));
    ASSERT(!op.matchesSingleElement(notMatch2["a"]));
    ASSERT(!op.matchesSingleElement(notMatch3["a"]));
    ASSERT(op.matchesSingleElement(match["a"]));
}

TEST(ElemMatchObjectMatchExpression, MatchesNonArray) {
    auto baseOperand = BSON("b" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("b", baseOperand["b"]);
    auto op = ElemMatchObjectMatchExpression{"a", std::move(eq)};
    // Directly nested objects are not matched with $elemMatch.  An intervening array is
    // required.
    ASSERT(!op.matchesBSON(BSON("a" << BSON("b" << 5)), nullptr));
    ASSERT(!op.matchesBSON(BSON("a" << BSON("0" << (BSON("b" << 5)))), nullptr));
    ASSERT(!op.matchesBSON(BSON("a" << 4), nullptr));
}

TEST(ElemMatchObjectMatchExpression, MatchesArrayObject) {
    auto baseOperand = BSON("b" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("b", baseOperand["b"]);
    auto op = ElemMatchObjectMatchExpression{"a", std::move(eq)};
    ASSERT(op.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 5))), nullptr));
    ASSERT(op.matchesBSON(BSON("a" << BSON_ARRAY(4 << BSON("b" << 5))), nullptr));
    ASSERT(op.matchesBSON(BSON("a" << BSON_ARRAY(BSONObj() << BSON("b" << 5))), nullptr));
    ASSERT(op.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 6) << BSON("b" << 5))), nullptr));
}

TEST(ElemMatchObjectMatchExpression, MatchesMultipleNamedValues) {
    auto baseOperand = BSON("c" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("c", baseOperand["c"]);
    auto op = ElemMatchObjectMatchExpression{"a.b", std::move(eq)};
    ASSERT(
        op.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(BSON("c" << 5))))), nullptr));
    ASSERT(op.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(BSON("c" << 1)))
                                                 << BSON("b" << BSON_ARRAY(BSON("c" << 5))))),
                          nullptr));
}

TEST(ElemMatchObjectMatchExpression, ElemMatchKey) {
    auto baseOperand = BSON("c" << 6);
    auto eq = std::make_unique<EqualityMatchExpression>("c", baseOperand["c"]);
    auto op = ElemMatchObjectMatchExpression{"a.b", std::move(eq)};
    auto details = MatchDetails{};
    details.requestElemMatchKey();
    ASSERT(!op.matchesBSON(BSONObj(), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!op.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY(BSON("c" << 7)))), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(op.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY(3 << BSON("c" << 6)))), &details));
    ASSERT(details.hasElemMatchKey());
    // The entry within the $elemMatch array is reported.
    ASSERT_EQUALS("1", details.elemMatchKey());
    ASSERT(op.matchesBSON(
        BSON("a" << BSON_ARRAY(1 << 2 << BSON("b" << BSON_ARRAY(3 << 5 << BSON("c" << 6))))),
        &details));
    ASSERT(details.hasElemMatchKey());
    // The entry within a parent of the $elemMatch array is reported.
    ASSERT_EQUALS("2", details.elemMatchKey());
}

TEST(ElemMatchObjectMatchExpression, Collation) {
    auto baseOperand = BSON("b"
                            << "string");
    auto match = BSON("a" << BSON_ARRAY(BSON("b"
                                             << "string")));
    auto notMatch = BSON("a" << BSON_ARRAY(BSON("b"
                                                << "string2")));
    auto eq = std::make_unique<EqualityMatchExpression>("b", baseOperand["b"]);
    auto op = ElemMatchObjectMatchExpression{"a", std::move(eq)};
    auto collator = CollatorInterfaceMock{CollatorInterfaceMock::MockType::kAlwaysEqual};
    op.setCollator(&collator);
    ASSERT(op.matchesSingleElement(match["a"]));
    ASSERT(op.matchesSingleElement(notMatch["a"]));
}

DEATH_TEST_REGEX(ElemMatchObjectMatchExpression,
                 GetChildFailsIndexGreaterThanOne,
                 "Tripwire assertion.*6400204") {
    auto baseOperand = BSON("c" << 6);
    auto eq = std::make_unique<EqualityMatchExpression>("c", baseOperand["c"]);
    auto op = ElemMatchObjectMatchExpression{"a.b", std::move(eq)};

    const size_t numChildren = 1;
    ASSERT_EQ(op.numChildren(), numChildren);
    ASSERT_THROWS_CODE(op.getChild(numChildren), AssertionException, 6400204);
}

/**
TEST(ElemMatchObjectMatchExpression, MatchesIndexKey) {
    auto baseOperand = BSON("b" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>();
    ASSERT(eq->init("b", baseOperand["b"]).isOK());
    auto op = ElemMatchObjectMatchExpression{};
    ASSERT(op.init("a", std::move(eq)).isOK());
    auto indexSpec = IndexSpec{BSON("a.b" << 1)};
    auto indexKey = BSON("" << "5");
    ASSERT(MatchMatchExpression::PartialMatchResult_Unknown ==
           op.matchesIndexKey(indexKey, indexSpec));
}
*/

TEST(ElemMatchValueMatchExpression, MatchesElementSingle) {
    auto baseOperand = BSON("$gt" << 5);
    auto match = BSON("a" << BSON_ARRAY(6));
    auto notMatch = BSON("a" << BSON_ARRAY(4));
    auto gt = std::make_unique<GTMatchExpression>("", baseOperand["$gt"]);
    auto op = ElemMatchValueMatchExpression{"a", std::unique_ptr<MatchExpression>{std::move(gt)}};
    ASSERT(op.matchesSingleElement(match["a"]));
    ASSERT(!op.matchesSingleElement(notMatch["a"]));
}

TEST(ElemMatchValueMatchExpression, MatchesElementMultiple) {
    auto baseOperand1 = BSON("$gt" << 1);
    auto baseOperand2 = BSON("$lt" << 10);
    auto notMatch1 = BSON("a" << BSON_ARRAY(0 << 1));
    auto notMatch2 = BSON("a" << BSON_ARRAY(10 << 11));
    auto match = BSON("a" << BSON_ARRAY(0 << 5 << 11));
    auto gt = std::make_unique<GTMatchExpression>("", baseOperand1["$gt"]);
    auto lt = std::make_unique<LTMatchExpression>("", baseOperand2["$lt"]);

    auto op = ElemMatchValueMatchExpression{"a"};
    op.add(std::move(gt));
    op.add(std::move(lt));

    ASSERT(!op.matchesSingleElement(notMatch1["a"]));
    ASSERT(!op.matchesSingleElement(notMatch2["a"]));
    ASSERT(op.matchesSingleElement(match["a"]));
}

TEST(ElemMatchValueMatchExpression, MatchesNonArray) {
    auto baseOperand = BSON("$gt" << 5);
    auto gt = std::make_unique<GTMatchExpression>("", baseOperand["$gt"]);
    auto op = ElemMatchObjectMatchExpression("a", std::move(gt));
    // Directly nested objects are not matched with $elemMatch.  An intervening array is
    // required.
    ASSERT(!op.matchesBSON(BSON("a" << 6), nullptr));
    ASSERT(!op.matchesBSON(BSON("a" << BSON("0" << 6)), nullptr));
}

TEST(ElemMatchValueMatchExpression, MatchesArrayScalar) {
    auto baseOperand = BSON("$gt" << 5);
    auto gt = std::make_unique<GTMatchExpression>("", baseOperand["$gt"]);
    auto op = ElemMatchValueMatchExpression{"a", std::unique_ptr<MatchExpression>{std::move(gt)}};
    ASSERT(op.matchesBSON(BSON("a" << BSON_ARRAY(6)), nullptr));
    ASSERT(op.matchesBSON(BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(op.matchesBSON(BSON("a" << BSON_ARRAY(BSONObj() << 7)), nullptr));
}

TEST(ElemMatchValueMatchExpression, MatchesMultipleNamedValues) {
    auto baseOperand = BSON("$gt" << 5);
    auto gt = std::make_unique<GTMatchExpression>("", baseOperand["$gt"]);
    auto op = ElemMatchValueMatchExpression{"a.b", std::unique_ptr<MatchExpression>{std::move(gt)}};
    ASSERT(op.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(6)))), nullptr));
    ASSERT(op.matchesBSON(
        BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(4)) << BSON("b" << BSON_ARRAY(4 << 6)))),
        nullptr));
}

TEST(ElemMatchValueMatchExpression, ElemMatchKey) {
    auto baseOperand = BSON("$gt" << 6);
    auto gt = std::make_unique<GTMatchExpression>("", baseOperand["$gt"]);
    auto op = ElemMatchValueMatchExpression{"a.b", std::unique_ptr<MatchExpression>{std::move(gt)}};
    auto details = MatchDetails{};
    details.requestElemMatchKey();
    ASSERT(!op.matchesBSON(BSONObj(), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!op.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY(2))), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(op.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY(3 << 7))), &details));
    ASSERT(details.hasElemMatchKey());
    // The entry within the $elemMatch array is reported.
    ASSERT_EQUALS("1", details.elemMatchKey());
    ASSERT(op.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << BSON("b" << BSON_ARRAY(3 << 7)))),
                          &details));
    ASSERT(details.hasElemMatchKey());
    // The entry within a parent of the $elemMatch array is reported.
    ASSERT_EQUALS("2", details.elemMatchKey());
}

DEATH_TEST_REGEX(ElemMatchValueMatchExpression,
                 GetChildFailsOnIndexLargerThanChildSet,
                 "Tripwire assertion.*6400205") {
    auto baseOperand = BSON("$gt" << 6);
    auto gt = std::make_unique<GTMatchExpression>("", baseOperand["$gt"]);
    auto op = ElemMatchValueMatchExpression{"a.b", std::unique_ptr<MatchExpression>{std::move(gt)}};

    const size_t numChildren = 1;
    ASSERT_EQ(op.numChildren(), numChildren);
    ASSERT_THROWS_CODE(op.getChild(numChildren), AssertionException, 6400205);
}

/**
TEST(ElemMatchValueMatchExpression, MatchesIndexKey) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LtOp>();
    ASSERT(lt->init("a", baseOperand["$lt"]).isOK());
    auto op = ElemMatchValueMatchExpression{};
    ASSERT(op.init("a", std::move(lt)).isOK());
    auto indexSpec = IndexSpec{BSON("a" << 1)};
    auto indexKey = BSON("" << "3");
    ASSERT(MatchMatchExpression::PartialMatchResult_Unknown ==
           op.matchesIndexKey(indexKey, indexSpec));
}
*/

TEST(AndOfElemMatch, MatchesElement) {
    auto baseOperanda1 = BSON("a" << 1);
    auto eqa1 = std::make_unique<EqualityMatchExpression>("a", baseOperanda1["a"]);

    auto baseOperandb1 = BSON("b" << 1);
    auto eqb1 = std::make_unique<EqualityMatchExpression>("b", baseOperandb1["b"]);

    auto and1 = std::make_unique<AndMatchExpression>();
    and1->add(std::move(eqa1));
    and1->add(std::move(eqb1));
    // and1 = { a : 1, b : 1 }

    auto elemMatch1 = std::make_unique<ElemMatchObjectMatchExpression>("x", std::move(and1));
    // elemMatch1 = { x : { $elemMatch : { a : 1, b : 1 } } }

    auto baseOperanda2 = BSON("a" << 2);
    auto eqa2 = std::make_unique<EqualityMatchExpression>("a", baseOperanda2["a"]);

    auto baseOperandb2 = BSON("b" << 2);
    auto eqb2 = std::make_unique<EqualityMatchExpression>("b", baseOperandb2["b"]);

    auto and2 = std::make_unique<AndMatchExpression>();
    and2->add(std::move(eqa2));
    and2->add(std::move(eqb2));
    // and2 = { a : 2, b : 2 }

    auto elemMatch2 = std::make_unique<ElemMatchObjectMatchExpression>("x", std::move(and2));
    // elemMatch2 = { x : { $elemMatch : { a : 2, b : 2 } } }

    auto andOfEM = std::make_unique<AndMatchExpression>();
    andOfEM->add(std::move(elemMatch1));
    andOfEM->add(std::move(elemMatch2));

    auto nonArray = BSON("x" << 4);
    ASSERT(!andOfEM->matchesSingleElement(nonArray["x"]));
    auto emptyArray = BSON("x" << BSONArray());
    ASSERT(!andOfEM->matchesSingleElement(emptyArray["x"]));
    auto nonObjArray = BSON("x" << BSON_ARRAY(4));
    ASSERT(!andOfEM->matchesSingleElement(nonObjArray["x"]));
    auto singleObjMatch = BSON("x" << BSON_ARRAY(BSON("a" << 1 << "b" << 1)));
    ASSERT(!andOfEM->matchesSingleElement(singleObjMatch["x"]));
    auto otherObjMatch = BSON("x" << BSON_ARRAY(BSON("a" << 2 << "b" << 2)));
    ASSERT(!andOfEM->matchesSingleElement(otherObjMatch["x"]));
    auto bothObjMatch =
        BSON("x" << BSON_ARRAY(BSON("a" << 1 << "b" << 1) << BSON("a" << 2 << "b" << 2)));
    ASSERT(andOfEM->matchesSingleElement(bothObjMatch["x"]));
    auto noObjMatch =
        BSON("x" << BSON_ARRAY(BSON("a" << 1 << "b" << 2) << BSON("a" << 2 << "b" << 1)));
    ASSERT(!andOfEM->matchesSingleElement(noObjMatch["x"]));
}

TEST(AndOfElemMatch, Matches) {
    auto baseOperandgt1 = BSON("$gt" << 1);
    auto gt1 = std::make_unique<GTMatchExpression>("", baseOperandgt1["$gt"]);

    auto baseOperandlt1 = BSON("$lt" << 10);
    auto lt1 = std::make_unique<LTMatchExpression>("", baseOperandlt1["$lt"]);

    auto elemMatch1 = std::make_unique<ElemMatchValueMatchExpression>("x");
    elemMatch1->add(std::move(gt1));
    elemMatch1->add(std::move(lt1));
    // elemMatch1 = { x : { $elemMatch : { $gt : 1 , $lt : 10 } } }

    auto baseOperandgt2 = BSON("$gt" << 101);
    auto gt2 = std::make_unique<GTMatchExpression>("", baseOperandgt2["$gt"]);

    auto baseOperandlt2 = BSON("$lt" << 110);
    auto lt2 = std::make_unique<LTMatchExpression>("", baseOperandlt2["$lt"]);

    auto elemMatch2 = std::make_unique<ElemMatchValueMatchExpression>("x");
    elemMatch2->add(std::move(gt2));
    elemMatch2->add(std::move(lt2));
    // elemMatch2 = { x : { $elemMatch : { $gt : 101 , $lt : 110 } } }

    auto andOfEM = std::make_unique<AndMatchExpression>();
    andOfEM->add(std::move(elemMatch1));
    andOfEM->add(std::move(elemMatch2));

    auto nonArray = BSON("x" << 4);
    ASSERT(!andOfEM->matchesBSON(nonArray, nullptr));
    auto emptyArray = BSON("x" << BSONArray());
    ASSERT(!andOfEM->matchesBSON(emptyArray, nullptr));
    auto nonNumberArray = BSON("x" << BSON_ARRAY("q"));
    ASSERT(!andOfEM->matchesBSON(nonNumberArray, nullptr));
    auto singleMatch = BSON("x" << BSON_ARRAY(5));
    ASSERT(!andOfEM->matchesBSON(singleMatch, nullptr));
    auto otherMatch = BSON("x" << BSON_ARRAY(105));
    ASSERT(!andOfEM->matchesBSON(otherMatch, nullptr));
    auto bothMatch = BSON("x" << BSON_ARRAY(5 << 105));
    ASSERT(andOfEM->matchesBSON(bothMatch, nullptr));
    auto neitherMatch = BSON("x" << BSON_ARRAY(0 << 200));
    ASSERT(!andOfEM->matchesBSON(neitherMatch, nullptr));
}

TEST(SizeMatchExpression, MatchesElement) {
    auto match = BSON("a" << BSON_ARRAY(5 << 6));
    auto notMatch = BSON("a" << BSON_ARRAY(5));
    auto size = SizeMatchExpression{"", 2};
    ASSERT(size.matchesSingleElement(match.firstElement()));
    ASSERT(!size.matchesSingleElement(notMatch.firstElement()));
}

TEST(SizeMatchExpression, MatchesNonArray) {
    // Non arrays do not match.
    auto stringValue = BSON("a"
                            << "z");
    auto numberValue = BSON("a" << 0);
    auto arrayValue = BSON("a" << BSONArray());
    auto size = SizeMatchExpression{"", 0};
    ASSERT(!size.matchesSingleElement(stringValue.firstElement()));
    ASSERT(!size.matchesSingleElement(numberValue.firstElement()));
    ASSERT(size.matchesSingleElement(arrayValue.firstElement()));
}

TEST(SizeMatchExpression, MatchesArray) {
    auto size = SizeMatchExpression{"a", 2};
    ASSERT(size.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5.5)), nullptr));
    // Arrays are not unwound to look for matching subarrays.
    ASSERT(!size.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5.5 << BSON_ARRAY(1 << 2))), nullptr));
}

TEST(SizeMatchExpression, MatchesNestedArray) {
    auto size = SizeMatchExpression{"a.2", 2};
    // A numerically referenced nested array is matched.
    ASSERT(size.matchesBSON(BSON("a" << BSON_ARRAY(4 << 5.5 << BSON_ARRAY(1 << 2))), nullptr));
}

TEST(SizeMatchExpression, ElemMatchKey) {
    auto size = SizeMatchExpression{"a.b", 3};
    auto details = MatchDetails{};
    details.requestElemMatchKey();
    ASSERT(!size.matchesBSON(BSON("a" << 1), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(size.matchesBSON(BSON("a" << BSON("b" << BSON_ARRAY(1 << 2 << 3))), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(size.matchesBSON(BSON("a" << BSON_ARRAY(2 << BSON("b" << BSON_ARRAY(1 << 2 << 3)))),
                            &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(SizeMatchExpression, Equivalent) {
    auto e1 = SizeMatchExpression{"a", 5};
    auto e2 = SizeMatchExpression{"a", 6};
    auto e3 = SizeMatchExpression{"v", 5};

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
    ASSERT(!e1.equivalent(&e3));
}

DEATH_TEST_REGEX(SizeMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400206") {
    auto e1 = SizeMatchExpression{"a", 5};

    const size_t numChildren = 0;
    ASSERT_EQ(e1.numChildren(), numChildren);
    ASSERT_THROWS_CODE(e1.getChild(0), AssertionException, 6400206);
}

/**
   TEST(SizeMatchExpression, MatchesIndexKey) {
   auto operand = BSON("$size" << 4);
   auto size = SizeMatchExpression{};
   ASSERT(size.init("a", operand["$size"]).isOK());
   auto indexSpec = IndexSpec{BSON("a" << 1)};
   auto indexKey = BSON("" << 1);
   ASSERT(MatchMatchExpression::PartialMatchResult_Unknown ==
          size.matchesIndexKey(indexKey, indexSpec));
   }
*/

}  // namespace mongo
