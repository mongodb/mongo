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


#include "mongo/crypto/fle_numeric.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

bool operator==(const OSTType_Int32& lhs, const OSTType_Int32& rhs) {
    return std::tie(lhs.value, lhs.min, lhs.max) == std::tie(rhs.value, rhs.min, rhs.max);
}

TEST(FLE2NumericTest, RangeTest_Int32_NoBounds) {
#define ASSERT_EI(x, y) ASSERT_EQ(getTypeInfo32((x), boost::none, boost::none).value, (y));

    ASSERT_EI(2147483647, 4294967295);

    ASSERT_EI(1, 2147483649);
    ASSERT_EI(0, 2147483648);
    ASSERT_EI(-1, 2147483647);
    ASSERT_EI(-2, 2147483646);
    ASSERT_EI(-2147483647, 1);

    // min int32_t, no equivalent in positive part of integer
    ASSERT_EI(-2147483648, 0);

#undef ASSERT_EI
}

std::basic_ostream<char>& operator<<(std::basic_ostream<char>& os, const OSTType_Int32& lhs) {
    return os << "(" << lhs.value << ", " << lhs.min << ", " << lhs.max << ")";
}

TEST(FLE2NumericTest, RangeTest_Int32_Bounds) {
#define ASSERT_EIB(x, y, z, e)                   \
    {                                            \
        auto _ti = getTypeInfo32((x), (y), (z)); \
        ASSERT_EQ(_ti, (e));                     \
    }

    ASSERT_EIB(1, 1, 3, OSTType_Int32(0, 0, 2));
    ASSERT_EIB(0, 0, 1, OSTType_Int32(0, 0, 1));
    ASSERT_EIB(-1, -1, 0, OSTType_Int32(0, 0, 1));
    ASSERT_EIB(-2, -2, 0, OSTType_Int32(0, 0, 2));

    // min int32_t, no equivalent in positive part of integer
    ASSERT_EIB(-2147483647, -2147483648, 1, OSTType_Int32(1, 0, 2147483649));
    ASSERT_EIB(-2147483648, -2147483648, 0, OSTType_Int32(0, 0, 2147483648));
    ASSERT_EIB(0, -2147483648, 1, OSTType_Int32(2147483648, 0, 2147483649));
    ASSERT_EIB(1, -2147483648, 2, OSTType_Int32(2147483649, 0, 2147483650));

    ASSERT_EIB(2147483647, -2147483647, 2147483647, OSTType_Int32(4294967294, 0, 4294967294));
    ASSERT_EIB(2147483647, -2147483648, 2147483647, OSTType_Int32(4294967295, 0, 4294967295));

    ASSERT_EIB(15, 10, 26, OSTType_Int32(5, 0, 16));

    ASSERT_EIB(15, -10, 55, OSTType_Int32(25, 0, 65));

#undef ASSERT_EIB
}


TEST(FLE2NumericTest, RangeTest_Int32_Errors) {
    ASSERT_THROWS_CODE(getTypeInfo32(1, boost::none, 2), AssertionException, 6775001);
    ASSERT_THROWS_CODE(getTypeInfo32(1, 0, boost::none), AssertionException, 6775001);
    ASSERT_THROWS_CODE(getTypeInfo32(1, 2, 1), AssertionException, 6775002);

    ASSERT_THROWS_CODE(getTypeInfo32(1, 2, 3), AssertionException, 6775003);
    ASSERT_THROWS_CODE(getTypeInfo32(4, 2, 3), AssertionException, 6775003);

    ASSERT_THROWS_CODE(getTypeInfo32(4, -2147483648, -2147483648), AssertionException, 6775002);
}

TEST(FLE2NumericTest, RangeTest_Int64_NoBounds) {
#define ASSERT_EI(x, y) ASSERT_EQ(getTypeInfo64((x), boost::none, boost::none).value, (y));

    ASSERT_EI(9223372036854775807LL, 18446744073709551615ULL);

    ASSERT_EI(1, 9223372036854775809ULL);
    ASSERT_EI(0, 9223372036854775808ULL);
    ASSERT_EI(-1, 9223372036854775807ULL);
    ASSERT_EI(-2, 9223372036854775806ULL);
    ASSERT_EI(-9223372036854775807LL, 1);

    // min Int64_t, no equivalent in positive part of integer
    ASSERT_EI(LLONG_MIN, 0);

#undef ASSERT_EI
}

