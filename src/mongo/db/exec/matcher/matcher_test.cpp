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

#include "mongo/db/exec/matcher/matcher.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <limits>

namespace mongo::evaluate_matcher_test {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

TEST(AlwaysFalseMatchExpression, RejectsAllObjects) {
    AlwaysFalseMatchExpression falseExpr;

    ASSERT_FALSE(exec::matcher::matchesBSON(&falseExpr, BSON("a" << BSONObj())));
    ASSERT_FALSE(exec::matcher::matchesBSON(&falseExpr, BSON("a" << 1)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&falseExpr, BSON("a" << "string")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&falseExpr, BSONObj()));
}

TEST(AlwaysTrueMatchExpression, AcceptsAllObjects) {
    AlwaysTrueMatchExpression trueExpr;

    ASSERT_TRUE(exec::matcher::matchesBSON(&trueExpr, BSON("a" << BSONObj())));
    ASSERT_TRUE(exec::matcher::matchesBSON(&trueExpr, BSON("a" << 1)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&trueExpr, BSON("a" << "string")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&trueExpr, BSONObj()));
}

TEST(ElemMatchObjectMatchExpression, MatchesElementSingle) {
    auto baseOperand = BSON("b" << 5);
    auto match = BSON("a" << BSON_ARRAY(BSON("b" << 5.0)));
    auto notMatch = BSON("a" << BSON_ARRAY(BSON("b" << 6)));
    auto eq = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand["b"]);
    auto op = ElemMatchObjectMatchExpression{"a"_sd, std::move(eq)};
    ASSERT(exec::matcher::matchesSingleElement(&op, match["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&op, notMatch["a"]));
}

TEST(ElemMatchObjectMatchExpression, MatchesElementArray) {
    auto baseOperand = BSON("1" << 5);
    auto match = BSON("a" << BSON_ARRAY(BSON_ARRAY('s' << 5.0)));
    auto notMatch = BSON("a" << BSON_ARRAY(BSON_ARRAY(5 << 6)));
    auto eq = std::make_unique<EqualityMatchExpression>("1"_sd, baseOperand["1"]);
    auto op = ElemMatchObjectMatchExpression{"a"_sd, std::move(eq)};
    ASSERT(exec::matcher::matchesSingleElement(&op, match["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&op, notMatch["a"]));
}

TEST(ElemMatchObjectMatchExpression, MatchesElementMultiple) {
    auto baseOperand1 = BSON("b" << 5);
    auto baseOperand2 = BSON("b" << 6);
    auto baseOperand3 = BSON("c" << 7);
    auto notMatch1 = BSON("a" << BSON_ARRAY(BSON("b" << 5 << "c" << 7)));
    auto notMatch2 = BSON("a" << BSON_ARRAY(BSON("b" << 6 << "c" << 7)));
    auto notMatch3 = BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(5 << 6))));
    auto match = BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(5 << 6) << "c" << 7)));
    auto eq1 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand1["b"]);
    auto eq2 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand2["b"]);
    auto eq3 = std::make_unique<EqualityMatchExpression>("c"_sd, baseOperand3["c"]);

    auto andOp = std::make_unique<AndMatchExpression>();
    andOp->add(std::move(eq1));
    andOp->add(std::move(eq2));
    andOp->add(std::move(eq3));

    auto op = ElemMatchObjectMatchExpression{"a"_sd, std::move(andOp)};
    ASSERT(!exec::matcher::matchesSingleElement(&op, notMatch1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&op, notMatch2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&op, notMatch3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&op, match["a"]));
}

TEST(ElemMatchObjectMatchExpression, MatchesNonArray) {
    auto baseOperand = BSON("b" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand["b"]);
    auto op = ElemMatchObjectMatchExpression{"a"_sd, std::move(eq)};
    // Directly nested objects are not matched with $elemMatch.  An intervening array is
    // required.
    ASSERT(!exec::matcher::matchesBSON(&op, BSON("a" << BSON("b" << 5)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&op, BSON("a" << BSON("0" << (BSON("b" << 5)))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&op, BSON("a" << 4), nullptr));
}

TEST(ElemMatchObjectMatchExpression, MatchesArrayObject) {
    auto baseOperand = BSON("b" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand["b"]);
    auto op = ElemMatchObjectMatchExpression{"a"_sd, std::move(eq)};
    ASSERT(exec::matcher::matchesBSON(&op, BSON("a" << BSON_ARRAY(BSON("b" << 5))), nullptr));
    ASSERT(exec::matcher::matchesBSON(&op, BSON("a" << BSON_ARRAY(4 << BSON("b" << 5))), nullptr));
    ASSERT(exec::matcher::matchesBSON(
        &op, BSON("a" << BSON_ARRAY(BSONObj() << BSON("b" << 5))), nullptr));
    ASSERT(exec::matcher::matchesBSON(
        &op, BSON("a" << BSON_ARRAY(BSON("b" << 6) << BSON("b" << 5))), nullptr));
}

TEST(ElemMatchObjectMatchExpression, MatchesMultipleNamedValues) {
    auto baseOperand = BSON("c" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("c"_sd, baseOperand["c"]);
    auto op = ElemMatchObjectMatchExpression{"a.b"_sd, std::move(eq)};
    ASSERT(exec::matcher::matchesBSON(
        &op, BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(BSON("c" << 5))))), nullptr));
    ASSERT(exec::matcher::matchesBSON(
        &op,
        BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(BSON("c" << 1)))
                               << BSON("b" << BSON_ARRAY(BSON("c" << 5))))),
        nullptr));
}

TEST(ElemMatchObjectMatchExpression, ElemMatchKey) {
    auto baseOperand = BSON("c" << 6);
    auto eq = std::make_unique<EqualityMatchExpression>("c"_sd, baseOperand["c"]);
    auto op = ElemMatchObjectMatchExpression{"a.b"_sd, std::move(eq)};
    auto details = MatchDetails{};
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&op, BSONObj(), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!exec::matcher::matchesBSON(
        &op, BSON("a" << BSON("b" << BSON_ARRAY(BSON("c" << 7)))), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &op, BSON("a" << BSON("b" << BSON_ARRAY(3 << BSON("c" << 6)))), &details));
    ASSERT(details.hasElemMatchKey());
    // The entry within the $elemMatch array is reported.
    ASSERT_EQUALS("1", details.elemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &op,
        BSON("a" << BSON_ARRAY(1 << 2 << BSON("b" << BSON_ARRAY(3 << 5 << BSON("c" << 6))))),
        &details));
    ASSERT(details.hasElemMatchKey());
    // The entry within a parent of the $elemMatch array is reported.
    ASSERT_EQUALS("2", details.elemMatchKey());
}

TEST(ElemMatchObjectMatchExpression, Collation) {
    auto baseOperand = BSON("b" << "string");
    auto match = BSON("a" << BSON_ARRAY(BSON("b" << "string")));
    auto notMatch = BSON("a" << BSON_ARRAY(BSON("b" << "string2")));
    auto eq = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand["b"]);
    auto op = ElemMatchObjectMatchExpression{"a"_sd, std::move(eq)};
    auto collator = CollatorInterfaceMock{CollatorInterfaceMock::MockType::kAlwaysEqual};
    op.setCollator(&collator);
    ASSERT(exec::matcher::matchesSingleElement(&op, match["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&op, notMatch["a"]));
}

TEST(ElemMatchValueMatchExpression, MatchesElementSingle) {
    auto baseOperand = BSON("$gt" << 5);
    auto match = BSON("a" << BSON_ARRAY(6));
    auto notMatch = BSON("a" << BSON_ARRAY(4));
    auto gt = std::make_unique<GTMatchExpression>(""_sd, baseOperand["$gt"]);
    auto op =
        ElemMatchValueMatchExpression{"a"_sd, std::unique_ptr<MatchExpression>{std::move(gt)}};
    ASSERT(exec::matcher::matchesSingleElement(&op, match["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&op, notMatch["a"]));
}

TEST(ElemMatchValueMatchExpression, MatchesElementMultiple) {
    auto baseOperand1 = BSON("$gt" << 1);
    auto baseOperand2 = BSON("$lt" << 10);
    auto notMatch1 = BSON("a" << BSON_ARRAY(0 << 1));
    auto notMatch2 = BSON("a" << BSON_ARRAY(10 << 11));
    auto match = BSON("a" << BSON_ARRAY(0 << 5 << 11));
    auto gt = std::make_unique<GTMatchExpression>(""_sd, baseOperand1["$gt"]);
    auto lt = std::make_unique<LTMatchExpression>(""_sd, baseOperand2["$lt"]);

    auto op = ElemMatchValueMatchExpression{"a"_sd};
    op.add(std::move(gt));
    op.add(std::move(lt));

    ASSERT(!exec::matcher::matchesSingleElement(&op, notMatch1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&op, notMatch2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&op, match["a"]));
}

TEST(ElemMatchValueMatchExpression, MatchesNonArray) {
    auto baseOperand = BSON("$gt" << 5);
    auto gt = std::make_unique<GTMatchExpression>(""_sd, baseOperand["$gt"]);
    auto op = ElemMatchObjectMatchExpression("a"_sd, std::move(gt));
    // Directly nested objects are not matched with $elemMatch.  An intervening array is
    // required.
    ASSERT(!exec::matcher::matchesBSON(&op, BSON("a" << 6), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&op, BSON("a" << BSON("0" << 6)), nullptr));
}

TEST(ElemMatchValueMatchExpression, MatchesArrayScalar) {
    auto baseOperand = BSON("$gt" << 5);
    auto gt = std::make_unique<GTMatchExpression>(""_sd, baseOperand["$gt"]);
    auto op =
        ElemMatchValueMatchExpression{"a"_sd, std::unique_ptr<MatchExpression>{std::move(gt)}};
    ASSERT(exec::matcher::matchesBSON(&op, BSON("a" << BSON_ARRAY(6)), nullptr));
    ASSERT(exec::matcher::matchesBSON(&op, BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(exec::matcher::matchesBSON(&op, BSON("a" << BSON_ARRAY(BSONObj() << 7)), nullptr));
}

TEST(ElemMatchValueMatchExpression, MatchesMultipleNamedValues) {
    auto baseOperand = BSON("$gt" << 5);
    auto gt = std::make_unique<GTMatchExpression>(""_sd, baseOperand["$gt"]);
    auto op =
        ElemMatchValueMatchExpression{"a.b"_sd, std::unique_ptr<MatchExpression>{std::move(gt)}};
    ASSERT(exec::matcher::matchesBSON(
        &op, BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(6)))), nullptr));
    ASSERT(exec::matcher::matchesBSON(
        &op,
        BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(4)) << BSON("b" << BSON_ARRAY(4 << 6)))),
        nullptr));
}

TEST(ElemMatchValueMatchExpression, ElemMatchKey) {
    auto baseOperand = BSON("$gt" << 6);
    auto gt = std::make_unique<GTMatchExpression>(""_sd, baseOperand["$gt"]);
    auto op =
        ElemMatchValueMatchExpression{"a.b"_sd, std::unique_ptr<MatchExpression>{std::move(gt)}};
    auto details = MatchDetails{};
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&op, BSONObj(), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!exec::matcher::matchesBSON(&op, BSON("a" << BSON("b" << BSON_ARRAY(2))), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&op, BSON("a" << BSON("b" << BSON_ARRAY(3 << 7))), &details));
    ASSERT(details.hasElemMatchKey());
    // The entry within the $elemMatch array is reported.
    ASSERT_EQUALS("1", details.elemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &op, BSON("a" << BSON_ARRAY(1 << 2 << BSON("b" << BSON_ARRAY(3 << 7)))), &details));
    ASSERT(details.hasElemMatchKey());
    // The entry within a parent of the $elemMatch array is reported.
    ASSERT_EQUALS("2", details.elemMatchKey());
}

TEST(AndOfElemMatch, MatchesElement) {
    auto baseOperanda1 = BSON("a" << 1);
    auto eqa1 = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperanda1["a"]);

    auto baseOperandb1 = BSON("b" << 1);
    auto eqb1 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperandb1["b"]);

    auto and1 = std::make_unique<AndMatchExpression>();
    and1->add(std::move(eqa1));
    and1->add(std::move(eqb1));
    // and1 = { a : 1, b : 1 }

    auto elemMatch1 = std::make_unique<ElemMatchObjectMatchExpression>("x"_sd, std::move(and1));
    // elemMatch1 = { x : { $elemMatch : { a : 1, b : 1 } } }

    auto baseOperanda2 = BSON("a" << 2);
    auto eqa2 = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperanda2["a"]);

    auto baseOperandb2 = BSON("b" << 2);
    auto eqb2 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperandb2["b"]);

    auto and2 = std::make_unique<AndMatchExpression>();
    and2->add(std::move(eqa2));
    and2->add(std::move(eqb2));
    // and2 = { a : 2, b : 2 }

    auto elemMatch2 = std::make_unique<ElemMatchObjectMatchExpression>("x"_sd, std::move(and2));
    // elemMatch2 = { x : { $elemMatch : { a : 2, b : 2 } } }

    auto andOfEM = std::make_unique<AndMatchExpression>();
    andOfEM->add(std::move(elemMatch1));
    andOfEM->add(std::move(elemMatch2));

    auto nonArray = BSON("x" << 4);
    ASSERT(!exec::matcher::matchesSingleElement(andOfEM.get(), nonArray["x"]));
    auto emptyArray = BSON("x" << BSONArray());
    ASSERT(!exec::matcher::matchesSingleElement(andOfEM.get(), emptyArray["x"]));
    auto nonObjArray = BSON("x" << BSON_ARRAY(4));
    ASSERT(!exec::matcher::matchesSingleElement(andOfEM.get(), nonObjArray["x"]));
    auto singleObjMatch = BSON("x" << BSON_ARRAY(BSON("a" << 1 << "b" << 1)));
    ASSERT(!exec::matcher::matchesSingleElement(andOfEM.get(), singleObjMatch["x"]));
    auto otherObjMatch = BSON("x" << BSON_ARRAY(BSON("a" << 2 << "b" << 2)));
    ASSERT(!exec::matcher::matchesSingleElement(andOfEM.get(), otherObjMatch["x"]));
    auto bothObjMatch =
        BSON("x" << BSON_ARRAY(BSON("a" << 1 << "b" << 1) << BSON("a" << 2 << "b" << 2)));
    ASSERT(exec::matcher::matchesSingleElement(andOfEM.get(), bothObjMatch["x"]));
    auto noObjMatch =
        BSON("x" << BSON_ARRAY(BSON("a" << 1 << "b" << 2) << BSON("a" << 2 << "b" << 1)));
    ASSERT(!exec::matcher::matchesSingleElement(andOfEM.get(), noObjMatch["x"]));
}

