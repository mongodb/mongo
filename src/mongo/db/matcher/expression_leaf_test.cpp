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

#include <cmath>
// IWYU pragma: no_include "ext/type_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

using std::string;

TEST(ComparisonMatchExpression, ComparisonMatchExpressionsWithUnequalCollatorsAreUnequal) {
    BSONObj operand = BSON("a" << 5);
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kReverseString);
    EqualityMatchExpression eq1("a"_sd, operand["a"]);
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    EqualityMatchExpression eq2("a"_sd, operand["a"]);
    eq2.setCollator(&collator2);
    ASSERT(!eq1.equivalent(&eq2));
}

TEST(ComparisonMatchExpression, ComparisonMatchExpressionsWithEqualCollatorsAreEqual) {
    BSONObj operand = BSON("a" << 5);
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kAlwaysEqual);
    EqualityMatchExpression eq1("a"_sd, operand["a"]);
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    EqualityMatchExpression eq2("a"_sd, operand["a"]);
    eq2.setCollator(&collator2);
    ASSERT(eq1.equivalent(&eq2));
}

DEATH_TEST_REGEX(EqOp, InvalidEooOperand, "failure.*eoo") {
    try {
        BSONObj operand;
        EqualityMatchExpression eq(""_sd, operand.firstElement());
    } catch (...) {
        invariant(false, "Threw when trying to construct obj from eoo element");
    }
}

TEST(EqOp, Equality1) {
    BSONObj operand = BSON("a" << 5 << "b" << 5 << "c" << 4);
    EqualityMatchExpression eq1("a"_sd, operand["a"]);
    EqualityMatchExpression eq2("a"_sd, operand["b"]);
    EqualityMatchExpression eq3("c"_sd, operand["c"]);

    ASSERT(eq1.equivalent(&eq1));
    ASSERT(eq1.equivalent(&eq2));
    ASSERT(!eq1.equivalent(&eq3));
}

DEATH_TEST_REGEX(LtOp, InvalidEooOperand, "failure.*eoo") {
    try {
        BSONObj operand;
        LTMatchExpression lt(""_sd, operand.firstElement());
    } catch (...) {
        invariant(false, "Threw when trying to construct obj from eoo element");
    }
}

DEATH_TEST_REGEX(LteOp, InvalidEooOperand, "failure.*eoo") {
    try {
        BSONObj operand;
        LTEMatchExpression lte(""_sd, operand.firstElement());
    } catch (...) {
        invariant(false, "Threw when trying to construct obj from eoo element");
    }
}

DEATH_TEST_REGEX(GtOp, InvalidEooOperand, "failure.*eoo") {
    try {
        BSONObj operand;
        GTMatchExpression gt(""_sd, operand.firstElement());
    } catch (...) {
        invariant(false, "Threw when trying to construct obj from eoo element");
    }
}

DEATH_TEST_REGEX(GteOp, InvalidEooOperand, "failure.*eoo") {
    try {
        BSONObj operand;
        GTEMatchExpression gte(""_sd, operand.firstElement());
    } catch (...) {
        invariant(false, "Threw when trying to construct obj from eoo element");
    }
}

TEST(RegexMatchExpression, TooLargePattern) {
    string tooLargePattern(50 * 1000, 'z');
    ASSERT_THROWS_CODE(
        RegexMatchExpression("a"_sd, tooLargePattern, ""), AssertionException, 51091);
}

TEST(RegexMatchExpression, Equality1) {
    RegexMatchExpression r1("a"_sd, "b", "");
    RegexMatchExpression r2("a"_sd, "b", "x");
    RegexMatchExpression r3("a"_sd, "c", "");
    RegexMatchExpression r4("b"_sd, "b", "");

    ASSERT(r1.equivalent(&r1));
    ASSERT(!r1.equivalent(&r2));
    ASSERT(!r1.equivalent(&r3));
    ASSERT(!r1.equivalent(&r4));
}

TEST(RegexMatchExpression, RegexCannotContainEmbeddedNullByte) {
    {
        const auto embeddedNull = "a\0b"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression("path"_sd, embeddedNull, ""),
                           AssertionException,
                           ErrorCodes::BadValue);
    }

    {
        const auto singleNullByte = "\0"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression("path"_sd, singleNullByte, ""),
                           AssertionException,
                           ErrorCodes::BadValue);
    }

    {
        const auto leadingNullByte = "\0bbbb"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression("path"_sd, leadingNullByte, ""),
                           AssertionException,
                           ErrorCodes::BadValue);
    }

    {
        const auto trailingNullByte = "bbbb\0"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression("path"_sd, trailingNullByte, ""),
                           AssertionException,
                           ErrorCodes::BadValue);
    }
}