bool operator==(const OSTType_Int64& lhs, const OSTType_Int64& rhs) {
    return std::tie(lhs.value, lhs.min, lhs.max) == std::tie(rhs.value, rhs.min, rhs.max);
}

std::basic_ostream<char>& operator<<(std::basic_ostream<char>& os, const OSTType_Int64& lhs) {
    return os << "(" << lhs.value << ", " << lhs.min << ", " << lhs.max << ")";
}


TEST(FLE2NumericTest, RangeTest_Int64_Bounds) {
#define ASSERT_EIB(x, y, z, e)                   \
    {                                            \
        auto _ti = getTypeInfo64((x), (y), (z)); \
        ASSERT_EQ(_ti, (e));                     \
    }

    ASSERT_EIB(1, 1, 2, OSTType_Int64(0, 0, 1));
    ASSERT_EIB(0, 0, 1, OSTType_Int64(0, 0, 1))
    ASSERT_EIB(-1, -1, 0, OSTType_Int64(0, 0, 1))
    ASSERT_EIB(-2, -2, 0, OSTType_Int64(0, 0, 2))

    // min Int64_t, no equivalent in positive part of integer
    ASSERT_EIB(-9223372036854775807LL, LLONG_MIN, 1, OSTType_Int64(1, 0, 9223372036854775809ULL));
    ASSERT_EIB(LLONG_MIN, LLONG_MIN, 0, OSTType_Int64(0, 0, 9223372036854775808ULL));
    ASSERT_EIB(0, LLONG_MIN, 37, OSTType_Int64(9223372036854775808ULL, 0, 9223372036854775845ULL));
    ASSERT_EIB(1, LLONG_MIN, 42, OSTType_Int64(9223372036854775809ULL, 0, 9223372036854775850ULL));

    ASSERT_EIB(9223372036854775807,
               -9223372036854775807,
               9223372036854775807,
               OSTType_Int64(18446744073709551614ULL, 0, 18446744073709551614ULL));
    ASSERT_EIB(9223372036854775807,
               LLONG_MIN,
               9223372036854775807,
               OSTType_Int64(18446744073709551615ULL, 0, 18446744073709551615ULL));

    ASSERT_EIB(15, 10, 26, OSTType_Int64(5, 0, 16));

    ASSERT_EIB(15, -10, 55, OSTType_Int64(25, 0, 65));

#undef ASSERT_EIB
}

TEST(FLE2NumericTest, RangeTest_Int64_Errors) {
    ASSERT_THROWS_CODE(getTypeInfo64(1, boost::none, 2), AssertionException, 6775004);
    ASSERT_THROWS_CODE(getTypeInfo64(1, 0, boost::none), AssertionException, 6775004);
    ASSERT_THROWS_CODE(getTypeInfo64(1, 2, 1), AssertionException, 6775005);

    ASSERT_THROWS_CODE(getTypeInfo64(1, 2, 3), AssertionException, 6775006);
    ASSERT_THROWS_CODE(getTypeInfo64(4, 2, 3), AssertionException, 6775006);

    ASSERT_THROWS_CODE(getTypeInfo64(4, LLONG_MIN, LLONG_MIN), AssertionException, 6775005);
}

