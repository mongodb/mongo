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

#include "mongo/db/storage/key_string/key_string_test_util.h"

#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/future.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::key_string_test {

Ordering ALL_ASCENDING = Ordering::make(BSONObj());
Ordering ONE_ASCENDING = Ordering::make(BSON("a" << 1));
Ordering ONE_DESCENDING = Ordering::make(BSON("a" << -1));

std::random_device rd;
std::mt19937_64 seedGen(rd());

unsigned newSeed() {
    unsigned int seed = seedGen();  // Replace by the reported number to repeat test execution.
    LOGV2(22232, "Initializing random number generator using seed {seed}", "seed"_attr = seed);
    return seed;
}

void KeyStringBuilderTest::run() {
    try {
        version = key_string::Version::V0;
        unittest::Test::run();
        version = key_string::Version::V1;
        unittest::Test::run();
    } catch (...) {
        LOGV2(22226,
              "exception while testing KeyStringBuilder version "
              "{mongo_KeyString_keyStringVersionToString_version}",
              "mongo_KeyString_keyStringVersionToString_version"_attr =
                  key_string::keyStringVersionToString(version));
        throw;
    }
}

BSONObj toBson(const key_string::Builder& ks, Ordering ord) {
    return key_string::toBson(ks.getView(), ord, ks.getTypeBits());
}

BSONObj toBsonAndCheckKeySize(const key_string::Value& ks, Ordering ord) {
    auto KeyStringSize = ks.getSize();

    // Validate size of the key in key_string::Value.
    ASSERT_EQUALS(KeyStringSize, key_string::getKeySize(ks.getView(), ord, ks.getVersion()));
    return key_string::toBson(ks.getView(), ord, ks.getTypeBits());
}