TEST(AndOfElemMatch, Matches) {
    auto baseOperandgt1 = BSON("$gt" << 1);
    auto gt1 = std::make_unique<GTMatchExpression>(""_sd, baseOperandgt1["$gt"]);

    auto baseOperandlt1 = BSON("$lt" << 10);
    auto lt1 = std::make_unique<LTMatchExpression>(""_sd, baseOperandlt1["$lt"]);

    auto elemMatch1 = std::make_unique<ElemMatchValueMatchExpression>("x"_sd);
    elemMatch1->add(std::move(gt1));
    elemMatch1->add(std::move(lt1));
    // elemMatch1 = { x : { $elemMatch : { $gt : 1 , $lt : 10 } } }

    auto baseOperandgt2 = BSON("$gt" << 101);
    auto gt2 = std::make_unique<GTMatchExpression>(""_sd, baseOperandgt2["$gt"]);

    auto baseOperandlt2 = BSON("$lt" << 110);
    auto lt2 = std::make_unique<LTMatchExpression>(""_sd, baseOperandlt2["$lt"]);

    auto elemMatch2 = std::make_unique<ElemMatchValueMatchExpression>("x"_sd);
    elemMatch2->add(std::move(gt2));
    elemMatch2->add(std::move(lt2));
    // elemMatch2 = { x : { $elemMatch : { $gt : 101 , $lt : 110 } } }

    auto andOfEM = std::make_unique<AndMatchExpression>();
    andOfEM->add(std::move(elemMatch1));
    andOfEM->add(std::move(elemMatch2));

    auto nonArray = BSON("x" << 4);
    ASSERT(!exec::matcher::matchesBSON(andOfEM.get(), nonArray, nullptr));
    auto emptyArray = BSON("x" << BSONArray());
    ASSERT(!exec::matcher::matchesBSON(andOfEM.get(), emptyArray, nullptr));
    auto nonNumberArray = BSON("x" << BSON_ARRAY("q"));
    ASSERT(!exec::matcher::matchesBSON(andOfEM.get(), nonNumberArray, nullptr));
    auto singleMatch = BSON("x" << BSON_ARRAY(5));
    ASSERT(!exec::matcher::matchesBSON(andOfEM.get(), singleMatch, nullptr));
    auto otherMatch = BSON("x" << BSON_ARRAY(105));
    ASSERT(!exec::matcher::matchesBSON(andOfEM.get(), otherMatch, nullptr));
    auto bothMatch = BSON("x" << BSON_ARRAY(5 << 105));
    ASSERT(exec::matcher::matchesBSON(andOfEM.get(), bothMatch, nullptr));
    auto neitherMatch = BSON("x" << BSON_ARRAY(0 << 200));
    ASSERT(!exec::matcher::matchesBSON(andOfEM.get(), neitherMatch, nullptr));
}

TEST(SizeMatchExpression, MatchesElement) {
    auto match = BSON("a" << BSON_ARRAY(5 << 6));
    auto notMatch = BSON("a" << BSON_ARRAY(5));
    auto size = SizeMatchExpression{""_sd, 2};
    ASSERT(exec::matcher::matchesSingleElement(&size, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&size, notMatch.firstElement()));
}

TEST(SizeMatchExpression, MatchesNonArray) {
    // Non arrays do not match.
    auto stringValue = BSON("a" << "z");
    auto numberValue = BSON("a" << 0);
    auto arrayValue = BSON("a" << BSONArray());
    auto size = SizeMatchExpression{""_sd, 0};
    ASSERT(!exec::matcher::matchesSingleElement(&size, stringValue.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&size, numberValue.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&size, arrayValue.firstElement()));
}

TEST(SizeMatchExpression, MatchesArray) {
    auto size = SizeMatchExpression{"a"_sd, 2};
    ASSERT(exec::matcher::matchesBSON(&size, BSON("a" << BSON_ARRAY(4 << 5.5)), nullptr));
    // Arrays are not unwound to look for matching subarrays.
    ASSERT(!exec::matcher::matchesBSON(
        &size, BSON("a" << BSON_ARRAY(4 << 5.5 << BSON_ARRAY(1 << 2))), nullptr));
}

TEST(SizeMatchExpression, MatchesNestedArray) {
    auto size = SizeMatchExpression{"a.2"_sd, 2};
    // A numerically referenced nested array is matched.
    ASSERT(exec::matcher::matchesBSON(
        &size, BSON("a" << BSON_ARRAY(4 << 5.5 << BSON_ARRAY(1 << 2))), nullptr));
}

TEST(SizeMatchExpression, ElemMatchKey) {
    auto size = SizeMatchExpression{"a.b"_sd, 3};
    auto details = MatchDetails{};
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&size, BSON("a" << 1), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &size, BSON("a" << BSON("b" << BSON_ARRAY(1 << 2 << 3))), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &size, BSON("a" << BSON_ARRAY(2 << BSON("b" << BSON_ARRAY(1 << 2 << 3)))), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(InternalExprComparisonMatchExpression, DoesNotPerformTypeBracketing) {
    BSONObj operand = BSON("x" << 2);
    {
        InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << MINKEY)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("y" << 0)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << BSONNULL)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << BSONUndefined)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << 1)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << 2)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << 3.5)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << "string")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON("a" << 1))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << OID())));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << DATENOW)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << Timestamp(0, 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << "/^m/")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << MAXKEY)));
    }
    {
        InternalExprGTEMatchExpression gte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << MINKEY)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("y" << 0)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << BSONNULL)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << BSONUndefined)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << 1)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << 2)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << 3.5)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << "string")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON("a" << 1))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << OID())));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << DATENOW)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << Timestamp(0, 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << "/^m/")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << MAXKEY)));
    }
    {
        InternalExprLTMatchExpression lt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << MINKEY)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("y" << 0)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSONNULL)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSONUndefined)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << 1)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << 2)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << 3.5)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << "string")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON("a" << 1))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &lt,
            BSON("x" << BSON_ARRAY(1 << 2
                                     << 3))));  // Always returns true if path contains an array.
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << OID())));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << DATENOW)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << Timestamp(0, 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << "/^m/")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << MAXKEY)));
    }
    {
        InternalExprLTEMatchExpression lte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << MINKEY)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("y" << 0)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSONNULL)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSONUndefined)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << 1)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << 2)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << 3.5)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << "string")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON("a" << 1))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &lte,
            BSON("x" << BSON_ARRAY(1 << 2
                                     << 3))));  // Always returns true if path contains an array.
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << OID())));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << DATENOW)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << Timestamp(0, 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << "/^m/")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << MAXKEY)));
    }
}

TEST(InternalExprComparisonMatchExpression, CorrectlyComparesNaN) {
    BSONObj operand = BSON("x" << kNaN);
    // This behavior differs from how regular comparison MatchExpressions treat NaN, and places NaN
    // within the total order of values as less than all numbers.
    {
        InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << MINKEY)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << BSONNULL)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << kNaN)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << Decimal128::kNegativeInfinity)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << 2)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << 3.5)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << MAXKEY)));
    }
    {
        InternalExprGTEMatchExpression gte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << MINKEY)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << BSONNULL)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << kNaN)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << Decimal128::kNegativeInfinity)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << 2)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << 3.5)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << MAXKEY)));
    }
    {
        InternalExprLTMatchExpression lt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << MINKEY)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSONNULL)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << kNaN)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << Decimal128::kNegativeInfinity)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << 2)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << 3.5)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << MAXKEY)));
    }
    {
        InternalExprLTEMatchExpression lte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << MINKEY)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSONNULL)));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << kNaN)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << Decimal128::kNegativeInfinity)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << 2)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << 3.5)));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << MAXKEY)));
    }
}

// Note we depend on this for our ability to rewrite predicates on timeseries collections where the
// buckets have mixed types.
TEST(InternalExprComparisonMatchExpression, AlwaysReturnsTrueWithLeafArrays) {
    BSONObj operand = BSON("x" << 2);
    {
        InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(0))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << 1)));
    }
    {
        InternalExprGTEMatchExpression gte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(0))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << 1)));
    }
    {
        InternalExprLTMatchExpression lt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(0))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << 3)));
    }
    {
        InternalExprLTEMatchExpression lte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(0))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << 3)));
    }
}

TEST(InternalExprComparisonMatchExpression, AlwaysReturnsTrueWithNonLeafArrays) {
    BSONObj operand = BSON("x.y" << 2);
    {
        InternalExprGTMatchExpression gt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON_ARRAY(BSON("y" << 1)))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &gt, BSON("x" << BSON_ARRAY(BSON("y" << 2) << BSON("y" << 3)))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gt, BSON("x" << BSON("y" << 1))));
    }
    {
        InternalExprGTEMatchExpression gte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON_ARRAY(BSON("y" << 1)))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &gte, BSON("x" << BSON_ARRAY(BSON("y" << 2) << BSON("y" << 3)))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&gte, BSON("x" << BSON("y" << 1))));
    }
    {
        InternalExprLTMatchExpression lt(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON_ARRAY(BSON("y" << 3)))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &lt, BSON("x" << BSON_ARRAY(BSON("y" << 1) << BSON("y" << 2)))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lt, BSON("x" << BSON("y" << 3))));
    }
    {
        InternalExprLTEMatchExpression lte(operand.firstElement().fieldNameStringData(),
                                           operand.firstElement());
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(BSONNULL))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(1 << 2 << 3))));
        ASSERT_TRUE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON_ARRAY(BSON("y" << 3)))));
        ASSERT_TRUE(exec::matcher::matchesBSON(
            &lte, BSON("x" << BSON_ARRAY(BSON("y" << 1) << BSON("y" << 2)))));
        ASSERT_FALSE(exec::matcher::matchesBSON(&lte, BSON("x" << BSON("y" << 3))));
    }
}

TEST(InternalExprEqMatchExpression, CorrectlyMatchesScalarElements) {
    BSONObj operand1 = BSON("a" << 5);

    InternalExprEqMatchExpression eq1(operand1.firstElement().fieldNameStringData(),
                                      operand1.firstElement());
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq1, BSON("a" << 5.0)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq1, BSON("a" << 6)));

    BSONObj operand2 = BSON("a" << "str");
    InternalExprEqMatchExpression eq2(operand2.firstElement().fieldNameStringData(),
                                      operand2.firstElement());
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq2, BSON("a" << "str")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq2, BSON("a" << "string")));
}

TEST(InternalExprEqMatchExpression, StringMatchingRespectsCollation) {
    BSONObj operand = BSON("a" << "string");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    eq.setCollator(&collator);
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << "string2")));
}

TEST(InternalExprEqMatchExpression, ComparisonRespectsNewCollationAfterCallingSetCollator) {
    BSONObj operand = BSON("a" << "string1");

    CollatorInterfaceMock collatorAlwaysEqual(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock collatorCompareLower(CollatorInterfaceMock::MockType::kToLowerString);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    eq.setCollator(&collatorAlwaysEqual);
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << "string2")));


    eq.setCollator(&collatorCompareLower);
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << "string1")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << "STRING1")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << "string2")));
}

TEST(InternalExprEqMatchExpression, CorrectlyMatchesArrayElement) {
    BSONObj operand = BSON("a.b" << 5);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON("b" << 5))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON("b" << 6))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY("b" << 5))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY("b" << BSON_ARRAY(5)))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(5 << "b"))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY("b" << 5 << 5))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY("b" << 6))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(BSON("b" << 6)))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << 1)));
}

TEST(InternalExprEqMatchExpression, CorrectlyMatchesNullElement) {
    BSONObj operand = BSON("a" << BSONNULL);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    // Expression equality to null should match literal null, but not missing or undefined.
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSONNULL)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSONObj()));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << BSONUndefined)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << 4)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(1 << 2))));
}

TEST(InternalExprEqMatchExpression, CorrectlyMatchesNaN) {
    BSONObj operand = BSON("x" << kNaN);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("x" << kNaN)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("x" << 0)));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSONObj()));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("x" << BSON_ARRAY(1))));
}

TEST(InternalExprEqMatchExpression, DoesNotTraverseLeafArrays) {
    BSONObj operand = BSON("a" << 5);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << 5.0)));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY("foo"))));
}

TEST(InternalExprEqMatchExpression, CorrectlyMatchesSubfieldAlongDottedPath) {
    BSONObj operand = BSON("x.y.z" << 5);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("x" << BSON("y" << BSON("z" << 5)))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("x" << BSON("y" << BSON("z" << 4)))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("x" << BSON("y" << 5))));
}

TEST(InternalExprEqMatchExpression, AlwaysMatchesDocumentWithArrayAlongPath) {
    BSONObj operand = BSON("x.y.z" << 5);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("x" << BSON_ARRAY(6))));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("x" << BSON("y" << BSON_ARRAY(6)))));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&eq, BSON("x" << BSON_ARRAY(BSON("y" << BSON("z" << 6))))));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&eq, BSON("x" << BSON("y" << BSON_ARRAY(BSON("z" << 6))))));
    ASSERT_TRUE(
        exec::matcher::matchesBSON(&eq, BSON("x" << BSON("y" << BSON("z" << BSON_ARRAY(10))))));

    ASSERT_FALSE(exec::matcher::matchesBSON(
        &eq, BSON("x" << BSON("y" << BSON("z" << BSON("foo" << BSON_ARRAY(10)))))));
}

TEST(InternalExprEqMatchExpression, ConsidersFieldNameInObjectEquality) {
    BSONObj operand = BSON("x" << BSON("a" << 1));

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("x" << BSON("a" << 1))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("x" << BSON("y" << 1))));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("y" << BSON("a" << 1))));
}

TEST(ComparisonMatchExpression, StringMatchingWithNullCollatorUsesBinaryComparison) {
    BSONObj operand = BSON("a" << "string");
    EqualityMatchExpression eq("a"_sd, operand["a"]);
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << "string2"), nullptr));
}

TEST(ComparisonMatchExpression, StringMatchingRespectsCollation) {
    BSONObj operand = BSON("a" << "string");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    EqualityMatchExpression eq("a"_sd, operand["a"]);
    eq.setCollator(&collator);
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << "string2"), nullptr));
}