TEST(FLE2NumericTest, RangeTest_Double_Bounds) {
#define ASSERT_EIB(x, z) \
    ASSERT_EQ(getTypeInfoDouble((x), boost::none, boost::none, boost::none).value, (z));

    // Larger numbers map to larger uint64
    ASSERT_EIB(-1111, 4570770991734587392ULL);
    ASSERT_EIB(-111, 4585860689314185216ULL);
    ASSERT_EIB(-11, 4600989969312382976ULL);
    ASSERT_EIB(-10, 4601552919265804288ULL);
    ASSERT_EIB(-3, 4609434218613702656ULL);
    ASSERT_EIB(-2, 4611686018427387904ULL);

    ASSERT_EIB(-1, 4616189618054758400ULL);
    ASSERT_EIB(1, 13830554455654793216ULL);
    ASSERT_EIB(22, 13850257704024539136ULL);
    ASSERT_EIB(333, 13867937850999177216ULL);

    // Larger exponents map to larger uint64
    ASSERT_EIB(33E56, 14690973652625833878ULL);
    ASSERT_EIB(22E57, 14703137697061005818ULL);
    ASSERT_EIB(11E58, 14713688953586463292ULL);

    // Smaller exponents map to smaller uint64
    ASSERT_EIB(1E-6, 13740701229962882445ULL);
    ASSERT_EIB(1E-7, 13725520251343122248ULL);
    ASSERT_EIB(1E-8, 13710498295186492474ULL);
    ASSERT_EIB(1E-56, 12992711961033031890ULL);
    ASSERT_EIB(1E-57, 12977434315086142017ULL);
    ASSERT_EIB(1E-58, 12962510038552207822ULL);

    // Smaller negative exponents map to smaller uint64
    ASSERT_EIB(-1E-06, 4706042843746669171ULL);
    ASSERT_EIB(-1E-07, 4721223822366429368ULL);
    ASSERT_EIB(-1E-08, 4736245778523059142ULL);
    ASSERT_EIB(-1E-56, 5454032112676519726ULL);
    ASSERT_EIB(-1E-57, 5469309758623409599ULL);
    ASSERT_EIB(-1E-58, 5484234035157343794ULL);

    // Larger exponents map to larger uint64
    ASSERT_EIB(-33E+56, 3755770421083717738ULL);
    ASSERT_EIB(-22E+57, 3743606376648545798ULL);
    ASSERT_EIB(-11E+58, 3733055120123088324ULL);

    ASSERT_EIB(0, 9223372036854775808ULL);
    ASSERT_EIB(-0.0, 9223372036854775808ULL);

#undef ASSERT_EIB
}

TEST(FLE2NumericTest, RangeTest_Double_Bounds_Precision) {
#define ASSERT_EIBP(x, y, z) ASSERT_EQ(getTypeInfoDouble((x), -100000, 100000, y).value, (z));

    ASSERT_EIBP(3.141592653589, 1, 1000031);
    ASSERT_EIBP(3.141592653589, 2, 10000314);
    ASSERT_EIBP(3.141592653589, 3, 100003141);
    ASSERT_EIBP(3.141592653589, 4, 1000031415);
    ASSERT_EIBP(3.141592653589, 5, 10000314159);
    ASSERT_EIBP(3.141592653589, 6, 100003141592);
    ASSERT_EIBP(3.141592653589, 7, 1000031415926);
#undef ASSERT_EIBP


#define ASSERT_EIBB(v, ub, lb, prc, z)                   \
    {                                                    \
        auto _ost = getTypeInfoDouble((v), lb, ub, prc); \
        ASSERT_NE(_ost.max, 18446744073709551615ULL);    \
        ASSERT_EQ(_ost.value, z);                        \
    }
#define ASSERT_EIBB_OVERFLOW(v, ub, lb, prc, z)          \
    {                                                    \
        auto _ost = getTypeInfoDouble((v), lb, ub, prc); \
        ASSERT_EQ(_ost.max, 18446744073709551615ULL);    \
        ASSERT_EQ(_ost.value, z);                        \
    }
#define ASSERT_EIBB_ERROR(v, ub, lb, prc, code)                                     \
    {                                                                               \
        ASSERT_THROWS_CODE(getTypeInfoDouble((v), lb, ub, prc), DBException, code); \
    }

    ASSERT_EIBB(0, 1, -1, 3, 1000);
    ASSERT_EIBB(0, 1, -1E5, 3, 100000000);

    ASSERT_EIBB(-1E-33, 1, -1E5, 3, 100000000);

    ASSERT_EIBB_ERROR(
        0, std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(), 3, 9178803);

    ASSERT_EIBB(3.141592653589, 5, 0, 0, 3);
    ASSERT_EIBB(3.141592653589, 5, 0, 1, 31);

    ASSERT_EIBB(3.141592653589, 5, 0, 2, 314);

    ASSERT_EIBB(3.141592653589, 5, 0, 3, 3141);
    ASSERT_EIBB(3.141592653589, 5, 0, 16, 31415926535890000);


    ASSERT_EIBB(-5, -1, -10, 3, 5000);

    ASSERT_EIBB_ERROR(-1E100, 0, std::numeric_limits<double>::lowest(), 3, 9178804);

    ASSERT_EIBB(1E9, 1E10, 0, 3, 1000000000000);
    ASSERT_EIBB(1E9, 1E10, 0, 0, 1000000000);


    ASSERT_EIBB(-5, 10, -10, 0, 5);
    ASSERT_EIBB(-5, 10, -10, 2, 500);

    /**
     * 1E-30 and 10E-30 cannot be represented accurately as a double, so there will
     * always be some rounding errors. That means scaled_max and scaled_min will also
     * have issues with precision and will have digits after the decimal place.
     */
    ASSERT_EIBB_ERROR(1E-30, 10E-30, 1E-30, 35, 9178801);
    ASSERT_EIBB_ERROR(-1E-30, 0, -10E-30, 35, 9178802);

#undef ASSERT_EIBB
#undef ASSERT_EIBB_OVERFLOW
}