const std::vector<BSONObj>& getInterestingElements(key_string::Version version) {
    static std::vector<BSONObj> elements;
    elements.clear();

    // These are used to test strings that include NUL bytes.
    const auto ball = "ball"_sd;
    const auto ball00n = "ball\0\0n"_sd;
    const auto zeroBall = "\0ball"_sd;

    elements.push_back(BSON("" << 1));
    elements.push_back(BSON("" << 1.0));
    elements.push_back(BSON("" << 1LL));
    elements.push_back(BSON("" << 123456789123456789LL));
    elements.push_back(BSON("" << -123456789123456789LL));
    elements.push_back(BSON("" << 112353998331165715LL));
    elements.push_back(BSON("" << 112353998331165710LL));
    elements.push_back(BSON("" << 1123539983311657199LL));
    elements.push_back(BSON("" << 123456789123456789.123));
    elements.push_back(BSON("" << -123456789123456789.123));
    elements.push_back(BSON("" << 112353998331165715.0));
    elements.push_back(BSON("" << 112353998331165710.0));
    elements.push_back(BSON("" << 1123539983311657199.0));
    elements.push_back(BSON("" << 5.0));
    elements.push_back(BSON("" << 5));
    elements.push_back(BSON("" << 2));
    elements.push_back(BSON("" << -2));
    elements.push_back(BSON("" << -2.2));
    elements.push_back(BSON("" << -12312312.2123123123123));
    elements.push_back(BSON("" << 12312312.2123123123123));
    elements.push_back(BSON("" << "aaa"));
    elements.push_back(BSON("" << "AAA"));
    elements.push_back(BSON("" << zeroBall));
    elements.push_back(BSON("" << ball));
    elements.push_back(BSON("" << ball00n));
    elements.push_back(BSON("" << BSONSymbol(zeroBall)));
    elements.push_back(BSON("" << BSONSymbol(ball)));
    elements.push_back(BSON("" << BSONSymbol(ball00n)));
    elements.push_back(BSON("" << BSON("a" << 5)));
    elements.push_back(BSON("" << BSON("a" << 6)));
    elements.push_back(BSON("" << BSON("b" << 6)));
    elements.push_back(BSON("" << BSON_ARRAY("a" << 5)));
    elements.push_back(BSON("" << BSONNULL));
    elements.push_back(BSON("" << BSONUndefined));
    elements.push_back(BSON("" << OID("abcdefabcdefabcdefabcdef")));
    elements.push_back(BSON("" << Date_t::fromMillisSinceEpoch(123)));
    elements.push_back(BSON("" << BSONCode("abc_code")));
    elements.push_back(BSON("" << BSONCode(zeroBall)));
    elements.push_back(BSON("" << BSONCode(ball)));
    elements.push_back(BSON("" << BSONCode(ball00n)));
    elements.push_back(BSON("" << BSONCodeWScope("def_code1", BSON("x_scope" << "a"))));
    elements.push_back(BSON("" << BSONCodeWScope("def_code2", BSON("x_scope" << "a"))));
    elements.push_back(BSON("" << BSONCodeWScope("def_code2", BSON("x_scope" << "b"))));
    elements.push_back(BSON("" << BSONCodeWScope(zeroBall, BSON("a" << 1))));
    elements.push_back(BSON("" << BSONCodeWScope(ball, BSON("a" << 1))));
    elements.push_back(BSON("" << BSONCodeWScope(ball00n, BSON("a" << 1))));
    elements.push_back(BSON("" << true));
    elements.push_back(BSON("" << false));

    // Something that needs multiple bytes of typeBits
    elements.push_back(BSON("" << BSON_ARRAY("" << BSONSymbol("") << 0 << 0ll << 0.0 << -0.0)));
    if (version != key_string::Version::V0) {
        // Something with exceptional typeBits for Decimal
        elements.push_back(
            BSON("" << BSON_ARRAY("" << BSONSymbol("") << Decimal128::kNegativeInfinity
                                     << Decimal128::kPositiveInfinity << Decimal128::kPositiveNaN
                                     << Decimal128("0.0000000") << Decimal128("-0E1000"))));
    }

    //
    // Interesting numeric cases
    //

    elements.push_back(BSON("" << 0));
    elements.push_back(BSON("" << 0ll));
    elements.push_back(BSON("" << 0.0));
    elements.push_back(BSON("" << -0.0));
    if (version != key_string::Version::V0) {
        Decimal128("0.0.0000000");
        Decimal128("-0E1000");
    }

    elements.push_back(BSON("" << std::numeric_limits<double>::quiet_NaN()));
    elements.push_back(BSON("" << std::numeric_limits<double>::infinity()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::infinity()));
    elements.push_back(BSON("" << std::numeric_limits<double>::max()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::max()));
    elements.push_back(BSON("" << std::numeric_limits<double>::min()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::min()));
    elements.push_back(BSON("" << std::numeric_limits<double>::denorm_min()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::denorm_min()));
    elements.push_back(BSON("" << std::numeric_limits<double>::denorm_min()));
    elements.push_back(BSON("" << -std::numeric_limits<double>::denorm_min()));

    elements.push_back(BSON("" << std::numeric_limits<long long>::max()));
    elements.push_back(BSON("" << -std::numeric_limits<long long>::max()));
    elements.push_back(BSON("" << std::numeric_limits<long long>::min()));

    elements.push_back(BSON("" << std::numeric_limits<int>::max()));
    elements.push_back(BSON("" << -std::numeric_limits<int>::max()));
    elements.push_back(BSON("" << std::numeric_limits<int>::min()));

    for (int powerOfTwo = 0; powerOfTwo < 63; powerOfTwo++) {
        const long long lNum = 1ll << powerOfTwo;
        const double dNum = double(lNum);

        // All powers of two in this range can be represented exactly as doubles.
        invariant(lNum == static_cast<long long>(dNum));

        elements.push_back(BSON("" << lNum));
        elements.push_back(BSON("" << -lNum));

        elements.push_back(BSON("" << dNum));
        elements.push_back(BSON("" << -dNum));


        elements.push_back(BSON("" << (lNum + 1)));
        elements.push_back(BSON("" << (lNum - 1)));
        elements.push_back(BSON("" << (-lNum + 1)));
        elements.push_back(BSON("" << (-lNum - 1)));

        if (powerOfTwo <= 52) {  // is dNum - 0.5 representable?
            elements.push_back(BSON("" << (dNum - 0.5)));
            elements.push_back(BSON("" << -(dNum - 0.5)));
            elements.push_back(BSON("" << (dNum - 0.1)));
            elements.push_back(BSON("" << -(dNum - 0.1)));
        }

        if (powerOfTwo <= 51) {  // is dNum + 0.5 representable?
            elements.push_back(BSON("" << (dNum + 0.5)));
            elements.push_back(BSON("" << -(dNum + 0.5)));
            elements.push_back(BSON("" << (dNum + 0.1)));
            elements.push_back(BSON("" << -(dNum + -.1)));
        }

        if (version != key_string::Version::V0) {
            const Decimal128 dec(static_cast<int64_t>(lNum));
            const Decimal128 one("1");
            const Decimal128 half("0.5");
            const Decimal128 tenth("0.1");
            elements.push_back(BSON("" << dec));
            elements.push_back(BSON("" << dec.add(one)));
            elements.push_back(BSON("" << dec.subtract(one)));
            elements.push_back(BSON("" << dec.negate()));
            elements.push_back(BSON("" << dec.add(one).negate()));
            elements.push_back(BSON("" << dec.subtract(one).negate()));
            elements.push_back(BSON("" << dec.subtract(half)));
            elements.push_back(BSON("" << dec.subtract(half).negate()));
            elements.push_back(BSON("" << dec.add(half)));
            elements.push_back(BSON("" << dec.add(half).negate()));
            elements.push_back(BSON("" << dec.subtract(tenth)));
            elements.push_back(BSON("" << dec.subtract(tenth).negate()));
            elements.push_back(BSON("" << dec.add(tenth)));
            elements.push_back(BSON("" << dec.add(tenth).negate()));
        }
    }

    {
        // Numbers around +/- numeric_limits<long long>::max() which can't be represented
        // precisely as a double.
        const long long maxLL = std::numeric_limits<long long>::max();
        const double closestAbove = 9223372036854775808.0;  // 2**63
        const double closestBelow = 9223372036854774784.0;  // 2**63 - epsilon

        elements.push_back(BSON("" << maxLL));
        elements.push_back(BSON("" << (maxLL - 1)));
        elements.push_back(BSON("" << closestAbove));
        elements.push_back(BSON("" << closestBelow));

        elements.push_back(BSON("" << -maxLL));
        elements.push_back(BSON("" << -(maxLL - 1)));
        elements.push_back(BSON("" << -closestAbove));
        elements.push_back(BSON("" << -closestBelow));
    }

    {
        // Numbers around numeric_limits<long long>::min() which can be represented precisely as
        // a double, but not as a positive long long.
        const long long minLL = std::numeric_limits<long long>::min();
        const double closestBelow = -9223372036854777856.0;  // -2**63 - epsilon
        const double equal = -9223372036854775808.0;         // 2**63
        const double closestAbove = -9223372036854774784.0;  // -2**63 + epsilon

        elements.push_back(BSON("" << minLL));
        elements.push_back(BSON("" << equal));
        elements.push_back(BSON("" << closestAbove));
        elements.push_back(BSON("" << closestBelow));
    }

    if (version != key_string::Version::V0) {
        // Numbers that are hard to round to between binary and decimal.
        elements.push_back(BSON("" << 0.1));
        elements.push_back(BSON("" << Decimal128("0.100000000")));
        // Decimals closest to the double representation of 0.1.
        elements.push_back(BSON("" << Decimal128("0.1000000000000000055511151231257827")));
        elements.push_back(BSON("" << Decimal128("0.1000000000000000055511151231257828")));

        // Decimals that failed at some point during testing.
        elements.push_back(BSON("" << Decimal128("0.999999999999999")));
        elements.push_back(BSON("" << Decimal128("2.22507385850721E-308")));
        elements.push_back(BSON("" << Decimal128("9.881312916824930883531375857364428E-324")));
        elements.push_back(BSON("" << Decimal128(9223372036854776000.0)));
        elements.push_back(BSON("" << Decimal128("9223372036854776000")));

        // Numbers close to numerical underflow/overflow for double.
        elements.push_back(BSON("" << Decimal128("1.797693134862315708145274237317044E308")));
        elements.push_back(BSON("" << Decimal128("1.797693134862315708145274237317043E308")));
        elements.push_back(BSON("" << Decimal128("-1.797693134862315708145274237317044E308")));
        elements.push_back(BSON("" << Decimal128("-1.797693134862315708145274237317043E308")));
        elements.push_back(BSON("" << Decimal128("9.881312916824930883531375857364427")));
        elements.push_back(BSON("" << Decimal128("9.881312916824930883531375857364428")));
        elements.push_back(BSON("" << Decimal128("-9.881312916824930883531375857364427")));
        elements.push_back(BSON("" << Decimal128("-9.881312916824930883531375857364428")));
        elements.push_back(BSON("" << Decimal128("4.940656458412465441765687928682213E-324")));
        elements.push_back(BSON("" << Decimal128("4.940656458412465441765687928682214E-324")));
        elements.push_back(BSON("" << Decimal128("-4.940656458412465441765687928682214E-324")));
        elements.push_back(BSON("" << Decimal128("-4.940656458412465441765687928682213E-324")));

        // Non-finite values. Note: can't roundtrip negative NaNs, so not testing here.
        elements.push_back(BSON("" << Decimal128::kPositiveNaN));
        elements.push_back(BSON("" << Decimal128::kNegativeInfinity));
        elements.push_back(BSON("" << Decimal128::kPositiveInfinity));
    }

    // Tricky double precision number for binary/decimal conversion: very close to a decimal
    if (version != key_string::Version::V0)
        elements.push_back(BSON("" << Decimal128("3743626360493413E-165")));
    elements.push_back(BSON("" << 3743626360493413E-165));

    return elements;
}