TEST(ComparisonMatchExpression, UnequalLengthString) {
    BSONObj operand = BSON("a" << "abc");
    BSONObj match = BSON("a" << "abcd");
    EqualityMatchExpression eq(""_sd, operand["a"]);
    ASSERT(!exec::matcher::matchesSingleElement(&eq, match.firstElement()));
}

TEST(ComparisonMatchExpression, NaNComparison) {
    BSONObj match = BSON("a" << 10);

    BSONObj operand = BSON("a" << sqrt(-2));
    EqualityMatchExpression eq(""_sd, operand["a"]);
    ASSERT(!exec::matcher::matchesSingleElement(&eq, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&eq, operand.firstElement()));

    BSONObj gteOp = BSON("$gte" << sqrt(-2));
    GTEMatchExpression gte(""_sd, gteOp["$gte"]);
    ASSERT(!exec::matcher::matchesSingleElement(&gte, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&gte, operand.firstElement()));

    BSONObj gtOp = BSON("$gt" << sqrt(-2));
    GTMatchExpression gt(""_sd, gtOp["$gt"]);
    ASSERT(!exec::matcher::matchesSingleElement(&gt, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&gt, operand.firstElement()));

    BSONObj lteOp = BSON("$lte" << sqrt(-2));
    LTEMatchExpression lte(""_sd, lteOp["$lte"]);
    ASSERT(!exec::matcher::matchesSingleElement(&lte, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&lte, operand.firstElement()));

    BSONObj ltOp = BSON("$lt" << sqrt(-2));
    LTMatchExpression lt(""_sd, ltOp["$lt"]);
    ASSERT(!exec::matcher::matchesSingleElement(&lt, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lt, operand.firstElement()));
}

TEST(ComparisonMatchExpression, NaNComparisonDecimal) {
    BSONObj match = BSON("a" << 10);

    BSONObj operand = BSON("a" << Decimal128::kPositiveNaN);
    EqualityMatchExpression eq(""_sd, operand["a"]);
    ASSERT(!exec::matcher::matchesSingleElement(&eq, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&eq, operand.firstElement()));

    BSONObj gteOp = BSON("$gte" << Decimal128::kPositiveNaN);
    GTEMatchExpression gte(""_sd, gteOp["$gte"]);
    ASSERT(!exec::matcher::matchesSingleElement(&gte, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&gte, operand.firstElement()));

    BSONObj gtOp = BSON("$gt" << Decimal128::kPositiveNaN);
    GTMatchExpression gt(""_sd, gtOp["$gt"]);
    ASSERT(!exec::matcher::matchesSingleElement(&gt, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&gt, operand.firstElement()));

    BSONObj lteOp = BSON("$lte" << Decimal128::kPositiveNaN);
    LTEMatchExpression lte(""_sd, lteOp["$lte"]);
    ASSERT(!exec::matcher::matchesSingleElement(&lte, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&lte, operand.firstElement()));

    BSONObj ltOp = BSON("$lt" << Decimal128::kPositiveNaN);
    LTMatchExpression lt(""_sd, ltOp["$lt"]);
    ASSERT(!exec::matcher::matchesSingleElement(&lt, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lt, operand.firstElement()));
}

TEST(EqOp, MatchesElement) {
    BSONObj operand = BSON("a" << 5);
    BSONObj match = BSON("a" << 5.0);
    BSONObj notMatch = BSON("a" << 6);

    EqualityMatchExpression eq(""_sd, operand["a"]);
    ASSERT(exec::matcher::matchesSingleElement(&eq, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&eq, notMatch.firstElement()));

    ASSERT(eq.equivalent(&eq));
}

TEST(EqOp, MatchesScalar) {
    BSONObj operand = BSON("a" << 5);
    EqualityMatchExpression eq("a"_sd, operand["a"]);
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << 5.0), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << 4), nullptr));
}

TEST(EqOp, MatchesArrayValue) {
    BSONObj operand = BSON("a" << 5);
    EqualityMatchExpression eq("a"_sd, operand["a"]);
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(5.0 << 6)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(6 << 7)), nullptr));
}

TEST(EqOp, MatchesReferencedObjectValue) {
    BSONObj operand = BSON("a.b" << 5);
    EqualityMatchExpression eq("a.b"_sd, operand["a.b"]);
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << BSON("b" << 5)), nullptr));
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << BSON("b" << BSON_ARRAY(5))), nullptr));
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(BSON("b" << 5))), nullptr));
}

TEST(EqOp, MatchesReferencedArrayValue) {
    BSONObj operand = BSON("a.0" << 5);
    EqualityMatchExpression eq("a.0"_sd, operand["a.0"]);
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(5)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), nullptr));
}

TEST(EqOp, MatchesNull) {
    BSONObj operand = BSON("a" << BSONNULL);
    EqualityMatchExpression eq("a"_sd, operand["a"]);
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSONObj(), nullptr));
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("a" << BSONNULL), nullptr));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << 4), nullptr));

    // {$eq:null} has special semantics which say that missing matched in addition to literal nulls.
    ASSERT_TRUE(exec::matcher::matchesBSON(&eq, BSON("b" << 4), nullptr));
    ASSERT_FALSE(exec::matcher::matchesBSON(&eq, BSON("a" << BSONUndefined), nullptr));
}

// This test documents how the matcher currently works,
// not necessarily how it should work ideally.
TEST(EqOp, MatchesNestedNull) {
    BSONObj operand = BSON("a.b" << BSONNULL);
    EqualityMatchExpression eq("a.b"_sd, operand["a.b"]);
    // null matches any empty object that is on a subpath of a.b
    ASSERT(exec::matcher::matchesBSON(&eq, BSONObj(), nullptr));
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << BSONObj()), nullptr));
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(BSONObj())), nullptr));
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << BSON("b" << BSONNULL)), nullptr));
    // b does not exist as an element in array under a.
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << BSONArray()), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(BSONNULL)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(1 << 2)), nullptr));
    // a.b exists but is not null.
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << BSON("b" << 4)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << BSON("b" << BSONObj())), nullptr));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("b" << 4), nullptr));
}

TEST(EqOp, MatchesMinKey) {
    BSONObj operand = BSON("a" << BSONType::minKey);
    EqualityMatchExpression eq("a"_sd, operand["a"]);
    BSONObj minKeyObj = BSON("a" << BSONType::minKey);
    BSONObj maxKeyObj = BSON("a" << BSONType::maxKey);
    BSONObj numObj = BSON("a" << 4);

    ASSERT(exec::matcher::matchesBSON(&eq, minKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, maxKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, numObj, nullptr));

    ASSERT(exec::matcher::matchesSingleElement(&eq, minKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&eq, maxKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&eq, numObj.firstElement()));
}


TEST(EqOp, MatchesMaxKey) {
    BSONObj operand = BSON("a" << BSONType::maxKey);
    EqualityMatchExpression eq("a"_sd, operand["a"]);
    BSONObj minKeyObj = BSON("a" << BSONType::minKey);
    BSONObj maxKeyObj = BSON("a" << BSONType::maxKey);
    BSONObj numObj = BSON("a" << 4);

    ASSERT(!exec::matcher::matchesBSON(&eq, minKeyObj, nullptr));
    ASSERT(exec::matcher::matchesBSON(&eq, maxKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, numObj, nullptr));

    ASSERT(!exec::matcher::matchesSingleElement(&eq, minKeyObj.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&eq, maxKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&eq, numObj.firstElement()));
}

TEST(EqOp, MatchesFullArray) {
    BSONObj operand = BSON("a" << BSON_ARRAY(1 << 2));
    EqualityMatchExpression eq("a"_sd, operand["a"]);
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(1 << 2)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(1 << 2 << 3)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(1)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << 1), nullptr));
}

TEST(EqOp, MatchesThroughNestedArray) {
    BSONObj operand = BSON("a.b.c.d" << 3);
    EqualityMatchExpression eq("a.b.c.d"_sd, operand["a.b.c.d"]);
    BSONObj obj = fromjson("{a:{b:[{c:[{d:1},{d:2}]},{c:[{d:3}]}]}}");
    ASSERT(exec::matcher::matchesBSON(&eq, obj, nullptr));
}

TEST(EqOp, ElemMatchKey) {
    BSONObj operand = BSON("a" << 5);
    EqualityMatchExpression eq("a"_sd, operand["a"]);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&eq, BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << 5), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&eq, BSON("a" << BSON_ARRAY(1 << 2 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("2", details.elemMatchKey());
}

// SERVER-14886: when an array is being traversed explictly at the same time that a nested array
// is being traversed implicitly, the elemMatch key should refer to the offset of the array
// being implicitly traversed.
TEST(EqOp, ElemMatchKeyWithImplicitAndExplicitTraversal) {
    BSONObj operand = BSON("a.0.b" << 3);
    BSONElement operandFirstElt = operand.firstElement();
    EqualityMatchExpression eq(operandFirstElt.fieldNameStringData(), operandFirstElt);
    MatchDetails details;
    details.requestElemMatchKey();
    BSONObj obj = fromjson("{a: [{b: [2, 3]}, {b: [4, 5]}]}");
    ASSERT(exec::matcher::matchesBSON(&eq, obj, &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(LtOp, MatchesElement) {
    BSONObj operand = BSON("$lt" << 5);
    BSONObj match = BSON("a" << 4.5);
    BSONObj notMatch = BSON("a" << 6);
    BSONObj notMatchEqual = BSON("a" << 5);
    BSONObj notMatchWrongType = BSON("a" << "foo");
    LTMatchExpression lt(""_sd, operand["$lt"]);
    ASSERT(exec::matcher::matchesSingleElement(&lt, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lt, notMatch.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lt, notMatchEqual.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lt, notMatchWrongType.firstElement()));
}

TEST(LtOp, MatchesScalar) {
    BSONObj operand = BSON("$lt" << 5);
    LTMatchExpression lt("a"_sd, operand["$lt"]);
    ASSERT(exec::matcher::matchesBSON(&lt, BSON("a" << 4.5), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << 6), nullptr));
}

TEST(LtOp, MatchesScalarEmptyKey) {
    BSONObj operand = BSON("$lt" << 5);
    LTMatchExpression lt(""_sd, operand["$lt"]);
    ASSERT(exec::matcher::matchesBSON(&lt, BSON("" << 4.5), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("" << 6), nullptr));
}

TEST(LtOp, MatchesArrayValue) {
    BSONObj operand = BSON("$lt" << 5);
    LTMatchExpression lt("a"_sd, operand["$lt"]);
    ASSERT(exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(6 << 4.5)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(6 << 7)), nullptr));
}

TEST(LtOp, MatchesWholeArray) {
    BSONObj operand = BSON("$lt" << BSON_ARRAY(5));
    LTMatchExpression lt("a"_sd, operand["$lt"]);
    ASSERT(exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(4)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(5)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(6)), nullptr));
    // Nested array.
    ASSERT(exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(BSON_ARRAY(6))), nullptr));
}

TEST(LtOp, MatchesNull) {
    BSONObj operand = BSON("$lt" << BSONNULL);
    LTMatchExpression lt("a"_sd, operand["$lt"]);
    ASSERT(!exec::matcher::matchesBSON(&lt, BSONObj(), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << BSONNULL), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << 4), nullptr));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("b" << 4), nullptr));
}

TEST(LtOp, MatchesDotNotationNull) {
    BSONObj operand = BSON("$lt" << BSONNULL);
    LTMatchExpression lt("a.b"_sd, operand["$lt"]);
    ASSERT(!exec::matcher::matchesBSON(&lt, BSONObj(), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << BSONNULL), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << 4), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << BSONObj()), nullptr));
    ASSERT(
        !exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(BSON("b" << BSONNULL))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(
        &lt, BSON("a" << BSON_ARRAY(BSON("a" << 4) << BSON("b" << 4))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(4)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(BSON("b" << 4))), nullptr));
}

TEST(LtOp, MatchesMinKey) {
    BSONObj operand = BSON("a" << BSONType::minKey);
    LTMatchExpression lt("a"_sd, operand["a"]);
    BSONObj minKeyObj = BSON("a" << BSONType::minKey);
    BSONObj maxKeyObj = BSON("a" << BSONType::maxKey);
    BSONObj numObj = BSON("a" << 4);

    ASSERT(!exec::matcher::matchesBSON(&lt, minKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, maxKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, numObj, nullptr));

    ASSERT(!exec::matcher::matchesSingleElement(&lt, minKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lt, maxKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lt, numObj.firstElement()));
}

TEST(LtOp, MatchesMaxKey) {
    BSONObj operand = BSON("a" << BSONType::maxKey);
    LTMatchExpression lt("a"_sd, operand["a"]);
    BSONObj minKeyObj = BSON("a" << BSONType::minKey);
    BSONObj maxKeyObj = BSON("a" << BSONType::maxKey);
    BSONObj numObj = BSON("a" << 4);

    ASSERT(exec::matcher::matchesBSON(&lt, minKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lt, maxKeyObj, nullptr));
    ASSERT(exec::matcher::matchesBSON(&lt, numObj, nullptr));

    ASSERT(exec::matcher::matchesSingleElement(&lt, minKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lt, maxKeyObj.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&lt, numObj.firstElement()));
}

TEST(LtOp, ElemMatchKey) {
    BSONObj operand = BSON("$lt" << 5);
    LTMatchExpression lt("a"_sd, operand["$lt"]);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&lt, BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&lt, BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&lt, BSON("a" << BSON_ARRAY(6 << 2 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(LteOp, MatchesElement) {
    BSONObj operand = BSON("$lte" << 5);
    BSONObj match = BSON("a" << 4.5);
    BSONObj equalMatch = BSON("a" << 5);
    BSONObj notMatch = BSON("a" << 6);
    BSONObj notMatchWrongType = BSON("a" << "foo");
    LTEMatchExpression lte(""_sd, operand["$lte"]);
    ASSERT(exec::matcher::matchesSingleElement(&lte, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&lte, equalMatch.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lte, notMatch.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lte, notMatchWrongType.firstElement()));
}

TEST(LteOp, MatchesScalar) {
    BSONObj operand = BSON("$lte" << 5);
    LTEMatchExpression lte("a"_sd, operand["$lte"]);
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << 4.5), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lte, BSON("a" << 6), nullptr));
}

TEST(LteOp, MatchesArrayValue) {
    BSONObj operand = BSON("$lte" << 5);
    LTEMatchExpression lte("a"_sd, operand["$lte"]);
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(6 << 4.5)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(6 << 7)), nullptr));
}