TEST(FLE2NumericTest, RangeTest_Double_Bounds_Precision_Errors) {

    ASSERT_THROWS_CODE(
        getTypeInfoDouble(1, boost::none, boost::none, 1), AssertionException, 6966803);

    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 1, 2, -1), AssertionException, 9125503);
}

TEST(FLE2NumericTest, RangeTest_Double_Errors) {
    ASSERT_THROWS_CODE(getTypeInfoDouble(1, boost::none, 2, 5), AssertionException, 6775007);
    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 0, boost::none, 5), AssertionException, 6775007);
    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 2, 1, 5), AssertionException, 6775009);

    ASSERT_THROWS_CODE(getTypeInfoDouble(1, 2, 3, 5), AssertionException, 6775010);
    ASSERT_THROWS_CODE(getTypeInfoDouble(4, 2, 3, 5), AssertionException, 6775010);


    ASSERT_THROWS_CODE(getTypeInfoDouble(std::numeric_limits<double>::infinity(), 1, 2, 5),
                       AssertionException,
                       6775008);
    ASSERT_THROWS_CODE(getTypeInfoDouble(std::numeric_limits<double>::quiet_NaN(), 1, 2, 5),
                       AssertionException,
                       6775008);
    ASSERT_THROWS_CODE(getTypeInfoDouble(std::numeric_limits<double>::signaling_NaN(), 1, 2, 5),
                       AssertionException,
                       6775008);
}


