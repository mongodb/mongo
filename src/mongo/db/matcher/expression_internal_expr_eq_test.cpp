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

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_internal_expr_eq.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

TEST(InternalExprEqMatchExpression, NodesWithDifferentCollationsAreNotEquivalent) {
    auto operand = BSON("a" << 5);
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kReverseString);
    InternalExprEqMatchExpression eq1(operand.firstElement().fieldNameStringData(),
                                      operand.firstElement());
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InternalExprEqMatchExpression eq2(operand.firstElement().fieldNameStringData(),
                                      operand.firstElement());
    eq2.setCollator(&collator2);
    ASSERT_FALSE(eq1.equivalent(&eq2));
}

TEST(InternalExprEqMatchExpression, ExprEqNotEquivalentToRegularEq) {
    auto operand = BSON("a" << 5);
    InternalExprEqMatchExpression exprEq(operand.firstElement().fieldNameStringData(),
                                         operand.firstElement());
    EqualityMatchExpression regularEq(operand.firstElement().fieldNameStringData(),
                                      operand.firstElement());
    ASSERT_FALSE(exprEq.equivalent(&regularEq));
}

TEST(InternalExprEqMatchExpression, NodesNotEquivalentWhenQueryDataDiffers) {
    auto operand1 = BSON("a" << 5);
    auto operand2 = BSON("a" << 6);
    InternalExprEqMatchExpression eq1(operand1.firstElement().fieldNameStringData(),
                                      operand1.firstElement());
    InternalExprEqMatchExpression eq2(operand2.firstElement().fieldNameStringData(),
                                      operand2.firstElement());
    ASSERT_FALSE(eq1.equivalent(&eq2));
}

TEST(InternalExprEqMatchExpression, NodesNotEquivalentWhenQueryDataDiffersByFieldName) {
    auto operand1 = BSON("a" << BSON("b" << 5));
    auto operand2 = BSON("a" << BSON("c" << 5));
    InternalExprEqMatchExpression eq1(operand1.firstElement().fieldNameStringData(),
                                      operand1.firstElement());
    InternalExprEqMatchExpression eq2(operand2.firstElement().fieldNameStringData(),
                                      operand2.firstElement());
    ASSERT_FALSE(eq1.equivalent(&eq2));
}

TEST(InternalExprEqMatchExpression, NodesAreEquivalentWhenTopLevelElementFieldNameDiffers) {
    auto operand1 = BSON("b" << BSON("a" << 5));
    auto operand2 = BSON("c" << BSON("a" << 5));
    InternalExprEqMatchExpression eq1("path"_sd, operand1.firstElement());
    InternalExprEqMatchExpression eq2("path"_sd, operand2.firstElement());
    ASSERT_TRUE(eq1.equivalent(&eq2));
}

TEST(InternalExprEqMatchExpression, NodesNotEquivalentWhenPathDiffers) {
    auto operand = BSON("a" << 5);
    InternalExprEqMatchExpression eq1("path1"_sd, operand.firstElement());
    InternalExprEqMatchExpression eq2("path2"_sd, operand.firstElement());
    ASSERT_FALSE(eq1.equivalent(&eq2));
}

TEST(InternalExprEqMatchExpression, EquivalentNodesAreEquivalent) {
    auto operand = BSON("a" << 5);
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InternalExprEqMatchExpression eq1(operand.firstElement().fieldNameStringData(),
                                      operand.firstElement());
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InternalExprEqMatchExpression eq2(operand.firstElement().fieldNameStringData(),
                                      operand.firstElement());
    eq2.setCollator(&collator2);
    ASSERT_TRUE(eq1.equivalent(&eq2));
}

TEST(InternalExprEqMatchExpression, CorrectlyMatchesScalarElements) {
    BSONObj operand1 = BSON("a" << 5);

    InternalExprEqMatchExpression eq1(operand1.firstElement().fieldNameStringData(),
                                      operand1.firstElement());
    ASSERT_TRUE(eq1.matchesBSON(BSON("a" << 5.0)));
    ASSERT_FALSE(eq1.matchesBSON(BSON("a" << 6)));

    BSONObj operand2 = BSON("a"
                            << "str");
    InternalExprEqMatchExpression eq2(operand2.firstElement().fieldNameStringData(),
                                      operand2.firstElement());
    ASSERT_TRUE(eq2.matchesBSON(BSON("a"
                                     << "str")));
    ASSERT_FALSE(eq2.matchesBSON(BSON("a"
                                      << "string")));
}