TEST(LteOp, MatchesWholeArray) {
    BSONObj operand = BSON("$lte" << BSON_ARRAY(5));
    LTEMatchExpression lte("a"_sd, operand["$lte"]);
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(4)), nullptr));
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(5)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(6)), nullptr));
    // Nested array.
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), nullptr));
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(BSON_ARRAY(6))), nullptr));
}

TEST(LteOp, MatchesNull) {
    BSONObj operand = BSON("$lte" << BSONNULL);
    LTEMatchExpression lte("a"_sd, operand["$lte"]);
    ASSERT(exec::matcher::matchesBSON(&lte, BSONObj(), nullptr));
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << BSONNULL), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lte, BSON("a" << 4), nullptr));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("b" << 4), nullptr));
}

TEST(LteOp, MatchesDotNotationNull) {
    BSONObj operand = BSON("$lte" << BSONNULL);
    LTEMatchExpression lte("a.b"_sd, operand["$lte"]);
    ASSERT(exec::matcher::matchesBSON(&lte, BSONObj(), nullptr));
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << BSONNULL), nullptr));
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << 4), nullptr));
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << BSONObj()), nullptr));
    ASSERT(
        exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(BSON("b" << BSONNULL))), nullptr));
    ASSERT(exec::matcher::matchesBSON(
        &lte, BSON("a" << BSON_ARRAY(BSON("a" << 4) << BSON("b" << 4))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(4)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(BSON("b" << 4))), nullptr));
}

TEST(LteOp, MatchesMinKey) {
    BSONObj operand = BSON("a" << BSONType::minKey);
    LTEMatchExpression lte("a"_sd, operand["a"]);
    BSONObj minKeyObj = BSON("a" << BSONType::minKey);
    BSONObj maxKeyObj = BSON("a" << BSONType::maxKey);
    BSONObj numObj = BSON("a" << 4);

    ASSERT(exec::matcher::matchesBSON(&lte, minKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lte, maxKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&lte, numObj, nullptr));

    ASSERT(exec::matcher::matchesSingleElement(&lte, minKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lte, maxKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&lte, numObj.firstElement()));
}

TEST(LteOp, MatchesMaxKey) {
    BSONObj operand = BSON("a" << BSONType::maxKey);
    LTEMatchExpression lte("a"_sd, operand["a"]);
    BSONObj minKeyObj = BSON("a" << BSONType::minKey);
    BSONObj maxKeyObj = BSON("a" << BSONType::maxKey);
    BSONObj numObj = BSON("a" << 4);

    ASSERT(exec::matcher::matchesBSON(&lte, minKeyObj, nullptr));
    ASSERT(exec::matcher::matchesBSON(&lte, maxKeyObj, nullptr));
    ASSERT(exec::matcher::matchesBSON(&lte, numObj, nullptr));

    ASSERT(exec::matcher::matchesSingleElement(&lte, minKeyObj.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&lte, maxKeyObj.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&lte, numObj.firstElement()));
}

TEST(LteOp, ElemMatchKey) {
    BSONObj operand = BSON("$lte" << 5);
    LTEMatchExpression lte("a"_sd, operand["$lte"]);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&lte, BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&lte, BSON("a" << BSON_ARRAY(6 << 2 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(GtOp, MatchesScalar) {
    BSONObj operand = BSON("$gt" << 5);
    GTMatchExpression gt("a"_sd, operand["$gt"]);
    ASSERT(exec::matcher::matchesBSON(&gt, BSON("a" << 5.5), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << 4), nullptr));
}

TEST(GtOp, MatchesArrayValue) {
    BSONObj operand = BSON("$gt" << 5);
    GTMatchExpression gt("a"_sd, operand["$gt"]);
    ASSERT(exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(3 << 5.5)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(2 << 4)), nullptr));
}

TEST(GtOp, MatchesWholeArray) {
    BSONObj operand = BSON("$gt" << BSON_ARRAY(5));
    GTMatchExpression gt("a"_sd, operand["$gt"]);
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(4)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(5)), nullptr));
    ASSERT(exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(6)), nullptr));
    // Nested array.
    // XXX: The following assertion documents current behavior.
    ASSERT(exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), nullptr));
    // XXX: The following assertion documents current behavior.
    ASSERT(exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), nullptr));
    ASSERT(exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(BSON_ARRAY(6))), nullptr));
}

TEST(GtOp, MatchesNull) {
    BSONObj operand = BSON("$gt" << BSONNULL);
    GTMatchExpression gt("a"_sd, operand["$gt"]);
    ASSERT(!exec::matcher::matchesBSON(&gt, BSONObj(), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << BSONNULL), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << 4), nullptr));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("b" << 4), nullptr));
}

TEST(GtOp, MatchesDotNotationNull) {
    BSONObj operand = BSON("$gt" << BSONNULL);
    GTMatchExpression gt("a.b"_sd, operand["$gt"]);
    ASSERT(!exec::matcher::matchesBSON(&gt, BSONObj(), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << BSONNULL), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << 4), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << BSONObj()), nullptr));
    ASSERT(
        !exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(BSON("b" << BSONNULL))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(
        &gt, BSON("a" << BSON_ARRAY(BSON("a" << 4) << BSON("b" << 4))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(4)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(BSON("b" << 4))), nullptr));
}

TEST(GtOp, MatchesMinKey) {
    BSONObj operand = BSON("a" << BSONType::minKey);
    GTMatchExpression gt("a"_sd, operand["a"]);
    BSONObj minKeyObj = BSON("a" << BSONType::minKey);
    BSONObj maxKeyObj = BSON("a" << BSONType::maxKey);
    BSONObj numObj = BSON("a" << 4);

    ASSERT(!exec::matcher::matchesBSON(&gt, minKeyObj, nullptr));
    ASSERT(exec::matcher::matchesBSON(&gt, maxKeyObj, nullptr));
    ASSERT(exec::matcher::matchesBSON(&gt, numObj, nullptr));

    ASSERT(!exec::matcher::matchesSingleElement(&gt, minKeyObj.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&gt, maxKeyObj.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&gt, numObj.firstElement()));
}

TEST(GtOp, MatchesMaxKey) {
    BSONObj operand = BSON("a" << BSONType::maxKey);
    GTMatchExpression gt("a"_sd, operand["a"]);
    BSONObj minKeyObj = BSON("a" << BSONType::minKey);
    BSONObj maxKeyObj = BSON("a" << BSONType::maxKey);
    BSONObj numObj = BSON("a" << 4);

    ASSERT(!exec::matcher::matchesBSON(&gt, minKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, maxKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gt, numObj, nullptr));

    ASSERT(!exec::matcher::matchesSingleElement(&gt, minKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&gt, maxKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&gt, numObj.firstElement()));
}

TEST(GtOp, ElemMatchKey) {
    BSONObj operand = BSON("$gt" << 5);
    GTMatchExpression gt("a"_sd, operand["$gt"]);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&gt, BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&gt, BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&gt, BSON("a" << BSON_ARRAY(2 << 6 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(GteOp, MatchesElement) {
    BSONObj operand = BSON("$gte" << 5);
    BSONObj match = BSON("a" << 5.5);
    BSONObj equalMatch = BSON("a" << 5);
    BSONObj notMatch = BSON("a" << 4);
    BSONObj notMatchWrongType = BSON("a" << "foo");
    GTEMatchExpression gte(""_sd, operand["$gte"]);
    ASSERT(exec::matcher::matchesSingleElement(&gte, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&gte, equalMatch.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&gte, notMatch.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&gte, notMatchWrongType.firstElement()));
}

TEST(GteOp, MatchesScalar) {
    BSONObj operand = BSON("$gte" << 5);
    GTEMatchExpression gte("a"_sd, operand["$gte"]);
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << 5.5), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gte, BSON("a" << 4), nullptr));
}

TEST(GteOp, MatchesArrayValue) {
    BSONObj operand = BSON("$gte" << 5);
    GTEMatchExpression gte("a"_sd, operand["$gte"]);
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(4 << 5.5)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(1 << 2)), nullptr));
}

TEST(GteOp, MatchesWholeArray) {
    BSONObj operand = BSON("$gte" << BSON_ARRAY(5));
    GTEMatchExpression gte("a"_sd, operand["$gte"]);
    ASSERT(!exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(4)), nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(5)), nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(6)), nullptr));
    // Nested array.
    // XXX: The following assertion documents current behavior.
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(BSON_ARRAY(6))), nullptr));
}

TEST(GteOp, MatchesNull) {
    BSONObj operand = BSON("$gte" << BSONNULL);
    GTEMatchExpression gte("a"_sd, operand["$gte"]);
    ASSERT(exec::matcher::matchesBSON(&gte, BSONObj(), nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << BSONNULL), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gte, BSON("a" << 4), nullptr));
    // A non-existent field is treated same way as an empty bson object
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("b" << 4), nullptr));
}

TEST(GteOp, MatchesDotNotationNull) {
    BSONObj operand = BSON("$gte" << BSONNULL);
    GTEMatchExpression gte("a.b"_sd, operand["$gte"]);
    ASSERT(exec::matcher::matchesBSON(&gte, BSONObj(), nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << BSONNULL), nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << 4), nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << BSONObj()), nullptr));
    ASSERT(
        exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(BSON("b" << BSONNULL))), nullptr));
    ASSERT(exec::matcher::matchesBSON(
        &gte, BSON("a" << BSON_ARRAY(BSON("a" << 4) << BSON("b" << 4))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(4)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(BSON("b" << 4))), nullptr));
}

TEST(GteOp, MatchesMinKey) {
    BSONObj operand = BSON("a" << BSONType::minKey);
    GTEMatchExpression gte("a"_sd, operand["a"]);
    BSONObj minKeyObj = BSON("a" << BSONType::minKey);
    BSONObj maxKeyObj = BSON("a" << BSONType::maxKey);
    BSONObj numObj = BSON("a" << 4);

    ASSERT(exec::matcher::matchesBSON(&gte, minKeyObj, nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, maxKeyObj, nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, numObj, nullptr));

    ASSERT(exec::matcher::matchesSingleElement(&gte, minKeyObj.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&gte, maxKeyObj.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&gte, numObj.firstElement()));
}

TEST(GteOp, MatchesMaxKey) {
    BSONObj operand = BSON("a" << BSONType::maxKey);
    GTEMatchExpression gte("a"_sd, operand["a"]);
    BSONObj minKeyObj = BSON("a" << BSONType::minKey);
    BSONObj maxKeyObj = BSON("a" << BSONType::maxKey);
    BSONObj numObj = BSON("a" << 4);

    ASSERT(!exec::matcher::matchesBSON(&gte, minKeyObj, nullptr));
    ASSERT(exec::matcher::matchesBSON(&gte, maxKeyObj, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&gte, numObj, nullptr));

    ASSERT(!exec::matcher::matchesSingleElement(&gte, minKeyObj.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&gte, maxKeyObj.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&gte, numObj.firstElement()));
}