TEST(RegexMatchExpression, RegexOptionsStringCannotContainEmbeddedNullByte) {
    {
        const auto embeddedNull = "a\0b"_sd;
        ASSERT_THROWS_CODE(
            RegexMatchExpression("path"_sd, "pattern", embeddedNull), AssertionException, 51108);
    }

    {
        const auto singleNullByte = "\0"_sd;
        ASSERT_THROWS_CODE(
            RegexMatchExpression("path"_sd, "pattern", singleNullByte), AssertionException, 51108);
    }

    {
        const auto leadingNullByte = "\0bbbb"_sd;
        ASSERT_THROWS_CODE(
            RegexMatchExpression("path"_sd, "pattern", leadingNullByte), AssertionException, 51108);
    }

    {
        const auto trailingNullByte = "bbbb\0"_sd;
        ASSERT_THROWS_CODE(RegexMatchExpression("path"_sd, "pattern", trailingNullByte),
                           AssertionException,
                           51108);
    }
}

TEST(RegexMatchExpression, MalformedRegexNotAccepted) {
    ASSERT_THROWS_CODE(RegexMatchExpression("a"_sd,  // path
                                            "[",     // regex
                                            ""       // options
                                            ),
                       AssertionException,
                       51091);
}

TEST(RegexMatchExpression, MalformedRegexWithStartOptionNotAccepted) {
    ASSERT_THROWS_CODE(RegexMatchExpression("a"_sd, "[(*ACCEPT)", ""), AssertionException, 51091);
}

TEST(ModMatchExpression, ZeroDivisor) {
    ASSERT_THROWS_CODE(ModMatchExpression(""_sd, 0, 1), AssertionException, ErrorCodes::BadValue);
}

TEST(ModMatchExpression, Equality1) {
    ModMatchExpression m1("a"_sd, 1, 2);
    ModMatchExpression m2("a"_sd, 2, 2);
    ModMatchExpression m3("a"_sd, 1, 1);
    ModMatchExpression m4("b"_sd, 1, 2);

    ASSERT(m1.equivalent(&m1));
    ASSERT(!m1.equivalent(&m2));
    ASSERT(!m1.equivalent(&m3));
    ASSERT(!m1.equivalent(&m4));
}

TEST(ExistsMatchExpression, Equivalent) {
    ExistsMatchExpression e1("a"_sd);
    ExistsMatchExpression e2("b"_sd);

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
}

TEST(InMatchExpression, MatchesUndefined) {
    BSONObj operand = BSON_ARRAY(BSONUndefined);

    InMatchExpression in("a"_sd);
    std::vector<BSONElement> equalities{operand.firstElement()};
    ASSERT_NOT_OK(in.setEqualities(std::move(equalities)));
}

TEST(InMatchExpression, InMatchExpressionsWithDifferentNumbersOfElementsAreUnequal) {
    BSONObj obj = BSON("" << "string");
    InMatchExpression eq1(""_sd);
    InMatchExpression eq2(""_sd);
    std::vector<BSONElement> equalities{obj.firstElement()};
    ASSERT_OK(eq1.setEqualities(std::move(equalities)));
    ASSERT(!eq1.equivalent(&eq2));
}

TEST(InMatchExpression, InMatchExpressionsWithUnequalCollatorsAreUnequal) {
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kReverseString);
    InMatchExpression eq1(""_sd);
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression eq2(""_sd);
    eq2.setCollator(&collator2);
    ASSERT(!eq1.equivalent(&eq2));
}

TEST(InMatchExpression, InMatchExpressionsWithEqualCollatorsAreEqual) {
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression eq1(""_sd);
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression eq2(""_sd);
    eq2.setCollator(&collator2);
    ASSERT(eq1.equivalent(&eq2));
}

TEST(InMatchExpression, InMatchExpressionsWithCollationEquivalentElementsAreEqual) {
    BSONObj obj1 = BSON("" << "string1");
    BSONObj obj2 = BSON("" << "string2");
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression eq1(""_sd);
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kAlwaysEqual);
    InMatchExpression eq2(""_sd);
    eq2.setCollator(&collator2);

    std::vector<BSONElement> equalities1{obj1.firstElement()};
    ASSERT_OK(eq1.setEqualities(std::move(equalities1)));

    std::vector<BSONElement> equalities2{obj2.firstElement()};
    ASSERT_OK(eq2.setEqualities(std::move(equalities2)));

    ASSERT(eq1.equivalent(&eq2));
}