void testPermutation(key_string::Version version,
                     const std::vector<BSONObj>& elementsOrig,
                     const std::vector<BSONObj>& orderings,
                     bool debug) {
    // Since key_string::Builders are compared using memcmp we can assume it provides a total
    // ordering such that there won't be cases where (a < b && b < c && !(a < c)). This test still
    // needs to ensure that it provides the *correct* total ordering.
    std::vector<stdx::future<void>> futures;
    for (size_t k = 0; k < orderings.size(); k++) {
        futures.push_back(
            stdx::async(stdx::launch::async, [k, version, elementsOrig, orderings, debug] {
                BSONObj orderObj = orderings[k];
                Ordering ordering = Ordering::make(orderObj);
                if (debug)
                    LOGV2(22229, "ordering: {orderObj}", "orderObj"_attr = orderObj);

                std::vector<BSONObj> elements = elementsOrig;
                BSONObjComparator bsonCmp(orderObj,
                                          BSONObjComparator::FieldNamesMode::kConsider,
                                          &simpleStringDataComparator);
                std::stable_sort(elements.begin(), elements.end(), bsonCmp.makeLessThan());

                for (size_t i = 0; i < elements.size(); i++) {
                    const BSONObj& o1 = elements[i];
                    if (debug)
                        LOGV2(22230, "\to1: {o1}", "o1"_attr = o1);
                    ROUNDTRIP_ORDER(version, o1, ordering);

                    key_string::Builder k1(version, o1, ordering);

                    if (i + 1 < elements.size()) {
                        const BSONObj& o2 = elements[i + 1];
                        if (debug)
                            LOGV2(22231, "\t\t o2: {o2}", "o2"_attr = o2);
                        key_string::Builder k2(version, o2, ordering);

                        int bsonCmp = o1.woCompare(o2, ordering);
                        invariant(bsonCmp <= 0);  // We should be sorted...

                        if (bsonCmp == 0) {
                            ASSERT_EQ(k1, k2);
                        } else {
                            ASSERT_LT(k1, k2);
                        }

                        // Test the query encodings using kLess and kGreater
                        int firstElementComp = o1.firstElement().woCompare(o2.firstElement());
                        if (ordering.descending(1))
                            firstElementComp = -firstElementComp;

                        invariant(firstElementComp <= 0);
                    }
                }
            }));
    }
    for (auto&& future : futures) {
        future.get();
    }
}