TEST(GteOp, ElemMatchKey) {
    BSONObj operand = BSON("$gte" << 5);
    GTEMatchExpression gte("a"_sd, operand["$gte"]);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&gte, BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&gte, BSON("a" << BSON_ARRAY(2 << 6 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(RegexMatchExpression, MatchesElementExact) {
    BSONObj match = BSON("a" << "b");
    BSONObj notMatch = BSON("a" << "c");
    RegexMatchExpression regex(""_sd, "b", "");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementSimplePrefix) {
    BSONObj match = BSON("x" << "abc");
    BSONObj notMatch = BSON("x" << "adz");
    RegexMatchExpression regex(""_sd, "^ab", "");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementCaseSensitive) {
    BSONObj match = BSON("x" << "abc");
    BSONObj notMatch = BSON("x" << "ABC");
    RegexMatchExpression regex(""_sd, "abc", "");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementCaseInsensitive) {
    BSONObj match = BSON("x" << "abc");
    BSONObj matchUppercase = BSON("x" << "ABC");
    BSONObj notMatch = BSON("x" << "abz");
    RegexMatchExpression regex(""_sd, "abc", "i");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&regex, matchUppercase.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementMultilineOff) {
    BSONObj match = BSON("x" << "az");
    BSONObj notMatch = BSON("x" << "\naz");
    RegexMatchExpression regex(""_sd, "^a", "");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementMultilineOn) {
    BSONObj match = BSON("x" << "az");
    BSONObj matchMultiline = BSON("x" << "\naz");
    BSONObj notMatch = BSON("x" << "\n\n");
    RegexMatchExpression regex(""_sd, "^a", "m");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&regex, matchMultiline.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementExtendedOff) {
    BSONObj match = BSON("x" << "a b");
    BSONObj notMatch = BSON("x" << "ab");
    RegexMatchExpression regex(""_sd, "a b", "");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementExtendedOn) {
    BSONObj match = BSON("x" << "ab");
    BSONObj notMatch = BSON("x" << "a b");
    RegexMatchExpression regex(""_sd, "a b", "x");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementDotAllOff) {
    BSONObj match = BSON("x" << "a b");
    BSONObj notMatch = BSON("x" << "a\nb");
    RegexMatchExpression regex(""_sd, "a.b", "");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementDotAllOn) {
    BSONObj match = BSON("x" << "a b");
    BSONObj matchDotAll = BSON("x" << "a\nb");
    BSONObj notMatch = BSON("x" << "ab");
    RegexMatchExpression regex(""_sd, "a.b", "s");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&regex, matchDotAll.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementMultipleFlags) {
    BSONObj matchMultilineDotAll = BSON("x" << "\na\nb");
    RegexMatchExpression regex(""_sd, "^a.b", "ms");
    ASSERT(exec::matcher::matchesSingleElement(&regex, matchMultilineDotAll.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementRegexType) {
    BSONObj match = BSONObjBuilder().appendRegex("x", "yz", "i").obj();
    BSONObj notMatchPattern = BSONObjBuilder().appendRegex("x", "r", "i").obj();
    BSONObj notMatchFlags = BSONObjBuilder().appendRegex("x", "yz", "s").obj();
    RegexMatchExpression regex(""_sd, "yz", "i");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatchPattern.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatchFlags.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementSymbolType) {
    BSONObj match = BSONObjBuilder().appendSymbol("x", "yz").obj();
    BSONObj notMatch = BSONObjBuilder().appendSymbol("x", "gg").obj();
    RegexMatchExpression regex(""_sd, "yz", "");
    ASSERT(exec::matcher::matchesSingleElement(&regex, match.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatch.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementWrongType) {
    BSONObj notMatchInt = BSON("x" << 1);
    BSONObj notMatchBool = BSON("x" << true);
    RegexMatchExpression regex(""_sd, "1", "");
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatchInt.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&regex, notMatchBool.firstElement()));
}

TEST(RegexMatchExpression, MatchesElementUtf8) {
    BSONObj multiByteCharacter = BSON("x" << "\xc2\xa5");
    RegexMatchExpression regex(""_sd, "^.$", "");
    ASSERT(exec::matcher::matchesSingleElement(&regex, multiByteCharacter.firstElement()));
}

TEST(RegexMatchExpression, MatchesScalar) {
    RegexMatchExpression regex("a"_sd, "b", "");
    ASSERT(exec::matcher::matchesBSON(&regex, BSON("a" << "b"), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&regex, BSON("a" << "c"), nullptr));
}

TEST(RegexMatchExpression, MatchesArrayValue) {
    RegexMatchExpression regex("a"_sd, "b", "");
    ASSERT(exec::matcher::matchesBSON(&regex, BSON("a" << BSON_ARRAY("c" << "b")), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&regex, BSON("a" << BSON_ARRAY("d" << "c")), nullptr));
}

TEST(RegexMatchExpression, MatchesNull) {
    RegexMatchExpression regex("a"_sd, "b", "");
    ASSERT(!exec::matcher::matchesBSON(&regex, BSONObj(), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&regex, BSON("a" << BSONNULL), nullptr));
}

TEST(RegexMatchExpression, ElemMatchKey) {
    RegexMatchExpression regex("a"_sd, "b", "");
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&regex, BSON("a" << "c"), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&regex, BSON("a" << "b"), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&regex, BSON("a" << BSON_ARRAY("c" << "b")), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(RegexMatchExpression, RegexAcceptsUCPStartOption) {
    RegexMatchExpression regex("a"_sd, "(*UCP)(\\w|\u304C)", "");
    ASSERT(exec::matcher::matchesBSON(&regex, BSON("a" << "k")));
    ASSERT(exec::matcher::matchesBSON(&regex, BSON("a" << "\u304B")));
    ASSERT(exec::matcher::matchesBSON(&regex, BSON("a" << "\u304C")));
}

TEST(RegexMatchExpression, RegexAcceptsLFOption) {
    // The LF option tells the regex to only treat \n as a newline. "." will not match newlines (by
    // default) so a\nb will not match, but a\rb will.
    RegexMatchExpression regexLF("a"_sd, "(*LF)a.b", "");
    ASSERT(!exec::matcher::matchesBSON(&regexLF, BSON("a" << "a\nb")));
    ASSERT(exec::matcher::matchesBSON(&regexLF, BSON("a" << "a\rb")));

    RegexMatchExpression regexCR("a"_sd, "(*CR)a.b", "");
    ASSERT(exec::matcher::matchesBSON(&regexCR, BSON("a" << "a\nb")));
    ASSERT(!exec::matcher::matchesBSON(&regexCR, BSON("a" << "a\rb")));
}

TEST(ModMatchExpression, MatchesElement) {
    BSONObj match = BSON("a" << 1);
    BSONObj largerMatch = BSON("a" << 4.0);
    BSONObj longLongMatch = BSON("a" << 68719476736LL);
    BSONObj notMatch = BSON("a" << 6);
    BSONObj negativeNotMatch = BSON("a" << -2);
    ModMatchExpression mod(""_sd, 3, 1);
    ASSERT(exec::matcher::matchesSingleElement(&mod, match.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&mod, largerMatch.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&mod, longLongMatch.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&mod, notMatch.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&mod, negativeNotMatch.firstElement()));
}

TEST(ModMatchExpression, MatchesScalar) {
    ModMatchExpression mod("a"_sd, 5, 2);
    ASSERT(exec::matcher::matchesBSON(&mod, BSON("a" << 7.0), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&mod, BSON("a" << 4), nullptr));
}

TEST(ModMatchExpression, MatchesArrayValue) {
    ModMatchExpression mod("a"_sd, 5, 2);
    ASSERT(exec::matcher::matchesBSON(&mod, BSON("a" << BSON_ARRAY(5 << 12LL)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&mod, BSON("a" << BSON_ARRAY(6 << 8)), nullptr));
}

TEST(ModMatchExpression, MatchesNull) {
    ModMatchExpression mod("a"_sd, 5, 2);
    ASSERT(!exec::matcher::matchesBSON(&mod, BSONObj(), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&mod, BSON("a" << BSONNULL), nullptr));
}

TEST(ModMatchExpression, ElemMatchKey) {
    ModMatchExpression mod("a"_sd, 5, 2);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&mod, BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&mod, BSON("a" << 2), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&mod, BSON("a" << BSON_ARRAY(1 << 2 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(ExistsMatchExpression, MatchesElement) {
    BSONObj existsInt = BSON("a" << 5);
    BSONObj existsNull = BSON("a" << BSONNULL);
    BSONObj doesntExist = BSONObj();
    ExistsMatchExpression exists(""_sd);
    ASSERT(exec::matcher::matchesSingleElement(&exists, existsInt.firstElement()));
    ASSERT(exec::matcher::matchesSingleElement(&exists, existsNull.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&exists, doesntExist.firstElement()));
}

TEST(ExistsMatchExpression, MatchesElementExistsTrueValue) {
    BSONObj exists = BSON("a" << 5);
    BSONObj missing = BSONObj();
    ExistsMatchExpression existsTrueValue(""_sd);
    ASSERT(exec::matcher::matchesSingleElement(&existsTrueValue, exists.firstElement()));
    ASSERT(!exec::matcher::matchesSingleElement(&existsTrueValue, missing.firstElement()));
}

TEST(ExistsMatchExpression, MatchesScalar) {
    ExistsMatchExpression exists("a"_sd);
    ASSERT(exec::matcher::matchesBSON(&exists, BSON("a" << 1), nullptr));
    ASSERT(exec::matcher::matchesBSON(&exists, BSON("a" << BSONNULL), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&exists, BSON("b" << 1), nullptr));
}

TEST(ExistsMatchExpression, MatchesArray) {
    ExistsMatchExpression exists("a"_sd);
    ASSERT(exec::matcher::matchesBSON(&exists, BSON("a" << BSON_ARRAY(4 << 5.5)), nullptr));
}

TEST(ExistsMatchExpression, ElemMatchKey) {
    ExistsMatchExpression exists("a.b"_sd);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&exists, BSON("a" << 1), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&exists, BSON("a" << BSON("b" << 6)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &exists, BSON("a" << BSON_ARRAY(2 << BSON("b" << 7))), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(InMatchExpression, MatchesElementSingle) {
    BSONArray operand = BSON_ARRAY(1);
    BSONObj match = BSON("a" << 1);
    BSONObj notMatch = BSON("a" << 2);
    InMatchExpression in(""_sd);
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));
    ASSERT(exec::matcher::matchesSingleElement(&in, match["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&in, notMatch["a"]));
}

TEST(InMatchExpression, MatchesEmpty) {
    InMatchExpression in("a"_sd);

    BSONObj notMatch = BSON("a" << 2);
    ASSERT(!exec::matcher::matchesSingleElement(&in, notMatch["a"]));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << 1), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSONObj(), nullptr));
}

TEST(InMatchExpression, MatchesElementMultiple) {
    BSONObj operand = BSON_ARRAY(1 << "r" << true << 1);
    InMatchExpression in(""_sd);
    std::vector<BSONElement> equalities{operand[0], operand[1], operand[2], operand[3]};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    BSONObj matchFirst = BSON("a" << 1);
    BSONObj matchSecond = BSON("a" << "r");
    BSONObj matchThird = BSON("a" << true);
    BSONObj notMatch = BSON("a" << false);
    ASSERT(exec::matcher::matchesSingleElement(&in, matchFirst["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&in, matchSecond["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&in, matchThird["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&in, notMatch["a"]));
}


TEST(InMatchExpression, MatchesScalar) {
    BSONObj operand = BSON_ARRAY(5);
    InMatchExpression in("a"_sd);
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(exec::matcher::matchesBSON(&in, BSON("a" << 5.0), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << 4), nullptr));
}

TEST(InMatchExpression, MatchesArrayValue) {
    BSONObj operand = BSON_ARRAY(5);
    InMatchExpression in("a"_sd);
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(exec::matcher::matchesBSON(&in, BSON("a" << BSON_ARRAY(5.0 << 6)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << BSON_ARRAY(6 << 7)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << BSON_ARRAY(BSON_ARRAY(5))), nullptr));
}

TEST(InMatchExpression, MatchesNull) {
    BSONObj operand = BSON_ARRAY(BSONNULL);

    InMatchExpression in("a"_sd);
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT_TRUE(exec::matcher::matchesBSON(&in, BSONObj(), nullptr));
    ASSERT_TRUE(exec::matcher::matchesBSON(&in, BSON("a" << BSONNULL), nullptr));
    ASSERT_FALSE(exec::matcher::matchesBSON(&in, BSON("a" << 4), nullptr));

    // When null appears inside an $in, it has the same special semantics as an {$eq:null}
    // predicate. In particular, we expect it to match missing and not undefined.
    ASSERT_TRUE(exec::matcher::matchesBSON(&in, BSON("b" << 4), nullptr));
    ASSERT_FALSE(exec::matcher::matchesBSON(&in, BSON("a" << BSONUndefined), nullptr));
}

TEST(InMatchExpression, MatchesMinKey) {
    BSONObj operand = BSON_ARRAY(BSONType::minKey);
    InMatchExpression in("a"_sd);
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(exec::matcher::matchesBSON(&in, BSON("a" << BSONType::minKey), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << BSONType::maxKey), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << 4), nullptr));
}

TEST(InMatchExpression, MatchesMaxKey) {
    BSONObj operand = BSON_ARRAY(BSONType::maxKey);
    InMatchExpression in("a"_sd);
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(exec::matcher::matchesBSON(&in, BSON("a" << BSONType::maxKey), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << BSONType::minKey), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << 4), nullptr));
}

TEST(InMatchExpression, MatchesFullArray) {
    BSONObj operand = BSON_ARRAY(BSON_ARRAY(1 << 2) << 4 << 5);
    InMatchExpression in("a"_sd);
    std::vector<BSONElement> equalities{operand[0], operand[1], operand[2]};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    ASSERT(exec::matcher::matchesBSON(&in, BSON("a" << BSON_ARRAY(1 << 2)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << BSON_ARRAY(1 << 2 << 3)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << BSON_ARRAY(1)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << 1), nullptr));
}

TEST(InMatchExpression, ElemMatchKey) {
    BSONObj operand = BSON_ARRAY(5 << 2);
    InMatchExpression in("a"_sd);
    std::vector<BSONElement> equalities{operand[0], operand[1]};
    ASSERT_OK(in.setEqualities(std::move(equalities)));

    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&in, BSON("a" << 4), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&in, BSON("a" << 5), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&in, BSON("a" << BSON_ARRAY(1 << 2 << 5)), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(InMatchExpression, StringMatchingWithNullCollatorUsesBinaryComparison) {
    BSONArray operand = BSON_ARRAY("string");
    BSONObj notMatch = BSON("a" << "string2");
    InMatchExpression in(""_sd);
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));
    ASSERT(!exec::matcher::matchesSingleElement(&in, notMatch["a"]));
}

TEST(InMatchExpression, StringMatchingRespectsCollation) {
    BSONArray operand = BSON_ARRAY("string");
    BSONObj match = BSON("a" << "string2");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression in(""_sd);
    in.setCollator(&collator);
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));
    ASSERT(exec::matcher::matchesSingleElement(&in, match["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchOther) {
    std::vector<uint32_t> bitPositions;

    BSONObj notMatch1 = fromjson("{a: {}}");     // Object
    BSONObj notMatch2 = fromjson("{a: null}");   // Null
    BSONObj notMatch3 = fromjson("{a: []}");     // Array
    BSONObj notMatch4 = fromjson("{a: true}");   // Boolean
    BSONObj notMatch5 = fromjson("{a: ''}");     // String
    BSONObj notMatch6 = fromjson("{a: 5.5}");    // Non-integral Double
    BSONObj notMatch7 = fromjson("{a: NaN}");    // NaN
    BSONObj notMatch8 = fromjson("{a: 1e100}");  // Too-Large Double
    BSONObj notMatch9 = fromjson("{a: ObjectId('000000000000000000000000')}");  // OID
    BSONObj notMatch10 = fromjson("{a: Date(54)}");                             // Date
    BSONObj notMatch11 = fromjson("{a: NumberDecimal(\"Infinity\")}");  // Infinity NumberDecimal
    BSONObj notMatch12 =
        fromjson("{a: NumberDecimal(\"-Infinity\")}");  // Negative infinity NumberDecimal
    BSONObj notMatch13 = fromjson("{a: NumberDecimal(\"NaN\")}");     // NaN NumberDecimal
    BSONObj notMatch14 = fromjson("{a: NumberDecimal(\"1e100\")}");   // Too-Large NumberDecimal
    BSONObj notMatch15 = fromjson("{a: NumberDecimal(\"-1e100\")}");  // Too-Small NumberDecimal
    BSONObj notMatch16 = fromjson("{a: NumberDecimal(\"5.5\")}");     // Non-integral NumberDecimal
    BSONObj notMatch17 =
        fromjson("{a: NumberDecimal(\"-5.5\")}");  // Negative-integral NumberDecimal

    BitsAllSetMatchExpression balls("a"_sd, bitPositions);
    BitsAllClearMatchExpression ballc("a"_sd, bitPositions);
    BitsAnySetMatchExpression banys("a"_sd, bitPositions);
    BitsAnyClearMatchExpression banyc("a"_sd, bitPositions);

    ASSERT_EQ((size_t)0, balls.numBitPositions());
    ASSERT_EQ((size_t)0, ballc.numBitPositions());
    ASSERT_EQ((size_t)0, banys.numBitPositions());
    ASSERT_EQ((size_t)0, banyc.numBitPositions());
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch3["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch4["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch5["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch6["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch7["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch8["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch9["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch10["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch11["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch12["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch13["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch14["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch15["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch16["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, notMatch17["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch3["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch4["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch5["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch6["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch7["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch8["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch9["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch10["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch11["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch12["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch13["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch14["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch15["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch16["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, notMatch17["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch3["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch4["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch5["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch6["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch7["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch8["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch9["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch10["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch11["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch12["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch13["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch14["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch15["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch16["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, notMatch17["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch3["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch4["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch5["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch6["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch7["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch8["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch9["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch10["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch11["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch12["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch13["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch14["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch15["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch16["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, notMatch17["a"]));
}

TEST(BitTestMatchExpression, MatchBinaryWithLongBitMask) {
    long long bitMask = 54;

    BSONObj match = fromjson("{a: {$binary: 'NgAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");

    BitsAllSetMatchExpression balls("a"_sd, bitMask);
    BitsAllClearMatchExpression ballc("a"_sd, bitMask);
    BitsAnySetMatchExpression banys("a"_sd, bitMask);
    BitsAnyClearMatchExpression banyc("a"_sd, bitMask);

    std::vector<uint32_t> bitPositions = balls.getBitPositions();
    ASSERT(exec::matcher::matchesSingleElement(&balls, match["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, match["a"]));
}

TEST(BitTestMatchExpression, MatchLongWithBinaryBitMask) {
    const char* bitMaskSet = "\x36\x00\x00\x00";
    const char* bitMaskClear = "\xC9\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF";

    BSONObj match = fromjson("{a: 54}");

    BitsAllSetMatchExpression balls("a"_sd, bitMaskSet, 4);
    BitsAllClearMatchExpression ballc("a"_sd, bitMaskClear, 9);
    BitsAnySetMatchExpression banys("a"_sd, bitMaskSet, 4);
    BitsAnyClearMatchExpression banyc("a"_sd, bitMaskClear, 9);

    ASSERT(exec::matcher::matchesSingleElement(&balls, match["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match["a"]));
}

TEST(BitTestMatchExpression, MatchesEmpty) {
    std::vector<uint32_t> bitPositions;

    BSONObj match1 = fromjson("{a: NumberInt(54)}");
    BSONObj match2 = fromjson("{a: NumberLong(54)}");
    BSONObj match3 = fromjson("{a: 54.0}");
    BSONObj match4 = fromjson("{a: {$binary: '2AAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");

    BitsAllSetMatchExpression balls("a"_sd, bitPositions);
    BitsAllClearMatchExpression ballc("a"_sd, bitPositions);
    BitsAnySetMatchExpression banys("a"_sd, bitPositions);
    BitsAnyClearMatchExpression banyc("a"_sd, bitPositions);

    ASSERT_EQ((size_t)0, balls.numBitPositions());
    ASSERT_EQ((size_t)0, ballc.numBitPositions());
    ASSERT_EQ((size_t)0, banys.numBitPositions());
    ASSERT_EQ((size_t)0, banyc.numBitPositions());
    ASSERT(exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match4["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match4["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, match3["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banys, match4["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, match2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, match3["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&banyc, match4["a"]));
}

std::vector<uint32_t> bsonArrayToBitPositions(const BSONArray& ba) {
    std::vector<uint32_t> bitPositions;

    // Convert BSONArray of bit positions to int vector
    for (const auto& elt : ba) {
        bitPositions.push_back(elt._numberInt());
    }

    return bitPositions;
}

TEST(BitTestMatchExpression, MatchesInteger) {
    BSONArray bas = BSON_ARRAY(1 << 2 << 4 << 5);
    BSONArray bac = BSON_ARRAY(0 << 3 << 600);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: NumberInt(54)}");
    BSONObj match2 = fromjson("{a: NumberLong(54)}");
    BSONObj match3 = fromjson("{a: 54.0}");

    BitsAllSetMatchExpression balls("a"_sd, bitPositionsSet);
    BitsAllClearMatchExpression ballc("a"_sd, bitPositionsClear);
    BitsAnySetMatchExpression banys("a"_sd, bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a"_sd, bitPositionsClear);

    ASSERT_EQ((size_t)4, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)4, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match3["a"]));
}

TEST(BitTestMatchExpression, MatchesNegativeInteger) {
    BSONArray bas = BSON_ARRAY(1 << 3 << 6 << 7 << 33);
    BSONArray bac = BSON_ARRAY(0 << 2 << 4 << 5);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: NumberInt(-54)}");
    BSONObj match2 = fromjson("{a: NumberLong(-54)}");
    BSONObj match3 = fromjson("{a: -54.0}");

    BitsAllSetMatchExpression balls("a"_sd, bitPositionsSet);
    BitsAllClearMatchExpression ballc("a"_sd, bitPositionsClear);
    BitsAnySetMatchExpression banys("a"_sd, bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a"_sd, bitPositionsClear);

    ASSERT_EQ((size_t)5, balls.numBitPositions());
    ASSERT_EQ((size_t)4, ballc.numBitPositions());
    ASSERT_EQ((size_t)5, banys.numBitPositions());
    ASSERT_EQ((size_t)4, banyc.numBitPositions());
    ASSERT(exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match3["a"]));
}

TEST(BitTestMatchExpression, MatchesIntegerWithBitMask) {
    long long bitMaskSet = 54;
    long long bitMaskClear = 201;

    BSONObj match1 = fromjson("{a: NumberInt(54)}");
    BSONObj match2 = fromjson("{a: NumberLong(54)}");
    BSONObj match3 = fromjson("{a: 54.0}");

    BitsAllSetMatchExpression balls("a"_sd, bitMaskSet);
    BitsAllClearMatchExpression ballc("a"_sd, bitMaskClear);
    BitsAnySetMatchExpression banys("a"_sd, bitMaskSet);
    BitsAnyClearMatchExpression banyc("a"_sd, bitMaskClear);

    ASSERT(exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match3["a"]));
}

TEST(BitTestMatchExpression, MatchesNegativeIntegerWithBitMask) {
    long long bitMaskSet = 10;
    long long bitMaskClear = 5;

    BSONObj match1 = fromjson("{a: NumberInt(-54)}");
    BSONObj match2 = fromjson("{a: NumberLong(-54)}");
    BSONObj match3 = fromjson("{a: -54.0}");

    BitsAllSetMatchExpression balls("a"_sd, bitMaskSet);
    BitsAllClearMatchExpression ballc("a"_sd, bitMaskClear);
    BitsAnySetMatchExpression banys("a"_sd, bitMaskSet);
    BitsAnyClearMatchExpression banyc("a"_sd, bitMaskClear);

    ASSERT(exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match3["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchInteger) {
    BSONArray bas = BSON_ARRAY(1 << 2 << 4 << 5 << 6);
    BSONArray bac = BSON_ARRAY(0 << 3 << 1);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: NumberInt(54)}");
    BSONObj match2 = fromjson("{a: NumberLong(54)}");
    BSONObj match3 = fromjson("{a: 54.0}");

    BitsAllSetMatchExpression balls("a"_sd, bitPositionsSet);
    BitsAllClearMatchExpression ballc("a"_sd, bitPositionsClear);
    BitsAnySetMatchExpression banys("a"_sd, bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a"_sd, bitPositionsClear);

    ASSERT_EQ((size_t)5, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)5, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match3["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match3["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchIntegerWithBitMask) {
    long long bitMaskSet = 118;
    long long bitMaskClear = 11;

    BSONObj match1 = fromjson("{a: NumberInt(54)}");
    BSONObj match2 = fromjson("{a: NumberLong(54)}");
    BSONObj match3 = fromjson("{a: 54.0}");

    BitsAllSetMatchExpression balls("a"_sd, bitMaskSet);
    BitsAllClearMatchExpression ballc("a"_sd, bitMaskClear);
    BitsAnySetMatchExpression banys("a"_sd, bitMaskSet);
    BitsAnyClearMatchExpression banyc("a"_sd, bitMaskClear);

    ASSERT(!exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match3["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match3["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match3["a"]));
}

TEST(BitTestMatchExpression, MatchesBinary1) {
    BSONArray bas = BSON_ARRAY(1 << 2 << 4 << 5);
    BSONArray bac = BSON_ARRAY(0 << 3 << 600);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: {$binary: 'NgAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00110110...
    BSONObj match2 = fromjson("{a: {$binary: 'NgAjqwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: 00110110...

    BitsAllSetMatchExpression balls("a"_sd, bitPositionsSet);
    BitsAllClearMatchExpression ballc("a"_sd, bitPositionsClear);
    BitsAnySetMatchExpression banys("a"_sd, bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a"_sd, bitPositionsClear);

    ASSERT_EQ((size_t)4, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)4, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
}

TEST(BitTestMatchExpression, MatchesBinary2) {
    BSONArray bas = BSON_ARRAY(21 << 22 << 8 << 9);
    BSONArray bac = BSON_ARRAY(20 << 23 << 612);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: {$binary: 'AANgAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00000000 00000011 01100000
    BSONObj match2 = fromjson("{a: {$binary: 'JANgqwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: ........ 00000011 01100000

    BitsAllSetMatchExpression balls("a"_sd, bitPositionsSet);
    BitsAllClearMatchExpression ballc("a"_sd, bitPositionsClear);
    BitsAnySetMatchExpression banys("a"_sd, bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a"_sd, bitPositionsClear);

    ASSERT_EQ((size_t)4, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)4, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
}

TEST(BitTestMatchExpression, MatchesBinaryWithBitMask) {
    const char* bas = "\0\x03\x60\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
    const char* bac = "\0\xFC\x9F\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

    BSONObj match1 = fromjson("{a: {$binary: 'AANgAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00000000 00000011 01100000
    BSONObj match2 = fromjson("{a: {$binary: 'JANgAwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: ........ 00000011 01100000

    BitsAllSetMatchExpression balls("a"_sd, bas, 21);
    BitsAllClearMatchExpression ballc("a"_sd, bac, 21);
    BitsAnySetMatchExpression banys("a"_sd, bas, 21);
    BitsAnyClearMatchExpression banyc("a"_sd, bac, 21);

    ASSERT(exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchBinary1) {
    BSONArray bas = BSON_ARRAY(1 << 2 << 4 << 5 << 6);
    BSONArray bac = BSON_ARRAY(0 << 3 << 1);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: {$binary: 'NgAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00110110...
    BSONObj match2 = fromjson("{a: {$binary: 'NgAjqwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: 00110110...

    BitsAllSetMatchExpression balls("a"_sd, bitPositionsSet);
    BitsAllClearMatchExpression ballc("a"_sd, bitPositionsClear);
    BitsAnySetMatchExpression banys("a"_sd, bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a"_sd, bitPositionsClear);

    ASSERT_EQ((size_t)5, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)5, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchBinary2) {
    BSONArray bas = BSON_ARRAY(21 << 22 << 23 << 24 << 25);
    BSONArray bac = BSON_ARRAY(20 << 23 << 21);
    std::vector<uint32_t> bitPositionsSet = bsonArrayToBitPositions(bas);
    std::vector<uint32_t> bitPositionsClear = bsonArrayToBitPositions(bac);

    BSONObj match1 = fromjson("{a: {$binary: 'AANgAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00000000 00000011 01100000
    BSONObj match2 = fromjson("{a: {$binary: 'JANgqwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: ........ 00000011 01100000

    BitsAllSetMatchExpression balls("a"_sd, bitPositionsSet);
    BitsAllClearMatchExpression ballc("a"_sd, bitPositionsClear);
    BitsAnySetMatchExpression banys("a"_sd, bitPositionsSet);
    BitsAnyClearMatchExpression banyc("a"_sd, bitPositionsClear);

    ASSERT_EQ((size_t)5, balls.numBitPositions());
    ASSERT_EQ((size_t)3, ballc.numBitPositions());
    ASSERT_EQ((size_t)5, banys.numBitPositions());
    ASSERT_EQ((size_t)3, banyc.numBitPositions());
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
}

TEST(BitTestMatchExpression, DoesNotMatchBinaryWithBitMask) {
    const char* bas = "\0\x03\x60\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\xFF";
    const char* bac = "\0\xFD\x9F\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\xFF";

    BSONObj match1 = fromjson("{a: {$binary: 'AANgAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}");
    // Base64 to Binary: 00000000 00000011 01100000
    BSONObj match2 = fromjson("{a: {$binary: 'JANgAwetkqwklEWRbWERKKJREtbq', $type: '00'}}");
    // Base64 to Binary: ........ 00000011 01100000

    BitsAllSetMatchExpression balls("a"_sd, bas, 22);
    BitsAllClearMatchExpression ballc("a"_sd, bac, 22);
    BitsAnySetMatchExpression banys("a"_sd, bas, 22);
    BitsAnyClearMatchExpression banyc("a"_sd, bac, 22);
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&balls, match2["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match1["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&ballc, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banys, match2["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match1["a"]));
    ASSERT(exec::matcher::matchesSingleElement(&banyc, match2["a"]));
}

TEST(LeafMatchExpressionTest, Equal1) {
    BSONObj temp = BSON("x" << 5);
    EqualityMatchExpression e("x"_sd, temp["x"]);

    ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : 5 }")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : [5] }")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : [1,5] }")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : [1,5,2] }")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : [5,2] }")));

    ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : null }")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 6 }")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : [4,2] }")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : [[5]] }")));
}

TEST(LeafMatchExpressionTest, Comp1) {
    BSONObj temp = BSON("x" << 5);

    {
        LTEMatchExpression e("x"_sd, temp["x"]);
        ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : 5 }")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : 4 }")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 6 }")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 'eliot' }")));
    }

    {
        LTMatchExpression e("x"_sd, temp["x"]);
        ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 5 }")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : 4 }")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 6 }")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 'eliot' }")));
    }

    {
        GTEMatchExpression e("x"_sd, temp["x"]);
        ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : 5 }")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 4 }")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : 6 }")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 'eliot' }")));
    }

    {
        GTMatchExpression e("x"_sd, temp["x"]);
        ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 5 }")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 4 }")));
        ASSERT_TRUE(exec::matcher::matchesBSON(&e, fromjson("{ x : 6 }")));
        ASSERT_FALSE(exec::matcher::matchesBSON(&e, fromjson("{ x : 'eliot' }")));
    }
}

TEST(MatchesBSONElement, ScalarEquality) {
    auto filterObj = fromjson("{i: 5}");
    EqualityMatchExpression filter("i"_sd, filterObj["i"]);

    auto aFive = fromjson("{a: 5}");
    auto iFive = fromjson("{i: 5}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aFive["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iFive));

    auto aSix = fromjson("{a: 6}");
    auto iSix = fromjson("{i: 6}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aSix["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iSix));

    auto aArrMatch1 = fromjson("{a: [5, 6]}");
    auto iArrMatch1 = fromjson("{i: [5, 6]}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aArrMatch1["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iArrMatch1));

    auto aArrMatch2 = fromjson("{a: [6, 5]}");
    auto iArrMatch2 = fromjson("{i: [6, 5]}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aArrMatch2["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iArrMatch2));

    auto aArrNoMatch = fromjson("{a: [6, 6]}");
    auto iArrNoMatch = fromjson("{i: [6, 6]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aArrNoMatch["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iArrNoMatch));

    auto aObj = fromjson("{a: {i: 5}}");
    auto iObj = fromjson("{i: {i: 5}}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObj["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObj));

    auto aObjArr = fromjson("{a: [{i: 5}]}");
    auto iObjArr = fromjson("{i: [{i: 5}]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjArr["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjArr));
}

TEST(MatchesBSONElement, DottedPathEquality) {
    auto filterObj = fromjson("{'i.a': 5}");
    EqualityMatchExpression filter("i.a"_sd, filterObj["i.a"]);

    auto aFive = fromjson("{a: 5}");
    auto iFive = fromjson("{i: 5}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aFive["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iFive));

    auto aArr = fromjson("{a: [5]}");
    auto iArr = fromjson("{i: [5]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aArr["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iArr));

    auto aObjMatch = fromjson("{a: {a: 5, b: 6}}");
    auto iObjMatch = fromjson("{i: {a: 5, b: 6}}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aObjMatch["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iObjMatch));

    auto aObjNoMatch1 = fromjson("{a: {a: 6}}");
    auto iObjNoMatch1 = fromjson("{i: {a: 6}}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjNoMatch1["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjNoMatch1));

    auto aObjNoMatch2 = fromjson("{a: {b: 5}}");
    auto iObjNoMatch2 = fromjson("{i: {b: 5}}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjNoMatch2["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjNoMatch2));

    auto aObjArrMatch1 = fromjson("{a: [{a: 5}, {a: 6}]}");
    auto iObjArrMatch1 = fromjson("{i: [{a: 5}, {a: 6}]}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aObjArrMatch1["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iObjArrMatch1));

    auto aObjArrMatch2 = fromjson("{a: [{a: 6}, {a: 5}]}");
    auto iObjArrMatch2 = fromjson("{i: [{a: 6}, {a: 5}]}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aObjArrMatch2["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iObjArrMatch2));

    auto aObjArrNoMatch1 = fromjson("{a: [{a: 6}, {a: 6}]}");
    auto iObjArrNoMatch1 = fromjson("{i: [{a: 6}, {a: 6}]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjArrNoMatch1["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjArrNoMatch1));

    auto aObjArrNoMatch2 = fromjson("{a: [{b: 5}, {b: 5}]}");
    auto iObjArrNoMatch2 = fromjson("{i: [{b: 5}, {b: 5}]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjArrNoMatch2["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjArrNoMatch2));
}

TEST(MatchesBSONElement, ArrayIndexEquality) {
    auto filterObj = fromjson("{'i.1': 5}");
    EqualityMatchExpression filter("i.1"_sd, filterObj["i.1"]);

    auto aFive = fromjson("{a: 5}");
    auto iFive = fromjson("{i: 5}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aFive["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iFive));

    auto aArrMatch = fromjson("{a: [6, 5]}");
    auto iArrMatch = fromjson("{i: [6, 5]}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aArrMatch["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iArrMatch));

    auto aArrNoMatch = fromjson("{a: [5, 6]}");
    auto iArrNoMatch = fromjson("{i: [5, 6]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aArrNoMatch["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iArrNoMatch));

    auto aObjMatch = fromjson("{a: {'1': 5}}");
    auto iObjMatch = fromjson("{i: {'1': 5}}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aObjMatch["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iObjMatch));

    auto aObjNoMatch = fromjson("{a: {i: 5}}");
    auto iObjNoMatch = fromjson("{i: {i: 5}}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjNoMatch["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjNoMatch));

    auto aObjArrMatch = fromjson("{a: [{'1': 5}]}");
    auto iObjArrMatch = fromjson("{i: [{'1': 5}]}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aObjArrMatch["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iObjArrMatch));

    auto aObjArrNoMatch = fromjson("{a: [{i: 6}, {i: 5}]}");
    auto iObjArrNoMatch = fromjson("{i: [{i: 6}, {i: 5}]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjArrNoMatch["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjArrNoMatch));

    auto aArrArr = fromjson("{a: [[6, 5], [6, 5]]}");
    auto iArrArr = fromjson("{i: [[6, 5], [6, 5]]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aArrArr["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iArrArr));
}

TEST(MatchesBSONElement, ObjectEquality) {
    auto filterObj = fromjson("{i: {a: 5}}");
    EqualityMatchExpression filter("i"_sd, filterObj["i"]);

    auto aFive = fromjson("{a: 5}");
    auto iFive = fromjson("{i: 5}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aFive["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iFive));

    auto aArr = fromjson("{a: [5]}");
    auto iArr = fromjson("{i: [5]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aArr["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iArr));

    auto aObjMatch = fromjson("{a: {a: 5}}");
    auto iObjMatch = fromjson("{i: {a: 5}}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aObjMatch["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iObjMatch));

    auto aObjNoMatch1 = fromjson("{a: {a: 5, b: 6}}");
    auto iObjNoMatch1 = fromjson("{i: {a: 5, b: 6}}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjNoMatch1["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjNoMatch1));

    auto aObjNoMatch2 = fromjson("{a: {a: 6}}");
    auto iObjNoMatch2 = fromjson("{i: {a: 6}}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjNoMatch2["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjNoMatch2));

    auto aObjNoMatch3 = fromjson("{a: {b: 5}}");
    auto iObjNoMatch3 = fromjson("{i: {b: 5}}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjNoMatch3["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjNoMatch3));

    auto aObjArrMatch1 = fromjson("{a: [{a: 5}, {a: 6}]}");
    auto iObjArrMatch1 = fromjson("{i: [{a: 5}, {a: 6}]}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aObjArrMatch1["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iObjArrMatch1));

    auto aObjArrMatch2 = fromjson("{a: [{a: 6}, {a: 5}]}");
    auto iObjArrMatch2 = fromjson("{i: [{a: 6}, {a: 5}]}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aObjArrMatch2["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iObjArrMatch2));

    auto aObjArrNoMatch = fromjson("{a: [{a: 6}, {a: 6}]}");
    auto iObjArrNoMatch = fromjson("{i: [{a: 6}, {a: 6}]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjArrNoMatch["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjArrNoMatch));
}

