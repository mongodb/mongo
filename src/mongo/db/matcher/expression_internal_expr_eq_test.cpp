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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

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

TEST(InternalExprEqMatchExpression, SerializesCorrectly) {
    BSONObj operand = BSON("x" << 5);

    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());

    BSONObjBuilder bob;
    eq.serialize(&bob, {});

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

    auto clone = eq.getMatchExpression()->clone();
    ASSERT_TRUE(eq.getMatchExpression()->equivalent(clone.get()));
}

DEATH_TEST_REGEX(InternalExprEqMatchExpression,
                 CannotCompareToArray,
                 R"#(Invariant failure.*_rhs.type\(\) != BSONType::array)#") {
    auto operand = BSON("a" << BSON_ARRAY(1 << 2));
    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
}

DEATH_TEST_REGEX(InternalExprEqMatchExpression,
                 CannotCompareToUndefined,
                 R"#(Invariant failure.*_rhs.type\(\) != BSONType::undefined)#") {
    auto operand = BSON("a" << BSONUndefined);
    InternalExprEqMatchExpression eq(operand.firstElement().fieldNameStringData(),
                                     operand.firstElement());
}

DEATH_TEST_REGEX(InternalExprEqMatchExpression, CannotCompareToMissing, "Invariant failure.*_rhs") {
    InternalExprEqMatchExpression eq("a"_sd, BSONElement());
}
}  // namespace
}  // namespace mongo
