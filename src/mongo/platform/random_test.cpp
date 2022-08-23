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


#include <set>
#include <vector>

#include "mongo/platform/random.h"

#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

TEST(RandomTest, Seed1) {
    PseudoRandom a(12);
    PseudoRandom b(12);

    for (int i = 0; i < 100; i++) {
        ASSERT_EQUALS(a.nextInt32(), b.nextInt32());
    }
}

TEST(RandomTest, Seed2) {
    PseudoRandom a(12);
    PseudoRandom b(12);

    for (int i = 0; i < 100; i++) {
        ASSERT_EQUALS(a.nextInt64(), b.nextInt64());
    }
}

TEST(RandomTest, Seed3) {
    PseudoRandom a(11);
    PseudoRandom b(12);

    ASSERT_NOT_EQUALS(a.nextInt32(), b.nextInt32());
}

TEST(RandomTest, Seed4) {
    PseudoRandom a(11);
    std::set<int32_t> s;
    for (int i = 0; i < 100; i++) {
        s.insert(a.nextInt32());
    }
    ASSERT_EQUALS(100U, s.size());
}

TEST(RandomTest, Seed5) {
    const int64_t seed = 0xCC453456FA345FABLL;
    PseudoRandom a(seed);
    std::set<int32_t> s;
    for (int i = 0; i < 100; i++) {
        s.insert(a.nextInt32());
    }
    ASSERT_EQUALS(100U, s.size());
}

TEST(RandomTest, R1) {
    PseudoRandom a(11);
    std::set<int32_t> s;
    for (int i = 0; i < 100; i++) {
        s.insert(a.nextInt32());
    }
    ASSERT_EQUALS(100U, s.size());
}

TEST(RandomTest, R2) {
    PseudoRandom a(11);
    std::set<int64_t> s;
    for (int i = 0; i < 100; i++) {
        s.insert(a.nextInt64());
    }
    ASSERT_EQUALS(100U, s.size());
}

/**
 * Test that if two PsuedoRandom's have the same seed, then subsequent calls to
 * nextCanonicalDouble() will return the same value.
 */
TEST(RandomTest, NextCanonicalSameSeed) {
    PseudoRandom a(12);
    PseudoRandom b(12);
    for (int i = 0; i < 100; i++) {
        ASSERT_EQUALS(a.nextCanonicalDouble(), b.nextCanonicalDouble());
    }
}

/**
 * Test that if two PsuedoRandom's have different seeds, then nextCanonicalDouble() will return
 * different values.
 */
TEST(RandomTest, NextCanonicalDifferentSeeds) {
    PseudoRandom a(12);
    PseudoRandom b(11);
    ASSERT_NOT_EQUALS(a.nextCanonicalDouble(), b.nextCanonicalDouble());
}

/**
 * Test that nextCanonicalDouble() avoids returning a value soon after it has previously returned
 * that value.
 */
TEST(RandomTest, NextCanonicalDistinctValues) {
    PseudoRandom a(11);
    std::set<double> s;
    for (int i = 0; i < 100; i++) {
        s.insert(a.nextCanonicalDouble());
    }
    ASSERT_EQUALS(100U, s.size());
}

/**
 * Test that nextCanonicalDouble() is at least very likely to return values in [0,1).
 */
TEST(RandomTest, NextCanonicalWithinRange) {
    PseudoRandom prng(10);
    for (size_t i = 0; i < 1'000'000; ++i) {
        double next = prng.nextCanonicalDouble();
        ASSERT_GTE(next, 0.0);
        ASSERT_LT(next, 1.0);
    }
}