TEST(MatchesBSONElement, ArrayEquality) {
    auto filterObj = fromjson("{i: [5]}");
    EqualityMatchExpression filter("i"_sd, filterObj["i"]);

    auto aFive = fromjson("{a: 5}");
    auto iFive = fromjson("{i: 5}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aFive["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iFive));

    auto aArrMatch = fromjson("{a: [5]}");
    auto iArrMatch = fromjson("{i: [5]}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aArrMatch["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iArrMatch));

    auto aArrNoMatch = fromjson("{a: [5, 6]}");
    auto iArrNoMatch = fromjson("{i: [5, 6]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aArrNoMatch["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iArrNoMatch));

    auto aObj = fromjson("{a: {i: [5]}}");
    auto iObj = fromjson("{i: {i: [5]}}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObj["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObj));

    auto aObjArr = fromjson("{a: [{i: [5]}]}");
    auto iObjArr = fromjson("{i: [{i: [5]}]}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aObjArr["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iObjArr));
}

TEST(NotMatchExpression, MatchesScalar) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a"_sd, baseOperand["$lt"]);
    auto notOp = NotMatchExpression{lt.release()};
    ASSERT(exec::matcher::matchesBSON(&notOp, BSON("a" << 6), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&notOp, BSON("a" << 4), nullptr));
}