TEST(FLE2NumericTest, RangeTest_Decimal128_Bounds) {
#define ASSERT_EIB(x, z)                                                                        \
    ASSERT_EQ(                                                                                  \
        boost::multiprecision::to_string(                                                       \
            getTypeInfoDecimal128(Decimal128(x), boost::none, boost::none, boost::none).value), \
        (z));

    // Larger numbers map tw larger uint64
    ASSERT_EIB(-1234567890E7, "108549948892579231731687303715884111887");
    ASSERT_EIB(-1234567890E6, "108559948892579231731687303715884111886");
    ASSERT_EIB(-1234567890E5, "108569948892579231731687303715884111885");
    ASSERT_EIB(-1234567890E4, "108579948892579231731687303715884111884");
    ASSERT_EIB(-1234567890E3, "108589948892579231731687303715884111883");
    ASSERT_EIB(-1234567890E2, "108599948892579231731687303715884111882");
    ASSERT_EIB(-1234567890E1, "108609948892579231731687303715884111881");
    ASSERT_EIB(-123456789012345, "108569948892579108281687303715884111885");
    ASSERT_EIB(-12345678901234, "108579948892579108331687303715884111884");
    ASSERT_EIB(-1234567890123, "108589948892579108731687303715884111883");
    ASSERT_EIB(-123456789012, "108599948892579111731687303715884111882");
    ASSERT_EIB(-12345678901, "108609948892579131731687303715884111881");
    ASSERT_EIB(-1234567890, "108619948892579231731687303715884111880");
    ASSERT_EIB(-99999999, "108631183460569231731687303715884111878");
    ASSERT_EIB(-8888888, "108642294572469231731687303715884111877");
    ASSERT_EIB(-777777, "108653405690469231731687303715884111876");
    ASSERT_EIB(-66666, "108664516860469231731687303715884111875");
    ASSERT_EIB(-5555, "108675628460469231731687303715884111874");
    ASSERT_EIB(-444, "108686743460469231731687303715884111873");
    ASSERT_EIB(-334, "108687843460469231731687303715884111873");
    ASSERT_EIB(-333, "108687853460469231731687303715884111873");
    ASSERT_EIB(-44, "108696783460469231731687303715884111872");
    ASSERT_EIB(-33, "108697883460469231731687303715884111872");
    ASSERT_EIB(-22, "108698983460469231731687303715884111872");
    ASSERT_EIB(-5, "108706183460469231731687303715884111871");
    ASSERT_EIB(-4, "108707183460469231731687303715884111871");
    ASSERT_EIB(-3, "108708183460469231731687303715884111871");
    ASSERT_EIB(-2, "108709183460469231731687303715884111871");
    ASSERT_EIB(-1, "108710183460469231731687303715884111871");
    ASSERT_EIB(0, "170141183460469231731687303715884105728");
    ASSERT_EIB(1, "231572183460469231731687303715884099585");
    ASSERT_EIB(2, "231573183460469231731687303715884099585");
    ASSERT_EIB(3, "231574183460469231731687303715884099585");
    ASSERT_EIB(4, "231575183460469231731687303715884099585");
    ASSERT_EIB(5, "231576183460469231731687303715884099585");
    ASSERT_EIB(22, "231583383460469231731687303715884099584");
    ASSERT_EIB(33, "231584483460469231731687303715884099584");
    ASSERT_EIB(44, "231585583460469231731687303715884099584");
    ASSERT_EIB(333, "231594513460469231731687303715884099583");
    ASSERT_EIB(334, "231594523460469231731687303715884099583");
    ASSERT_EIB(444, "231595623460469231731687303715884099583");
    ASSERT_EIB(5555, "231606738460469231731687303715884099582");
    ASSERT_EIB(66666, "231617850060469231731687303715884099581");
    ASSERT_EIB(777777, "231628961230469231731687303715884099580");
    ASSERT_EIB(8888888, "231640072348469231731687303715884099579");
    ASSERT_EIB(33E56, "232144483460469231731687303715884099528");
    ASSERT_EIB(22E57, "232153383460469231731687303715884099527");
    ASSERT_EIB(11E58, "232162283460469231731687303715884099526");

    // Smaller exponents map to smaller uint64
    ASSERT_EIB(1E-6, "231512183460469231731687303715884099591");
    ASSERT_EIB(1E-7, "231502183460469231731687303715884099592");
    ASSERT_EIB(1E-8, "231492183460469231731687303715884099593");
    ASSERT_EIB(1E-56, "231012183460469231731687303715884099641");
    ASSERT_EIB(1E-57, "231002183460469231731687303715884099642");
    ASSERT_EIB(1E-58, "230992183460469231731687303715884099643");

    // Smaller negative exponents map to smaller uint64
    ASSERT_EIB(-1E-6, "108770183460469231731687303715884111865");
    ASSERT_EIB(-1E-7, "108780183460469231731687303715884111864");
    ASSERT_EIB(-1E-8, "108790183460469231731687303715884111863");
    ASSERT_EIB(-1E-56, "109270183460469231731687303715884111815");
    ASSERT_EIB(-1E-57, "109280183460469231731687303715884111814");
    ASSERT_EIB(-1E-58, "109290183460469231731687303715884111813");

    // Larger exponents map to larger uint64
    ASSERT_EIB(-33E56, "108137883460469231731687303715884111928");
    ASSERT_EIB(-22E57, "108128983460469231731687303715884111929");
    ASSERT_EIB(-11E58, "108120083460469231731687303715884111930");

    ASSERT_EIB(Decimal128::kLargestPositive, "293021183460469231731687303715884093440");
    ASSERT_EIB(Decimal128::kSmallestPositive, "170141183460469231731687303715884105729");
    ASSERT_EIB(Decimal128::kLargestNegative, "47261183460469231731687303715884118016");
    ASSERT_EIB(Decimal128::kSmallestNegative, "170141183460469231731687303715884105727");
    ASSERT_EIB(Decimal128::kNormalizedZero, "170141183460469231731687303715884105728");
    ASSERT_EIB(Decimal128::kLargestNegativeExponentZero, "170141183460469231731687303715884105728");

#undef ASSERT_EIB
}


