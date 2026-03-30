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
#include <cstdint>
#include <limits>
// IWYU pragma: no_include "ext/type_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/match_expression_test_util.h"
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

DEATH_TEST_REGEX(EqOpDeathTest, InvalidEooOperand, "failure.*eoo") {
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

DEATH_TEST_REGEX(LtOpDeathTest, InvalidEooOperand, "failure.*eoo") {
    try {
        BSONObj operand;
        LTMatchExpression lt(""_sd, operand.firstElement());
    } catch (...) {
        invariant(false, "Threw when trying to construct obj from eoo element");
    }
}

DEATH_TEST_REGEX(LteOpDeathTest, InvalidEooOperand, "failure.*eoo") {
    try {
        BSONObj operand;
        LTEMatchExpression lte(""_sd, operand.firstElement());
    } catch (...) {
        invariant(false, "Threw when trying to construct obj from eoo element");
    }
}

DEATH_TEST_REGEX(GtOpDeathTest, InvalidEooOperand, "failure.*eoo") {
    try {
        BSONObj operand;
        GTMatchExpression gt(""_sd, operand.firstElement());
    } catch (...) {
        invariant(false, "Threw when trying to construct obj from eoo element");
    }
}

DEATH_TEST_REGEX(GteOpDeathTest, InvalidEooOperand, "failure.*eoo") {
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

DEATH_TEST_REGEX(RegexMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    BSONObj match = BSON("a" << "b");
    BSONObj notMatch = BSON("a" << "c");
    RegexMatchExpression regex(""_sd, "b", "");

    ASSERT_EQ(regex.numChildren(), 0);
    ASSERT_THROWS_CODE(regex.getChild(0), AssertionException, 6400209);
}

DEATH_TEST_REGEX(ModMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    ModMatchExpression mod("a"_sd, 5, 2);

    ASSERT_EQ(mod.numChildren(), 0);
    ASSERT_THROWS_CODE(mod.getChild(0), AssertionException, 6400209);
}

DEATH_TEST_REGEX(ExistsMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    ExistsMatchExpression exists("a"_sd);

    ASSERT_EQ(exists.numChildren(), 0);
    ASSERT_THROWS_CODE(exists.getChild(0), AssertionException, 6400209);
}

DEATH_TEST_REGEX(InMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    InMatchExpression in("a"_sd);

    ASSERT_EQ(in.numChildren(), 0);
    ASSERT_THROWS_CODE(in.getChild(0), AssertionException, 6400209);
}

// ---------------------------------------------------------------------------
// BitTestMatchExpression::equivalent() tests
//
// All four operators share the same equivalent() implementation (it compares
// sorted bit positions derived from getBitPositions()), so the BinData/Array
// cross-form cases are demonstrated with BitsAllSet; the remaining operators
// only verify that different operator types are never equivalent.
// ---------------------------------------------------------------------------

TEST(BitTestMatchExpressionEquivalentTest, ArrayOrderDoesNotMatterForEquivalence) {
    BitsAllSetMatchExpression e1("a"_sd, std::vector<uint32_t>{4, 1, 5, 2});
    BitsAllSetMatchExpression e2("a"_sd, std::vector<uint32_t>{1, 2, 4, 5});
    ASSERT_TRUE(e1.equivalent(&e2));
}

TEST(BitTestMatchExpressionEquivalentTest, MultiByteBinDataAndArrayAreEquivalent) {
    // Bits 9, 10, 12, 13 live in byte 1 (bits 1, 2, 4, 5 within that byte).
    std::vector<uint32_t> positions{9, 10, 12, 13};
    auto buf = bitPositionsToBinData(positions);
    BitsAllSetMatchExpression fromBinData("a"_sd, buf.data(), buf.size());
    BitsAllSetMatchExpression fromArray("a"_sd, positions);
    BitsAllSetMatchExpression fromInt64("a"_sd, uint64_t{0x3600});  // bits 9,10,12,13 = 0x3600

    ASSERT_TRUE(fromBinData.equivalent(&fromArray));
    ASSERT_TRUE(fromArray.equivalent(&fromBinData));
    ASSERT_TRUE(fromInt64.equivalent(&fromBinData));
    ASSERT_TRUE(fromInt64.equivalent(&fromArray));
}

TEST(BitTestMatchExpressionEquivalentTest, IntegerMaskAndBinDataAreEquivalent) {
    std::vector<uint32_t> positions{1, 2, 4, 5};
    auto buf = bitPositionsToBinData(positions);
    BitsAllSetMatchExpression fromMask("a"_sd, uint64_t{0x36});
    BitsAllSetMatchExpression fromBinData("a"_sd, buf.data(), buf.size());

    ASSERT_TRUE(fromMask.equivalent(&fromBinData));
    ASSERT_TRUE(fromBinData.equivalent(&fromMask));
}

TEST(BitTestMatchExpressionEquivalentTest, NegativeInt64Bit63EquivalentToBinDataAndArray) {
    // The min integer has only bit 63 set (the sign bit in two's complement).
    // A BinData with byte[7]=0x80 and an array [63] encode the same single bit.
    auto buf = bitPositionsToBinData({63});  // 8 bytes, byte[7] = 0x80
    BitsAllSetMatchExpression fromNeg("a"_sd, std::numeric_limits<long long>::min());
    BitsAllSetMatchExpression fromBinData("a"_sd, buf.data(), buf.size());
    BitsAllSetMatchExpression fromArray("a"_sd, std::vector<uint32_t>{63});

    ASSERT_TRUE(fromNeg.equivalent(&fromBinData));
    ASSERT_TRUE(fromNeg.equivalent(&fromArray));
    ASSERT_TRUE(fromBinData.equivalent(&fromNeg));
    ASSERT_TRUE(fromArray.equivalent(&fromNeg));
}

TEST(BitTestMatchExpressionEquivalentTest, AllFourOperatorsWorkWithBinDataAndArray) {
    // Verify the BinData/Array cross-form equivalence holds for all four operators.
    std::vector<uint32_t> positions{3, 4, 6};
    auto buf = bitPositionsToBinData(positions);
    const char* data = buf.data();
    const uint32_t len = buf.size();

    BitsAllSetMatchExpression allSet_bin("a"_sd, data, len);
    BitsAllSetMatchExpression allSet_arr("a"_sd, positions);
    ASSERT_TRUE(allSet_bin.equivalent(&allSet_arr));

    BitsAllClearMatchExpression allClear_bin("a"_sd, data, len);
    BitsAllClearMatchExpression allClear_arr("a"_sd, positions);
    ASSERT_TRUE(allClear_bin.equivalent(&allClear_arr));

    BitsAnySetMatchExpression anySet_bin("a"_sd, data, len);
    BitsAnySetMatchExpression anySet_arr("a"_sd, positions);
    ASSERT_TRUE(anySet_bin.equivalent(&anySet_arr));

    BitsAnyClearMatchExpression anyClear_bin("a"_sd, data, len);
    BitsAnyClearMatchExpression anyClear_arr("a"_sd, positions);
    ASSERT_TRUE(anyClear_bin.equivalent(&anyClear_arr));
}

TEST(BitTestMatchExpressionEquivalentTest, DifferentOperatorsAreNotEquivalent) {
    auto buf = bitPositionsToBinData({1, 2, 4, 5});
    const char* data = buf.data();
    const uint32_t len = buf.size();
    BitsAllSetMatchExpression allSet("a"_sd, data, len);
    BitsAllClearMatchExpression allClear("a"_sd, data, len);
    BitsAnySetMatchExpression anySet("a"_sd, data, len);
    BitsAnyClearMatchExpression anyClear("a"_sd, data, len);

    ASSERT_FALSE(allSet.equivalent(&allClear));
    ASSERT_FALSE(allSet.equivalent(&anySet));
    ASSERT_FALSE(allSet.equivalent(&anyClear));
    ASSERT_FALSE(anySet.equivalent(&anyClear));
}

TEST(BitTestMatchExpressionEquivalentTest, DifferentBitsAreNotEquivalent) {
    BitsAllSetMatchExpression e1("a"_sd, std::vector<uint32_t>{1, 2, 4, 5});
    BitsAllSetMatchExpression e2("a"_sd, std::vector<uint32_t>{1, 2, 4});

    ASSERT_FALSE(e1.equivalent(&e2));
    ASSERT_FALSE(e2.equivalent(&e1));
}

TEST(BitTestMatchExpressionEquivalentTest, DifferentPathsAreNotEquivalent) {
    auto buf = bitPositionsToBinData({1, 2, 4, 5});
    const char* data = buf.data();
    const uint32_t len = buf.size();
    BitsAllSetMatchExpression onA("a"_sd, data, len);
    BitsAllSetMatchExpression onB("b"_sd, data, len);

    ASSERT_FALSE(onA.equivalent(&onB));
}

TEST(BitTestMatchExpressionEquivalentTest, TrailingZeroBytesDoNotAffectEquivalence) {
    // {0x36} and {0x36, 0x00} encode the same bit positions — the trailing zero byte
    // contributes no bits, so both expressions must be equivalent().
    auto buf1 = bitPositionsToBinData({1, 2, 4, 5});  // 1 byte: 0x36
    auto buf2 = buf1;
    buf2.push_back(0x00);  // 2 bytes: 0x36 0x00

    BitsAllSetMatchExpression e1("a"_sd, buf1.data(), buf1.size());
    BitsAllSetMatchExpression e2("a"_sd, buf2.data(), buf2.size());
    BitsAllSetMatchExpression e3("a"_sd, uint64_t{0x36});  // bits 1,2,4,5 = 0x36

    ASSERT_TRUE(e1.equivalent(&e2));
    ASSERT_TRUE(e2.equivalent(&e1));
    ASSERT_TRUE(e3.equivalent(&e1));
    ASSERT_TRUE(e3.equivalent(&e2));
}

DEATH_TEST_REGEX(BitTestMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    long long bitMask = 54;

    BitsAllSetMatchExpression balls("a"_sd, bitMask);

    ASSERT_EQ(balls.numChildren(), 0);
    ASSERT_THROWS_CODE(balls.getChild(0), AssertionException, 6400209);
}

DEATH_TEST_REGEX(ComparisonMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400209") {
    BSONObj operand = BSON("a" << "string");
    EqualityMatchExpression eq("a"_sd, operand["a"]);

    ASSERT_EQ(eq.numChildren(), 0);
    ASSERT_THROWS_CODE(eq.getChild(0), AssertionException, 6400209);
}
}  // namespace mongo