TEST(NotMatchExpression, MatchesArray) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a"_sd, baseOperand["$lt"]);
    auto notOp = NotMatchExpression{lt.release()};
    ASSERT(exec::matcher::matchesBSON(&notOp, BSON("a" << BSON_ARRAY(6)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&notOp, BSON("a" << BSON_ARRAY(4)), nullptr));
    // All array elements must match.
    ASSERT(!exec::matcher::matchesBSON(&notOp, BSON("a" << BSON_ARRAY(4 << 5 << 6)), nullptr));
}

TEST(NotMatchExpression, ElemMatchKey) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a"_sd, baseOperand["$lt"]);
    auto notOp = NotMatchExpression{lt.release()};
    auto details = MatchDetails{};
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&notOp, BSON("a" << BSON_ARRAY(1)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&notOp, BSON("a" << 6), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&notOp, BSON("a" << BSON_ARRAY(6)), &details));
    // elemMatchKey is not implemented for negative match operators.
    ASSERT(!details.hasElemMatchKey());
}

TEST(NotMatchExpression, SetCollatorPropagatesToChild) {
    auto baseOperand = BSON("a" << "string");
    auto eq = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperand["a"]);
    auto notOp = NotMatchExpression{eq.release()};
    auto collator = CollatorInterfaceMock{CollatorInterfaceMock::MockType::kAlwaysEqual};
    notOp.setCollator(&collator);
    ASSERT(!exec::matcher::matchesBSON(&notOp, BSON("a" << "string2"), nullptr));
}

TEST(AndOp, NoClauses) {
    auto andMatchExpression = AndMatchExpression{};
    ASSERT(exec::matcher::matchesBSON(&andMatchExpression, BSONObj{}, nullptr));
}

TEST(AndOp, MatchesElementThreeClauses) {
    auto baseOperand1 = BSON("$lt" << "z1");
    auto baseOperand2 = BSON("$gt" << "a1");
    auto match = BSON("a" << "r1");
    auto notMatch1 = BSON("a" << "z1");
    auto notMatch2 = BSON("a" << "a1");
    auto notMatch3 = BSON("a" << "r");

    auto sub1 = std::make_unique<LTMatchExpression>("a"_sd, baseOperand1["$lt"]);
    auto sub2 = std::make_unique<GTMatchExpression>("a"_sd, baseOperand2["$gt"]);
    auto sub3 = std::make_unique<RegexMatchExpression>("a"_sd, "1", "");

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(sub1));
    andOp.add(std::move(sub2));
    andOp.add(std::move(sub3));

    ASSERT(exec::matcher::matchesBSON(&andOp, match));
    ASSERT(!exec::matcher::matchesBSON(&andOp, notMatch1));
    ASSERT(!exec::matcher::matchesBSON(&andOp, notMatch2));
    ASSERT(!exec::matcher::matchesBSON(&andOp, notMatch3));
}

TEST(AndOp, MatchesSingleClause) {
    auto baseOperand = BSON("$ne" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperand["$ne"]);
    auto ne = std::make_unique<NotMatchExpression>(eq.release());

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(ne));

    ASSERT(exec::matcher::matchesBSON(&andOp, BSON("a" << 4), nullptr));
    ASSERT(exec::matcher::matchesBSON(&andOp, BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&andOp, BSON("a" << 5), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&andOp, BSON("a" << BSON_ARRAY(4 << 5)), nullptr));
}

TEST(AndOp, MatchesThreeClauses) {
    auto baseOperand1 = BSON("$gt" << 1);
    auto baseOperand2 = BSON("$lt" << 10);
    auto baseOperand3 = BSON("$lt" << 100);

    auto sub1 = std::make_unique<GTMatchExpression>("a"_sd, baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a"_sd, baseOperand2["$lt"]);
    auto sub3 = std::make_unique<LTMatchExpression>("b"_sd, baseOperand3["$lt"]);

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(sub1));
    andOp.add(std::move(sub2));
    andOp.add(std::move(sub3));

    ASSERT(exec::matcher::matchesBSON(&andOp, BSON("a" << 5 << "b" << 6), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&andOp, BSON("a" << 5), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&andOp, BSON("b" << 6), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&andOp, BSON("a" << 1 << "b" << 6), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&andOp, BSON("a" << 10 << "b" << 6), nullptr));
}

TEST(AndOp, ElemMatchKey) {
    auto baseOperand1 = BSON("a" << 1);
    auto baseOperand2 = BSON("b" << 2);

    auto sub1 = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperand1["a"]);
    auto sub2 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand2["b"]);

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(sub1));
    andOp.add(std::move(sub2));

    auto details = MatchDetails{};
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&andOp, BSON("a" << BSON_ARRAY(1)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!exec::matcher::matchesBSON(&andOp, BSON("b" << BSON_ARRAY(2)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &andOp, BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(1 << 2)), &details));
    ASSERT(details.hasElemMatchKey());
    // The elem match key for the second $and clause is recorded.
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(OrOp, NoClauses) {
    auto orOp = OrMatchExpression{};
    ASSERT(!exec::matcher::matchesBSON(&orOp, BSONObj{}, nullptr));
}

TEST(OrOp, MatchesSingleClause) {
    auto baseOperand = BSON("$ne" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperand["$ne"]);
    auto ne = std::make_unique<NotMatchExpression>(eq.release());

    auto orOp = OrMatchExpression{};
    orOp.add(std::move(ne));

    ASSERT(exec::matcher::matchesBSON(&orOp, BSON("a" << 4), nullptr));
    ASSERT(exec::matcher::matchesBSON(&orOp, BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&orOp, BSON("a" << 5), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&orOp, BSON("a" << BSON_ARRAY(4 << 5)), nullptr));
}

TEST(OrOp, MatchesTwoClauses) {
    auto clauseObj1 = fromjson("{i: 5}");
    auto clauseObj2 = fromjson("{'i.a': 6}");
    auto clause1 = std::make_unique<EqualityMatchExpression>("i"_sd, clauseObj1["i"]);
    auto clause2 = std::make_unique<EqualityMatchExpression>("i.a"_sd, clauseObj2["i.a"]);

    auto filter = OrMatchExpression{};
    filter.add(std::move(clause1));
    filter.add(std::move(clause2));

    auto aClause1 = fromjson("{a: 5}");
    auto iClause1 = fromjson("{i: 5}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aClause1["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iClause1));

    auto aClause2 = fromjson("{a: {a: 6}}");
    auto iClause2 = fromjson("{i: {a: 6}}");
    ASSERT_TRUE(exec::matcher::matchesBSONElement(&filter, aClause2["a"]));
    ASSERT_TRUE(exec::matcher::matchesBSON(&filter, iClause2));

    auto aNoMatch1 = fromjson("{a: 6}");
    auto iNoMatch1 = fromjson("{i: 6}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aNoMatch1["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iNoMatch1));

    auto aNoMatch2 = fromjson("{a: {a: 5}}");
    auto iNoMatch2 = fromjson("{i: {a: 5}}");
    ASSERT_FALSE(exec::matcher::matchesBSONElement(&filter, aNoMatch2["a"]));
    ASSERT_FALSE(exec::matcher::matchesBSON(&filter, iNoMatch2));
}