TEST(FLE2NumericTest, RangeTest_Decimal128_Bounds_Precision) {

#define ASSERT_EIBP(x, y, z)                                                                    \
    ASSERT_EQ(                                                                                  \
        getTypeInfoDecimal128(Decimal128(x), Decimal128(-100000), Decimal128(100000), y).value, \
        (z));

    ASSERT_EIBP("3.141592653589E-1", 10, 1000003141592653);
    ASSERT_EIBP("31.41592653589E-2", 10, 1000003141592653);
    ASSERT_EIBP("314.1592653589E-3", 10, 1000003141592653);
    ASSERT_EIBP("3141.592653589E-4", 10, 1000003141592653);
    ASSERT_EIBP("31415.92653589E-5", 10, 1000003141592653);
    ASSERT_EIBP("314159.2653589E-6", 10, 1000003141592653);
    ASSERT_EIBP("3141592.653589E-7", 10, 1000003141592653);
    ASSERT_EIBP("31415926.53589E-8", 10, 1000003141592653);

#undef ASSERT_EIBP

#define ASSERT_EIBPL(x, y, z)                                                                   \
    ASSERT_EQ(                                                                                  \
        getTypeInfoDecimal128(Decimal128(x), Decimal128(-100000), Decimal128("1E22"), y).value, \
        boost::multiprecision::uint128_t(z));

    ASSERT_EIBPL("3.1415926535897932384626433832795E20", 5, "31415926535897942384626433");
    ASSERT_EIBPL("3.1415926535897932384626433832795E20", 6, "314159265358979423846264338");

    ASSERT_EIBPL("3.1415926535897932384626433832795E20", 7, "3141592653589794238462643383");

    ASSERT_EIBPL("3.1415926535897932384626433832795E20", 8, "31415926535897942384626433832");

#undef ASSERT_EIBP

#define ASSERT_EIBP(x, y, z)                                                                    \
    ASSERT_EQ(                                                                                  \
        getTypeInfoDecimal128(Decimal128(x), Decimal128(-100000), Decimal128(100000), y).value, \
        (z));

    ASSERT_EIBP(3.141592653589, 1, 1000031);
    ASSERT_EIBP(3.141592653589, 2, 10000314);
    ASSERT_EIBP(3.141592653589, 3, 100003141);
    ASSERT_EIBP(3.141592653589, 4, 1000031415);
    ASSERT_EIBP(3.141592653589, 5, 10000314159);
    ASSERT_EIBP(3.141592653589, 6, 100003141592);
    ASSERT_EIBP(3.141592653589, 7, 1000031415926);
#undef ASSERT_EIBP


#define ASSERT_EIBB(v, ub, lb, prc, z)                                                         \
    {                                                                                          \
        auto _ost = getTypeInfoDecimal128(Decimal128(v), Decimal128(lb), Decimal128(ub), prc); \
        ASSERT_NE(_ost.max.str(), "340282366920938463463374607431768211455");                  \
        ASSERT_EQ(_ost.value, z);                                                              \
    }

#define ASSERT_EIBB_ERROR_CODE(v, ub, lb, prc, code)                                   \
    {                                                                                  \
        ASSERT_THROWS_CODE(                                                            \
            getTypeInfoDecimal128(Decimal128(v), Decimal128(lb), Decimal128(ub), prc), \
            DBException,                                                               \
            code);                                                                     \
    }

#define ASSERT_EIBB_OVERFLOW(v, ub, lb, prc, z)                                                \
    {                                                                                          \
        auto _ost = getTypeInfoDecimal128(Decimal128(v), Decimal128(lb), Decimal128(ub), prc); \
        ASSERT_EQ(_ost.max.str(), "340282366920938463463374607431768211455");                  \
        ASSERT_EQ(_ost.value, z);                                                              \
    }

    ASSERT_EIBB(0, 1, -1, 3, 1000);
    ASSERT_EIBB(0, 1, -1E5, 3, 100000000);

    ASSERT_EIBB(-1E-33, 1, -1E5, 3, 100000000);

    ASSERT_EIBB_ERROR_CODE(
        0, Decimal128::kLargestPositive, Decimal128::kLargestNegative, 3, 9178810);
    ASSERT_EIBB_ERROR_CODE(0, 0, Decimal128::kLargestNegative, 3, 9178811);

    ASSERT_EIBB_ERROR_CODE(
        0, std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest(), 3, 9178810);

    ASSERT_EIBB(3.141592653589, 5, 0, 0, 3);
    ASSERT_EIBB(3.141592653589, 5, 0, 1, 31);

    ASSERT_EIBB(3.141592653589, 5, 0, 2, 314);

    ASSERT_EIBB(3.141592653589, 5, 0, 3, 3141);
    ASSERT_EIBB(3.141592653589, 5, 0, 16, 31415926535890000);


    ASSERT_EIBB(-5, -1, -10, 3, 5000);


    ASSERT_EIBB_ERROR_CODE(1E100,
                           std::numeric_limits<double>::max(),
                           std::numeric_limits<double>::lowest(),
                           3,
                           9178810);

    ASSERT_EIBB(1E9, 1E10, 0, 3, 1000000000000);
    ASSERT_EIBB(1E9, 1E10, 0, 0, 1000000000);


    ASSERT_EIBB(-5, 10, -10, 0, 5);
    ASSERT_EIBB(-5, 10, -10, 2, 500);


    ASSERT_EIBB(5E-30, 10E-30, 1E-30, 35, boost::multiprecision::uint128_t("400000"));

    // Test a range that requires > 64 bits.
    ASSERT_EIBB(5, "18446744073709551616", ".1", 1, 49);
    // Test a range that requires > 64 bits.
    // min has more places after the decimal than precision.
    ASSERT_EIBB_ERROR_CODE(5, "18446744073709551.616", ".01", 1, 9178808);
    ASSERT_EIBB_ERROR_CODE(5, "18446744073709551616", ".01", 1, 9178809);

#undef ASSERT_EIBB
#undef ASSERT_EIBB_ERROR_CODE
#undef ASSERT_EIBB_OVERFLOW
}

