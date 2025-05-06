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
#include "mongo/logv2/log.h"
#include "mongo/stdx/future.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest
namespace mongo::key_string_test {

TEST_F(KeyStringBuilderTest, CommonIntPerf) {
    // Exponential distribution, so skewed towards smaller integers.
    std::mt19937 gen(newSeed());
    std::exponential_distribution<double> expReal(1e-3);

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(BSON("" << static_cast<int>(expReal(gen))));

    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, UniformInt64Perf) {
    std::vector<BSONObj> numbers;
    std::mt19937 gen(newSeed());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(BSON("" << uniformInt64(gen)));

    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, CommonDoublePerf) {
    std::mt19937 gen(newSeed());
    std::exponential_distribution<double> expReal(1e-3);

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(BSON("" << expReal(gen)));

    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, UniformDoublePerf) {
    std::vector<BSONObj> numbers;
    std::mt19937 gen(newSeed());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    for (uint64_t x = 0; x < kMinPerfSamples; x++) {
        uint64_t u = uniformInt64(gen);
        double d;
        memcpy(&d, &u, sizeof(d));
        if (std::isnormal(d))
            numbers.push_back(BSON("" << d));
    }
    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, CommonDecimalPerf) {
    std::mt19937 gen(newSeed());
    std::exponential_distribution<double> expReal(1e-3);

    if (version == key_string::Version::V0)
        return;

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++)
        numbers.push_back(
            BSON("" << Decimal128(
                           expReal(gen), Decimal128::kRoundTo34Digits, Decimal128::kRoundTiesToAway)
                           .quantize(Decimal128("0.01", Decimal128::kRoundTiesToAway))));

    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, UniformDecimalPerf) {
    std::mt19937 gen(newSeed());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    if (version == key_string::Version::V0)
        return;

    std::vector<BSONObj> numbers;
    for (uint64_t x = 0; x < kMinPerfSamples; x++) {
        uint64_t hi = uniformInt64(gen);
        uint64_t lo = uniformInt64(gen);
        Decimal128 d(Decimal128::Value{lo, hi});
        if (!d.isZero() && !d.isNaN() && !d.isInfinite())
            numbers.push_back(BSON("" << d));
    }
    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, DecimalFromUniformDoublePerf) {
    std::vector<BSONObj> numbers;
    std::mt19937 gen(newSeed());
    std::uniform_int_distribution<long long> uniformInt64(std::numeric_limits<long long>::min(),
                                                          std::numeric_limits<long long>::max());

    if (version == key_string::Version::V0)
        return;

    // In addition to serve as a data ponit for performance, this test also generates many decimal
    // values close to binary floating point numbers, so edge cases around 15-digit approximations
    // get extra randomized coverage over time.
    for (uint64_t x = 0; x < kMinPerfSamples; x++) {
        uint64_t u = uniformInt64(gen);
        double d;
        memcpy(&d, &u, sizeof(d));
        if (!std::isnan(d)) {
            Decimal128::RoundingMode mode =
                x & 1 ? Decimal128::kRoundTowardPositive : Decimal128::kRoundTowardNegative;
            Decimal128::RoundingPrecision prec =
                x & 2 ? Decimal128::kRoundTo15Digits : Decimal128::kRoundTo34Digits;
            numbers.push_back(BSON("" << Decimal128(d, prec, mode)));
        }
    }
    perfTest(version, numbers);
}

TEST_F(KeyStringBuilderTest, AllPermCompare) {
    std::vector<BSONObj> elements = getInterestingElements(version);

    for (size_t i = 0; i < elements.size(); i++) {
        const BSONObj& o = elements[i];
        ROUNDTRIP(version, o);
    }

    std::vector<BSONObj> orderings;
    orderings.push_back(BSON("a" << 1));
    orderings.push_back(BSON("a" << -1));

    testPermutation(version, elements, orderings, false);
}

TEST_F(KeyStringBuilderTest, AllPerm2Compare) {
    std::vector<BSONObj> baseElements = getInterestingElements(version);
    auto seed = newSeed();

    // Select only a small subset of elements, as the combination is quadratic.
    // We want to select two subsets independently, so all combinations will get tested eventually.
    // kMaxPermElements is the desired number of elements to pass to testPermutation.
    const size_t kMaxPermElements = kDebugBuild ? 100'000 : 500'000;
    size_t maxElements = sqrt(kMaxPermElements);
    auto firstElements = thinElements(baseElements, seed, maxElements);
    auto secondElements = thinElements(baseElements, seed + 1, maxElements);

    std::vector<BSONObj> elements;
    for (size_t i = 0; i < firstElements.size(); i++) {
        for (size_t j = 0; j < secondElements.size(); j++) {
            BSONObjBuilder b;
            b.appendElements(firstElements[i]);
            b.appendElements(secondElements[j]);
            BSONObj o = b.obj();
            elements.push_back(o);
        }
    }

    LOGV2(22234,
          "AllPerm2Compare {keyStringVersionToString_version} size:{elements_size}",
          "keyStringVersionToString_version"_attr = keyStringVersionToString(version),
          "elements_size"_attr = elements.size());

    for (size_t i = 0; i < elements.size(); i++) {
        const BSONObj& o = elements[i];
        ROUNDTRIP(version, o);
    }

    std::vector<BSONObj> orderings;
    orderings.push_back(BSON("a" << 1 << "b" << 1));
    orderings.push_back(BSON("a" << -1 << "b" << 1));
    orderings.push_back(BSON("a" << 1 << "b" << -1));
    orderings.push_back(BSON("a" << -1 << "b" << -1));

    testPermutation(version, elements, orderings, false);
}