TEST(InternalExprEqMatchExpression, StringMatchingRespectsCollation) {
    BSONObj operand = BSON("a"
                           << "string");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    eq.setCollator(&collator);
    ASSERT_TRUE(eq.matchesBSON(BSON("a"
                                    << "string2")));
}

TEST(InternalExprEqMatchExpression, ComparisonRespectsNewCollationAfterCallingSetCollator) {
    BSONObj operand = BSON("a"
                           << "string1");

    CollatorInterfaceMock collatorAlwaysEqual(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock collatorCompareLower(CollatorInterfaceMock::MockType::kToLowerString);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    eq.setCollator(&collatorAlwaysEqual);
    ASSERT_TRUE(eq.matchesBSON(BSON("a"
                                    << "string2")));


    eq.setCollator(&collatorCompareLower);
    ASSERT_TRUE(eq.matchesBSON(BSON("a"
                                    << "string1")));
    ASSERT_TRUE(eq.matchesBSON(BSON("a"
                                    << "STRING1")));
    ASSERT_FALSE(eq.matchesBSON(BSON("a"
                                     << "string2")));
}

TEST(InternalExprEqMatchExpression, CorrectlyMatchesArrayElement) {
    BSONObj operand = BSON("a.b" << 5);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON("b" << 5))));
    ASSERT_FALSE(eq.matchesBSON(BSON("a" << BSON("b" << 6))));
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON_ARRAY("b" << 5))));
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON_ARRAY("b" << BSON_ARRAY(5)))));
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON_ARRAY(5 << "b"))));
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON_ARRAY("b" << 5 << 5))));
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON_ARRAY("b" << 6))));
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 6)))));
    ASSERT_FALSE(eq.matchesBSON(BSON("a" << 1)));
}

TEST(InternalSchemaEqMatchExpression, DoesNotTraverseThroughAnArrayWithANumericalPathComponent) {
    BSONObj operand = BSON("" << 5);
    InternalExprEqMatchExpression eq("a.0.b", operand.firstElement());
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON("0" << BSON("b" << 5)))));
    ASSERT_FALSE(eq.matchesBSON(BSON("a" << BSON("0" << BSON("b" << 6)))));
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON_ARRAY(BSON("b" << 7)))));
}

TEST(InternalExprEqMatchExpression, CorrectlyMatchesNullElement) {
    BSONObj operand = BSON("a" << BSONNULL);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    // Expression equality to null should match literal null, but not missing or undefined.
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSONNULL)));
    ASSERT_FALSE(eq.matchesBSON(BSONObj()));
    ASSERT_FALSE(eq.matchesBSON(BSON("a" << BSONUndefined)));
    ASSERT_FALSE(eq.matchesBSON(BSON("a" << 4)));
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2))));
}

TEST(InternalExprEqMatchExpression, CorrectlyMatchesNaN) {
    BSONObj operand = BSON("x" << kNaN);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(eq.matchesBSON(BSON("x" << kNaN)));
    ASSERT_FALSE(eq.matchesBSON(BSON("x" << 0)));
    ASSERT_FALSE(eq.matchesBSON(BSONObj()));
    ASSERT_TRUE(eq.matchesBSON(BSON("x" << BSON_ARRAY(1))));
}

TEST(InternalExprEqMatchExpression, DoesNotTraverseLeafArrays) {
    BSONObj operand = BSON("a" << 5);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << 5.0)));
    ASSERT_TRUE(eq.matchesBSON(BSON("a" << BSON_ARRAY("foo"))));
}

TEST(InternalExprEqMatchExpression, CorrectlyMatchesSubfieldAlongDottedPath) {
    BSONObj operand = BSON("x.y.z" << 5);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(eq.matchesBSON(BSON("x" << BSON("y" << BSON("z" << 5)))));
    ASSERT_FALSE(eq.matchesBSON(BSON("x" << BSON("y" << BSON("z" << 4)))));
    ASSERT_FALSE(eq.matchesBSON(BSON("x" << BSON("y" << 5))));
}

