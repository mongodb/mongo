// random_test.cpp


/*    Copyright 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include <set>
#include <vector>

#include "mongo/platform/random.h"

#include "mongo/unittest/unittest.h"

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
 * Test that nextCanonicalDouble() always returns values between 0 and 1.
 */
TEST(RandomTest, NextCanonicalWithinRange) {
    PseudoRandom prng(10);
    for (int i = 0; i < 100; i++) {
        double next = prng.nextCanonicalDouble();
        ASSERT_LTE(0.0, next);
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

TEST(RandomTest, Secure1) {
    SecureRandom* a = SecureRandom::create();
    SecureRandom* b = SecureRandom::create();

    for (int i = 0; i < 100; i++) {
        ASSERT_NOT_EQUALS(a->nextInt64(), b->nextInt64());
    }

    delete a;
    delete b;
}
}