TEST(RandomTest, NextInt32SanityCheck) {
    // Generate 1000 int32s and assert that each bit is set between 40% and 60% of the time. This is
    // a bare minimum sanity check, not an attempt to ensure quality random numbers.

    PseudoRandom a(11);
    std::vector<int32_t> nums;
    for (int i = 0; i < 1000; i++) {
        nums.push_back(a.nextInt32());
    }

    for (int bit = 0; bit < 32; bit++) {
        int onesCount = 0;
        for (auto&& num : nums) {
            bool isSet = (num >> bit) & 1;
            if (isSet)
                onesCount++;
        }

        if (onesCount < 400 || onesCount > 600)
            FAIL(str::stream() << "bit " << bit << " was set " << (onesCount / 10.)
                               << "% of the time.");
    }
}

TEST(RandomTest, NextInt64SanityCheck) {
    // Generate 1000 int64s and assert that each bit is set between 40% and 60% of the time. This is
    // a bare minimum sanity check, not an attempt to ensure quality random numbers.

    PseudoRandom a(11);
    std::vector<int64_t> nums;
    for (int i = 0; i < 1000; i++) {
        nums.push_back(a.nextInt64());
    }

    for (int bit = 0; bit < 64; bit++) {
        int onesCount = 0;
        for (auto&& num : nums) {
            bool isSet = (num >> bit) & 1;
            if (isSet)
                onesCount++;
        }

        if (onesCount < 400 || onesCount > 600)
            FAIL(str::stream() << "bit " << bit << " was set " << (onesCount / 10.)
                               << "% of the time.");
    }
}

TEST(RandomTest, NextInt32InRange) {
    PseudoRandom a(11);
    for (int i = 0; i < 1000; i++) {
        auto res = a.nextInt32(10);
        ASSERT_GTE(res, 0);
        ASSERT_LT(res, 10);
    }
}

TEST(RandomTest, NextInt64InRange) {
    PseudoRandom a(11);
    for (int i = 0; i < 1000; i++) {
        auto res = a.nextInt64(10);
        ASSERT_GTE(res, 0);
        ASSERT_LT(res, 10);
    }
}

/**
 * Test uniformity of nextInt32(max)
 */
TEST(RandomTest, NextInt32Uniformity) {
    PseudoRandom prng(10);
    /* Break the range into sections. */
    /* Check that all sections get roughly equal # of hits */
    constexpr int32_t kMax = (int32_t{3} << 29) - 1;
    constexpr size_t kBuckets = 64;
    constexpr size_t kNIter = 1'000'000;
    constexpr double mu = static_cast<double>(kNIter) / kBuckets;
    constexpr double muSqInv = 1. / (mu * mu);
    std::vector<size_t> hist(kBuckets);
    for (size_t i = 0; i < kNIter; ++i) {
        auto next = prng.nextInt32(kMax);
        ASSERT_GTE(next, 0);
        ASSERT_LTE(next, kMax);
        ++hist[static_cast<double>(next) * static_cast<double>(kBuckets) / (kMax + 1)];
    }
    if (kDebugBuild) {
        for (size_t i = 0; i < hist.size(); ++i) {
            double dev = std::pow(std::pow((hist[i] - mu) / mu, 2), .5);
            LOGV2(22611,
                  "{format_FMT_STRING_4_count_4_dev_6f_i_hist_i_dev_std_string_hist_i_256}",
                  "format_FMT_STRING_4_count_4_dev_6f_i_hist_i_dev_std_string_hist_i_256"_attr =
                      format(FMT_STRING("  [{:4}] count:{:4}, dev:{:6f}, {}"),
                             i,
                             hist[i],
                             dev,
                             std::string(hist[i] / 256, '*')));
        }
    }
    for (size_t i = 0; i < hist.size(); ++i) {
        double dev = std::pow(std::pow(hist[i] - mu, 2) * muSqInv, .5);
        ASSERT_LT(dev, 0.1) << format(FMT_STRING("hist[{}]={}, mu={}"), i, hist[i], mu);
    }
}

TEST(RandomTest, Secure1) {
    auto a = SecureRandom();
    auto b = SecureRandom();

    for (int i = 0; i < 100; i++) {
        ASSERT_NOT_EQUALS(a.nextInt64(), b.nextInt64());
    }
}
}  // namespace mongo