TEST(InternalExprEqMatchExpression, AlwaysMatchesDocumentWithArrayAlongPath) {
    BSONObj operand = BSON("x.y.z" << 5);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(eq.matchesBSON(BSON("x" << BSON_ARRAY(6))));
    ASSERT_TRUE(eq.matchesBSON(BSON("x" << BSON("y" << BSON_ARRAY(6)))));
    ASSERT_TRUE(eq.matchesBSON(BSON("x" << BSON_ARRAY(BSON("y" << BSON("z" << 6))))));
    ASSERT_TRUE(eq.matchesBSON(BSON("x" << BSON("y" << BSON_ARRAY(BSON("z" << 6))))));
    ASSERT_TRUE(eq.matchesBSON(BSON("x" << BSON("y" << BSON("z" << BSON_ARRAY(10))))));

    ASSERT_FALSE(
        eq.matchesBSON(BSON("x" << BSON("y" << BSON("z" << BSON("foo" << BSON_ARRAY(10)))))));
}

TEST(InternalExprEqMatchExpression, ConsidersFieldNameInObjectEquality) {
    BSONObj operand = BSON("x" << BSON("a" << 1));

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
    ASSERT_TRUE(eq.matchesBSON(BSON("x" << BSON("a" << 1))));
    ASSERT_FALSE(eq.matchesBSON(BSON("x" << BSON("y" << 1))));
    ASSERT_FALSE(eq.matchesBSON(BSON("y" << BSON("a" << 1))));
}

TEST(InternalExprEqMatchExpression, SerializesCorrectly) {
    BSONObj operand = BSON("x" << 5);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());

    BSONObjBuilder bob;
    eq.serialize(&bob);

    ASSERT_BSONOBJ_EQ(BSON("x" << BSON("$_internalExprEq" << 5)), bob.obj());
}

TEST(InternalExprEqMatchExpression, EquivalentReturnsCorrectResults) {
    auto query = fromjson("{a: {$_internalExprEq: {b: {c: 1, d: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher eqExpr(query, expCtx);

    // Field order is considered when evaluating equivalency.
    query = fromjson("{a: {$_internalExprEq: {b: {d: 1, c: 1}}}}");
    Matcher eqDifferentFieldOrder(query, expCtx);
    ASSERT_FALSE(
        eqExpr.getMatchExpression()->equivalent(eqDifferentFieldOrder.getMatchExpression()));

    query = fromjson("{a: {$_internalExprEq: {b: {d: 1}}}}");
    Matcher eqMissingField(query, expCtx);
    ASSERT_FALSE(eqExpr.getMatchExpression()->equivalent(eqMissingField.getMatchExpression()));
}

TEST(InternalExprEqMatchExpression, EquivalentToClone) {
    auto query = fromjson("{a: {$_internalExprEq: {a:1, b: {c: 1, d: [1]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Matcher eq(query, expCtx);
    eq.getMatchExpression()->setCollator(&collator);
    auto relevantTag = std::make_unique<RelevantTag>();
    relevantTag->first.push_back(0u);
    relevantTag->notFirst.push_back(1u);
    eq.getMatchExpression()->setTag(relevantTag.release());

    auto clone = eq.getMatchExpression()->shallowClone();
    ASSERT_TRUE(eq.getMatchExpression()->equivalent(clone.get()));
}

DEATH_TEST(InternalExprEqMatchExpression,
           CannotCompareToArray,
           "Invariant failure _rhs.type() != BSONType::Array") {
    auto operand = BSON("a" << BSON_ARRAY(1 << 2));
    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
}

DEATH_TEST(InternalExprEqMatchExpression,
           CannotCompareToUndefined,
           "Invariant failure _rhs.type() != BSONType::Undefined") {
    auto operand = BSON("a" << BSONUndefined);
    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
}

DEATH_TEST(InternalExprEqMatchExpression, CannotCompareToMissing, "Invariant failure _rhs") {
    InternalExprEqMatchExpression eq("a"_sd, BSONElement());
}
}  // namespace
}  // namespace mongo