TEST(OrOp, MatchesThreeClauses) {
    auto baseOperand1 = BSON("$gt" << 10);
    auto baseOperand2 = BSON("$lt" << 0);
    auto baseOperand3 = BSON("b" << 100);
    auto sub1 = std::make_unique<GTMatchExpression>("a"_sd, baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a"_sd, baseOperand2["$lt"]);
    auto sub3 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand3["b"]);

    auto orOp = OrMatchExpression{};
    orOp.add(std::move(sub1));
    orOp.add(std::move(sub2));
    orOp.add(std::move(sub3));

    ASSERT(exec::matcher::matchesBSON(&orOp, BSON("a" << -1), nullptr));
    ASSERT(exec::matcher::matchesBSON(&orOp, BSON("a" << 11), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&orOp, BSON("a" << 5), nullptr));
    ASSERT(exec::matcher::matchesBSON(&orOp, BSON("b" << 100), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&orOp, BSON("b" << 101), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&orOp, BSONObj(), nullptr));
    ASSERT(exec::matcher::matchesBSON(&orOp, BSON("a" << 11 << "b" << 100), nullptr));
}

TEST(OrOp, ElemMatchKey) {
    auto baseOperand1 = BSON("a" << 1);
    auto baseOperand2 = BSON("b" << 2);
    auto sub1 = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperand1["a"]);
    auto sub2 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand2["b"]);

    auto orOp = OrMatchExpression{};
    orOp.add(std::move(sub1));
    orOp.add(std::move(sub2));

    auto details = MatchDetails{};
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&orOp, BSONObj{}, &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!exec::matcher::matchesBSON(
        &orOp, BSON("a" << BSON_ARRAY(10) << "b" << BSON_ARRAY(10)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &orOp, BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(1 << 2)), &details));
    // The elem match key feature is not implemented for $or.
    ASSERT(!details.hasElemMatchKey());
}

TEST(NorOp, NoClauses) {
    auto norOp = NorMatchExpression{};
    ASSERT(exec::matcher::matchesBSON(&norOp, BSONObj{}, nullptr));
}

TEST(NorOp, MatchesSingleClause) {
    auto baseOperand = BSON("$ne" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperand["$ne"]);
    auto ne = std::make_unique<NotMatchExpression>(eq.release());

    auto norOp = NorMatchExpression{};
    norOp.add(std::move(ne));

    ASSERT(!exec::matcher::matchesBSON(&norOp, BSON("a" << 4), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&norOp, BSON("a" << BSON_ARRAY(4 << 6)), nullptr));
    ASSERT(exec::matcher::matchesBSON(&norOp, BSON("a" << 5), nullptr));
    ASSERT(exec::matcher::matchesBSON(&norOp, BSON("a" << BSON_ARRAY(4 << 5)), nullptr));
}

TEST(NorOp, MatchesThreeClauses) {
    auto baseOperand1 = BSON("$gt" << 10);
    auto baseOperand2 = BSON("$lt" << 0);
    auto baseOperand3 = BSON("b" << 100);

    auto sub1 = std::make_unique<GTMatchExpression>("a"_sd, baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a"_sd, baseOperand2["$lt"]);
    auto sub3 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand3["b"]);

    auto norOp = NorMatchExpression{};
    norOp.add(std::move(sub1));
    norOp.add(std::move(sub2));
    norOp.add(std::move(sub3));

    ASSERT(!exec::matcher::matchesBSON(&norOp, BSON("a" << -1), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&norOp, BSON("a" << 11), nullptr));
    ASSERT(exec::matcher::matchesBSON(&norOp, BSON("a" << 5), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&norOp, BSON("b" << 100), nullptr));
    ASSERT(exec::matcher::matchesBSON(&norOp, BSON("b" << 101), nullptr));
    ASSERT(exec::matcher::matchesBSON(&norOp, BSONObj{}, nullptr));
    ASSERT(!exec::matcher::matchesBSON(&norOp, BSON("a" << 11 << "b" << 100), nullptr));
}

TEST(NorOp, ElemMatchKey) {
    auto baseOperand1 = BSON("a" << 1);
    auto baseOperand2 = BSON("b" << 2);
    auto sub1 = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperand1["a"]);
    auto sub2 = std::make_unique<EqualityMatchExpression>("b"_sd, baseOperand2["b"]);

    auto norOp = NorMatchExpression{};
    norOp.add(std::move(sub1));
    norOp.add(std::move(sub2));

    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&norOp, BSON("a" << 1), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(!exec::matcher::matchesBSON(
        &norOp, BSON("a" << BSON_ARRAY(1) << "b" << BSON_ARRAY(10)), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &norOp, BSON("a" << BSON_ARRAY(3) << "b" << BSON_ARRAY(4)), &details));
    // The elem match key feature is not implemented for $nor.
    ASSERT(!details.hasElemMatchKey());
}

TEST(ExpressionTypeTest, MatchesElementStringType) {
    BSONObj match = BSON("a" << "abc");
    BSONObj notMatch = BSON("a" << 5);
    TypeMatchExpression type(""_sd, BSONType::string);
    ASSERT(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionTypeTest, MatchesElementNullType) {
    BSONObj match = BSON("a" << BSONNULL);
    BSONObj notMatch = BSON("a" << "abc");
    TypeMatchExpression type(""_sd, BSONType::null);
    ASSERT(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT(!exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionTypeTest, MatchesElementNumber) {
    BSONObj match1 = BSON("a" << 1);
    BSONObj match2 = BSON("a" << 1LL);
    BSONObj match3 = BSON("a" << 2.5);
    BSONObj notMatch = BSON("a" << "abc");
    ASSERT_EQ(BSONType::numberInt, match1["a"].type());
    ASSERT_EQ(BSONType::numberLong, match2["a"].type());
    ASSERT_EQ(BSONType::numberDouble, match3["a"].type());

    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    TypeMatchExpression typeExpr("a"_sd, std::move(typeSet));

    ASSERT_EQ("a", typeExpr.path());
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&typeExpr, match1["a"]));
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&typeExpr, match2["a"]));
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&typeExpr, match3["a"]));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&typeExpr, notMatch["a"]));
}

TEST(ExpressionTypeTest, MatchesScalar) {
    TypeMatchExpression type("a"_sd, BSONType::boolean);
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << true), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << 1), nullptr));
}

TEST(ExpressionTypeTest, MatchesArray) {
    TypeMatchExpression type("a"_sd, BSONType::numberInt);
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY(4)), nullptr));
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY(4 << "a")), nullptr));
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY("a" << 4)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY("a")), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY(BSON_ARRAY(4))), nullptr));
}

TEST(ExpressionTypeTest, TypeArrayMatchesOuterAndInnerArray) {
    TypeMatchExpression type("a"_sd, BSONType::array);
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSONArray()), nullptr));
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY(4 << "a")), nullptr));
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY(BSONArray() << 2)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << "bar"), nullptr));
}

TEST(ExpressionTypeTest, MatchesObject) {
    TypeMatchExpression type("a"_sd, BSONType::object);
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON("b" << 1)), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << 1), nullptr));
}

TEST(ExpressionTypeTest, MatchesDotNotationFieldObject) {
    TypeMatchExpression type("a.b"_sd, BSONType::object);
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON("b" << BSON("c" << 1))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << BSON("b" << 1)), nullptr));
}

TEST(ExpressionTypeTest, MatchesDotNotationArrayElementArray) {
    TypeMatchExpression type("a.0"_sd, BSONType::array);
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY(BSON_ARRAY(1))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY("b")), nullptr));
}

TEST(ExpressionTypeTest, MatchesDotNotationArrayElementScalar) {
    TypeMatchExpression type("a.0"_sd, BSONType::string);
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY("b")), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY(1)), nullptr));
}

TEST(ExpressionTypeTest, MatchesDotNotationArrayElementObject) {
    TypeMatchExpression type("a.0"_sd, BSONType::object);
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY(BSON("b" << 1))), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << BSON_ARRAY(1)), nullptr));
}

TEST(ExpressionTypeTest, MatchesNull) {
    TypeMatchExpression type("a"_sd, BSONType::null);
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSONNULL), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << 4), nullptr));
    ASSERT(!exec::matcher::matchesBSON(&type, BSONObj(), nullptr));
}

TEST(ExpressionTypeTest, ElemMatchKey) {
    TypeMatchExpression type("a.b"_sd, BSONType::string);
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT(!exec::matcher::matchesBSON(&type, BSON("a" << 1), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(&type, BSON("a" << BSON("b" << "string")), &details));
    ASSERT(!details.hasElemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &type, BSON("a" << BSON("b" << BSON_ARRAY("string"))), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("0", details.elemMatchKey());
    ASSERT(exec::matcher::matchesBSON(
        &type, BSON("a" << BSON_ARRAY(2 << BSON("b" << BSON_ARRAY("string")))), &details));
    ASSERT(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(ExpressionTypeTest, InternalSchemaTypeArrayOnlyMatchesArrays) {
    InternalSchemaTypeExpression expr("a"_sd, BSONType::array);
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: []}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: [1]}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: 1}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: {b: []}}")));
}

TEST(ExpressionTypeTest, InternalSchemaTypeNumberDoesNotMatchArrays) {
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    InternalSchemaTypeExpression expr("a"_sd, std::move(typeSet));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: []}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: [1]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: ['b', 2, 3]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: {b: []}}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: 1}")));
}

TEST(ExpressionTypeTest, TypeExprWithMultipleTypesMatchesAllSuchTypes) {
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    typeSet.bsonTypes.insert(BSONType::string);
    typeSet.bsonTypes.insert(BSONType::object);
    TypeMatchExpression expr("a"_sd, std::move(typeSet));

    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: []}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: 1}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: [1]}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: null}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: 'str'}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: ['str']}")));
}

TEST(ExpressionTypeTest, InternalSchemaTypeExprWithMultipleTypesMatchesAllSuchTypes) {
    MatcherTypeSet typeSet;
    typeSet.allNumbers = true;
    typeSet.bsonTypes.insert(BSONType::string);
    typeSet.bsonTypes.insert(BSONType::object);
    InternalSchemaTypeExpression expr("a"_sd, std::move(typeSet));

    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: []}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: 1}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: [1]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: [{b: 1}, {b: 2}]}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: null}")));
    ASSERT_TRUE(exec::matcher::matchesBSON(&expr, fromjson("{a: 'str'}")));
    ASSERT_FALSE(exec::matcher::matchesBSON(&expr, fromjson("{a: ['str']}")));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataGeneral) {
    BSONObj match = BSON("a" << BSONBinData(nullptr, 0, BinDataType::BinDataGeneral));
    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::bdtCustom));
    InternalSchemaBinDataSubTypeExpression type(""_sd, BinDataType::BinDataGeneral);
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataFunction) {
    BSONObj match = BSON("a" << BSONBinData(nullptr, 0, BinDataType::Function));
    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::MD5Type));
    InternalSchemaBinDataSubTypeExpression type(""_sd, BinDataType::Function);
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataNewUUID) {
    BSONObj match = BSON("a" << BSONBinData(nullptr, 0, BinDataType::newUUID));
    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::BinDataGeneral));
    InternalSchemaBinDataSubTypeExpression type(""_sd, BinDataType::newUUID);
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataMD5Type) {
    BSONObj match = BSON("a" << BSONBinData(nullptr, 0, BinDataType::MD5Type));
    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::newUUID));
    InternalSchemaBinDataSubTypeExpression type(""_sd, BinDataType::MD5Type);
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataEncryptType) {
    BSONObj match = BSON("a" << BSONBinData(nullptr, 0, BinDataType::Encrypt));
    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::newUUID));
    InternalSchemaBinDataSubTypeExpression type(""_sd, BinDataType::Encrypt);
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataColumnType) {
    BSONObj match = BSON("a" << BSONBinData(nullptr, 0, BinDataType::Column));
    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::newUUID));
    InternalSchemaBinDataSubTypeExpression type(""_sd, BinDataType::Column);
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataSensitiveType) {
    BSONObj match = BSON("a" << BSONBinData(nullptr, 0, BinDataType::Sensitive));
    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::newUUID));
    InternalSchemaBinDataSubTypeExpression type(""_sd, BinDataType::Sensitive);
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataVectorType) {
    BSONObj match = BSON("a" << BSONBinData(nullptr, 0, BinDataType::Vector));
    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::newUUID));
    InternalSchemaBinDataSubTypeExpression type(""_sd, BinDataType::Vector);
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, MatchesBinDataBdtCustom) {
    BSONObj match = BSON("a" << BSONBinData(nullptr, 0, BinDataType::bdtCustom));
    BSONObj notMatch = BSON("a" << BSONBinData(nullptr, 0, BinDataType::Function));
    InternalSchemaBinDataSubTypeExpression type(""_sd, BinDataType::bdtCustom);
    ASSERT_TRUE(exec::matcher::matchesSingleElement(&type, match["a"]));
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

TEST(ExpressionBinDataSubTypeTest, DoesNotMatchArrays) {
    InternalSchemaBinDataSubTypeExpression type("a"_sd, BinDataType::BinDataGeneral);
    ASSERT_FALSE(exec::matcher::matchesBSON(
        &type,
        BSON("a" << BSON_ARRAY(BSONBinData(nullptr, 0, BinDataType::BinDataGeneral)
                               << BSONBinData(nullptr, 0, BinDataType::BinDataGeneral)))));
    ASSERT_FALSE(exec::matcher::matchesBSON(
        &type,
        BSON("a" << BSON_ARRAY(BSONBinData(nullptr, 0, BinDataType::BinDataGeneral)
                               << BSONBinData(nullptr, 0, BinDataType::Function)))));
}

TEST(ExpressionBinDataSubTypeTest, DoesNotMatchString) {
    BSONObj notMatch = BSON("a" << "str");
    InternalSchemaBinDataSubTypeExpression type(""_sd, BinDataType::bdtCustom);
    ASSERT_FALSE(exec::matcher::matchesSingleElement(&type, notMatch["a"]));
}

}  // namespace mongo::evaluate_matcher_test