TEST_F(KeyStringBuilderTest, LotsOfNumbers3) {
    std::vector<stdx::future<void>> futures;

    for (double k = 0; k < 8; k++) {
        futures.push_back(stdx::async(stdx::launch::async, [k, this] {
            for (double i = -1100; i < 1100; i++) {
                for (double j = 0; j < 52; j++) {
                    const auto V1 = key_string::Version::V1;
                    Decimal128::RoundingPrecision roundingPrecisions[]{
                        Decimal128::kRoundTo15Digits, Decimal128::kRoundTo34Digits};
                    Decimal128::RoundingMode roundingModes[]{Decimal128::kRoundTowardNegative,
                                                             Decimal128::kRoundTowardPositive};
                    double x = pow(2, i);
                    double y = pow(2, i - j);
                    double z = pow(2, i - 53 + k);
                    double bin = x + y - z;

                    // In general NaNs don't roundtrip as we only store a single NaN, see the NaNs
                    // test.
                    if (std::isnan(bin))
                        continue;

                    ROUNDTRIP(version, BSON("" << bin));
                    ROUNDTRIP(version, BSON("" << -bin));

                    if (version < V1)
                        continue;

                    for (auto precision : roundingPrecisions) {
                        for (auto mode : roundingModes) {
                            Decimal128 rounded = Decimal128(bin, precision, mode);
                            ROUNDTRIP(V1, BSON("" << rounded));
                            ROUNDTRIP(V1, BSON("" << rounded.negate()));
                        }
                    }
                }
            }
        }));
    }
    for (auto&& future : futures) {
        future.get();
    }
}

TEST_F(KeyStringBuilderTest, NumberOrderLots) {
    std::vector<BSONObj> numbers;
    {
        numbers.push_back(BSON("" << 0));
        numbers.push_back(BSON("" << 0.0));
        numbers.push_back(BSON("" << -0.0));

        numbers.push_back(BSON("" << std::numeric_limits<long long>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<long long>::max()));
        numbers.push_back(BSON("" << static_cast<double>(std::numeric_limits<long long>::min())));
        numbers.push_back(BSON("" << static_cast<double>(std::numeric_limits<long long>::max())));
        numbers.push_back(BSON("" << std::numeric_limits<double>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<double>::max()));
        numbers.push_back(BSON("" << std::numeric_limits<int>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<int>::max()));
        numbers.push_back(BSON("" << std::numeric_limits<short>::min()));
        numbers.push_back(BSON("" << std::numeric_limits<short>::max()));

        for (int i = 0; i < 64; i++) {
            int64_t x = 1LL << i;
            numbers.push_back(BSON("" << static_cast<long long>(x)));
            numbers.push_back(BSON("" << static_cast<int>(x)));
            numbers.push_back(BSON("" << static_cast<double>(x)));
            numbers.push_back(BSON("" << (static_cast<double>(x) + .1)));

            numbers.push_back(BSON("" << (static_cast<long long>(x) + 1)));
            numbers.push_back(BSON("" << (static_cast<int>(x) + 1)));
            numbers.push_back(BSON("" << (static_cast<double>(x) + 1)));
            numbers.push_back(BSON("" << (static_cast<double>(x) + 1.1)));

            // Avoid negating signed integral minima
            if (i < 63)
                numbers.push_back(BSON("" << -static_cast<long long>(x)));

            if (i < 31)
                numbers.push_back(BSON("" << -static_cast<int>(x)));

            numbers.push_back(BSON("" << -static_cast<double>(x)));
            numbers.push_back(BSON("" << -(static_cast<double>(x) + .1)));

            numbers.push_back(BSON("" << -(static_cast<long long>(x) + 1)));
            numbers.push_back(BSON("" << -(static_cast<int>(x) + 1)));
            numbers.push_back(BSON("" << -(static_cast<double>(x) + 1)));
            numbers.push_back(BSON("" << -(static_cast<double>(x) + 1.1)));
        }

        for (double i = 0; i < 1000; i++) {
            double x = pow(2.1, i);
            numbers.push_back(BSON("" << x));
        }
    }

    Ordering ordering = Ordering::make(BSON("a" << 1));

    std::vector<std::unique_ptr<key_string::Builder>> KeyStringBuilders;
    for (size_t i = 0; i < numbers.size(); i++) {
        KeyStringBuilders.push_back(
            std::make_unique<key_string::Builder>(version, numbers[i], ordering));
    }

    for (size_t i = 0; i < numbers.size(); i++) {
        for (size_t j = 0; j < numbers.size(); j++) {
            const key_string::Builder& a = *KeyStringBuilders[i];
            const key_string::Builder& b = *KeyStringBuilders[j];
            ASSERT_EQUALS(a.compare(b), -b.compare(a));

            if (a.compare(b) !=
                compareNumbers(numbers[i].firstElement(), numbers[j].firstElement())) {
                LOGV2(22235,
                      "{numbers_i} {numbers_j}",
                      "numbers_i"_attr = numbers[i],
                      "numbers_j"_attr = numbers[j]);
            }

            ASSERT_EQUALS(a.compare(b),
                          compareNumbers(numbers[i].firstElement(), numbers[j].firstElement()));
        }
    }
}
}  // namespace mongo::key_string_test