std::vector<BSONObj> thinElements(std::vector<BSONObj> elements,
                                  unsigned seed,
                                  size_t maxElements) {
    std::mt19937_64 gen(seed);

    if (elements.size() <= maxElements)
        return elements;

    LOGV2(22233,
          "only keeping {maxElements} of {elements_size} elements using random selection",
          "maxElements"_attr = maxElements,
          "elements_size"_attr = elements.size());
    std::shuffle(elements.begin(), elements.end(), gen);
    elements.resize(maxElements);
    return elements;
}

RecordId ridFromOid(const OID& oid) {
    key_string::Builder builder(key_string::Version::kLatestVersion);
    builder.appendOID(oid);
    return RecordId(builder.getView());
}

RecordId ridFromStr(StringData str) {
    key_string::Builder builder(key_string::Version::kLatestVersion);
    builder.appendString(str);
    return RecordId(builder.getView());
}

int compareLongToDouble(long long lhs, double rhs) {
    if (rhs >= static_cast<double>(std::numeric_limits<long long>::max()))
        return -1;
    if (rhs < std::numeric_limits<long long>::min())
        return 1;

    if (fabs(rhs) >= (1LL << 52)) {
        return COMPARE_HELPER(lhs, static_cast<long long>(rhs));
    }

    return COMPARE_HELPER(static_cast<double>(lhs), rhs);
}