TEST(InMatchExpression, InMatchExpressionsWithCollationNonEquivalentElementsAreUnequal) {
    BSONObj obj1 = BSON("" << "string1");
    BSONObj obj2 = BSON("" << "string2");
    CollatorInterfaceMock collator1(CollatorInterfaceMock::MockType::kReverseString);
    InMatchExpression eq1(""_sd);
    eq1.setCollator(&collator1);
    CollatorInterfaceMock collator2(CollatorInterfaceMock::MockType::kReverseString);
    InMatchExpression eq2(""_sd);
    eq2.setCollator(&collator2);

    std::vector<BSONElement> equalities1{obj1.firstElement()};
    ASSERT_OK(eq1.setEqualities(std::move(equalities1)));

    std::vector<BSONElement> equalities2{obj2.firstElement()};
    ASSERT_OK(eq2.setEqualities(std::move(equalities2)));

    ASSERT(!eq1.equivalent(&eq2));
}

TEST(InMatchExpression, ChangingCollationAfterAddingEqualitiesPreservesEqualities) {
    BSONObj obj1 = BSON("" << "string1");
    BSONObj obj2 = BSON("" << "string2");
    CollatorInterfaceMock collatorAlwaysEqual(CollatorInterfaceMock::MockType::kAlwaysEqual);
    CollatorInterfaceMock collatorReverseString(CollatorInterfaceMock::MockType::kReverseString);
    InMatchExpression in(""_sd);
    in.setCollator(&collatorAlwaysEqual);
    std::vector<BSONElement> equalities{obj1.firstElement(), obj2.firstElement()};
    ASSERT_OK(in.setEqualities(std::move(equalities)));
    ASSERT(in.getEqualities().size() == 1);
    in.setCollator(&collatorReverseString);
    ASSERT(in.getEqualities().size() == 2);
    ASSERT(in.contains(obj1.firstElement()));
    ASSERT(in.contains(obj2.firstElement()));
}

// Serializes expression 'expression' into the query shape format.
BSONObj serializeToQueryShape(const MatchExpression& expression) {
    BSONObjBuilder objBuilder;
    expression.serialize(&objBuilder,
                         SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    return objBuilder.obj();
}

TEST(InMatchExpression, SerializeToQueryShapeEmptyList) {
    InMatchExpression matchExpression("a"_sd);
    ASSERT_BSONOBJ_EQ(serializeToQueryShape(matchExpression), fromjson("{ a: { $in: [] } }"));
}

TEST(InMatchExpression, SerializeToQueryShapeMultipleElementsList) {
    BSONObj operand = BSON_ARRAY(1 << "r" << true);
    InMatchExpression matchExpression("a"_sd);
    ASSERT_OK(matchExpression.setEqualities({operand[0], operand[1], operand[2]}));
    ASSERT_BSONOBJ_EQ(serializeToQueryShape(matchExpression),
                      fromjson("{ a: { $in: [ 2, \"or more types\" ] } }"));
}

TEST(InMatchExpression, SerializeToQueryShapeSingleElementList) {
    BSONObj operand = BSON_ARRAY(5);
    InMatchExpression matchExpression("a"_sd);
    ASSERT_OK(matchExpression.setEqualities({operand.firstElement()}));
    ASSERT_BSONOBJ_EQ(serializeToQueryShape(matchExpression), fromjson("{ a: { $in: [1] } }"));
}

DEATH_TEST_REGEX(RegexMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    BSONObj match = BSON("a" << "b");
    BSONObj notMatch = BSON("a" << "c");
    RegexMatchExpression regex(""_sd, "b", "");

    ASSERT_EQ(regex.numChildren(), 0);
    ASSERT_THROWS_CODE(regex.getChild(0), AssertionException, 6400209);
}

DEATH_TEST_REGEX(ModMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    ModMatchExpression mod("a"_sd, 5, 2);

    ASSERT_EQ(mod.numChildren(), 0);
    ASSERT_THROWS_CODE(mod.getChild(0), AssertionException, 6400209);
}

DEATH_TEST_REGEX(ExistsMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    ExistsMatchExpression exists("a"_sd);

    ASSERT_EQ(exists.numChildren(), 0);
    ASSERT_THROWS_CODE(exists.getChild(0), AssertionException, 6400209);
}

DEATH_TEST_REGEX(InMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    InMatchExpression in("a"_sd);

    ASSERT_EQ(in.numChildren(), 0);
    ASSERT_THROWS_CODE(in.getChild(0), AssertionException, 6400209);
}

DEATH_TEST_REGEX(BitTestMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    long long bitMask = 54;

    BitsAllSetMatchExpression balls("a"_sd, bitMask);

    ASSERT_EQ(balls.numChildren(), 0);
    ASSERT_THROWS_CODE(balls.getChild(0), AssertionException, 6400209);
}

DEATH_TEST_REGEX(ComparisonMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    BSONObj operand = BSON("a" << "string");
    EqualityMatchExpression eq("a"_sd, operand["a"]);

    ASSERT_EQ(eq.numChildren(), 0);
    ASSERT_THROWS_CODE(eq.getChild(0), AssertionException, 6400209);
}
}  // namespace mongo