TEST(FLE2NumericTest, RangeTest_Decimal128_Errors) {
    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), boost::none, Decimal128(2), 5),
                       AssertionException,
                       6854201);
    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), Decimal128(0), boost::none, 5),
                       AssertionException,
                       6854201);
    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), Decimal128(2), Decimal128(1), 5),
                       AssertionException,
                       6854203);


    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), Decimal128(2), Decimal128(3), 5),
                       AssertionException,
                       6854204);
    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(4), Decimal128(2), Decimal128(3), 5),
                       AssertionException,
                       6854204);


    ASSERT_THROWS_CODE(
        getTypeInfoDecimal128(Decimal128::kPositiveInfinity, boost::none, boost::none, boost::none),
        AssertionException,
        6854202);
    ASSERT_THROWS_CODE(
        getTypeInfoDecimal128(Decimal128::kNegativeInfinity, boost::none, boost::none, boost::none),
        AssertionException,
        6854202);

    ASSERT_THROWS_CODE(
        getTypeInfoDecimal128(Decimal128::kPositiveNaN, boost::none, boost::none, boost::none),
        AssertionException,
        6854202);

    ASSERT_THROWS_CODE(
        getTypeInfoDecimal128(Decimal128::kNegativeNaN, boost::none, boost::none, boost::none),
        AssertionException,
        6854202);
}


TEST(FLE2NumericTest, RangeTest_Decimal128_Bounds_Precision_Errors) {

    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), boost::none, boost::none, 1),
                       AssertionException,
                       6966804);

    ASSERT_THROWS_CODE(getTypeInfoDecimal128(Decimal128(1), Decimal128(1), Decimal128(2), -1),
                       AssertionException,
                       9125501);
}


void roundTripDecimal128_Int128(std::string dec_str) {
    Decimal128 dec(dec_str);

    auto ret = toUInt128FromDecimal128(dec);

    Decimal128 roundTrip(ret.str());
    ASSERT(roundTrip == dec);
}

TEST(FLE2NumericTest, RangeTest_Decimal128_to_Int128) {
    roundTripDecimal128_Int128("0");
    roundTripDecimal128_Int128("123");
    roundTripDecimal128_Int128("40000000");
    roundTripDecimal128_Int128("40000000.00");
    roundTripDecimal128_Int128("40000000.00000");
    roundTripDecimal128_Int128("40000000.000000000000");

    roundTripDecimal128_Int128("40000.000E5");
    roundTripDecimal128_Int128("40000000E10");
    roundTripDecimal128_Int128("40000.000E10");
    roundTripDecimal128_Int128("40000000E20");
    roundTripDecimal128_Int128("40000.000E20");
    roundTripDecimal128_Int128("40000000E30");
    roundTripDecimal128_Int128("40000.000E30");
    roundTripDecimal128_Int128("4E37");
}

}  // namespace mongo