int compareNumbers(const BSONElement& lhs, const BSONElement& rhs) {
    invariant(lhs.isNumber());
    invariant(rhs.isNumber());

    if (lhs.type() == BSONType::numberInt || lhs.type() == BSONType::numberLong) {
        if (rhs.type() == BSONType::numberInt || rhs.type() == BSONType::numberLong) {
            return COMPARE_HELPER(lhs.numberLong(), rhs.numberLong());
        }
        return compareLongToDouble(lhs.numberLong(), rhs.Double());
    } else {  // double
        if (rhs.type() == BSONType::numberDouble) {
            return COMPARE_HELPER(lhs.Double(), rhs.Double());
        }
        return -compareLongToDouble(rhs.numberLong(), lhs.Double());
    }
}

BSONObj buildKeyWhichWillHaveNByteOfTypeBits(size_t n, bool allZeros) {
    int numItems = n * 8 / 2 /* kInt/kDouble needs two bits */;

    BSONObj obj;
    BSONArrayBuilder array;
    for (int i = 0; i < numItems; i++)
        if (allZeros)
            array.append(123); /* kInt uses 00 */
        else
            array.append(1.2); /* kDouble uses 10 */

    obj = BSON("" << array.arr());
    return obj;
}

void checkKeyWithNByteOfTypeBits(key_string::Version version, size_t n, bool allZeros) {
    const BSONObj orig = buildKeyWhichWillHaveNByteOfTypeBits(n, allZeros);
    const key_string::Builder ks(version, orig, ALL_ASCENDING);
    const size_t typeBitsSize = ks.getTypeBits().getSize();
    if (n == 1 || allZeros) {
        // Case 1&2
        // Case 2: Since we use kDouble, TypeBits="01010101" when n=1. The size
        // is thus 1.
        ASSERT_EQ(1u, typeBitsSize);
    } else if (n <= 127) {
        // Case 3
        ASSERT_EQ(n + 1, typeBitsSize);
    } else {
        // Case 4
        ASSERT_EQ(n + 5, typeBitsSize);
    }
    const BSONObj converted = toBsonAndCheckKeySize(ks, ALL_ASCENDING);
    ASSERT_BSONOBJ_EQ(converted, orig);
    ASSERT(converted.binaryEqual(orig));

    // Also test TypeBits::fromBuffer()
    BufReader bufReader(ks.getTypeBits().getBuffer(), typeBitsSize);
    key_string::TypeBits newTypeBits = key_string::TypeBits::fromBuffer(version, &bufReader);
    ASSERT_EQ(hexblob::encode(newTypeBits.getBuffer(), newTypeBits.getSize()),
              hexblob::encode(ks.getTypeBits().getBuffer(), ks.getTypeBits().getSize()));
}

void perfTest(key_string::Version version, const Numbers& numbers) {
    uint64_t micros = 0;
    uint64_t iters;
    // Ensure at least 16 iterations are done and at least 50 milliseconds is timed
    for (iters = 16; iters < (1 << 30) && micros < kMinPerfMicros; iters *= 2) {
        // Measure the number of loops
        Timer t;

        for (uint64_t i = 0; i < iters; i++)
            for (const auto& item : numbers) {
                // Assuming there are sufficient invariants in the to/from key_string::Builder
                // methods
                // that calls will not be optimized away.
                const key_string::Builder ks(version, item, ALL_ASCENDING);
                const BSONObj& converted = toBson(ks, ALL_ASCENDING);
                invariant(converted.binaryEqual(item));
            }

        micros = t.micros();
    }

    auto minmax = std::minmax_element(
        numbers.begin(), numbers.end(), SimpleBSONObjComparator::kInstance.makeLessThan());

    LOGV2(22236,
          "{_1E3_micros_static_cast_double_iters_numbers_size} ns per "
          "{mongo_KeyString_keyStringVersionToString_version} roundtrip{kDebugBuild_DEBUG_BUILD} "
          "min {minmax_first}, max{minmax_second}",
          "_1E3_micros_static_cast_double_iters_numbers_size"_attr =
              1E3 * micros / static_cast<double>(iters * numbers.size()),
          "mongo_KeyString_keyStringVersionToString_version"_attr =
              mongo::key_string::keyStringVersionToString(version),
          "kDebugBuild_DEBUG_BUILD"_attr = (kDebugBuild ? " (DEBUG BUILD!)" : ""),
          "minmax_first"_attr = (*minmax.first)[""],
          "minmax_second"_attr = (*minmax.second)[""]);
}
}  // namespace mongo::key_string_test
