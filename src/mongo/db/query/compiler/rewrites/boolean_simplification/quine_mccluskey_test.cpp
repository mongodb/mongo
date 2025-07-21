/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/rewrites/boolean_simplification/quine_mccluskey.h"

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"

namespace mongo::boolean_simplification {
TEST(FindPrimeImplicantsTest, Test1) {
    // AB | ~AB = B
    Bitset mask{"11"_b};
    Maxterm maxterm{{
        Minterm{"10"_b, mask},
        Minterm{"11"_b, mask},
    }};
    Maxterm expectedMaxterm{Minterm{"10", "10"}};
    std::vector<CoveredOriginalMinterms> expectedCoveredMinterms{{0, 1}};

    auto [actualMaxterm, actualCoveredMinterms] = findPrimeImplicants(maxterm);
    ASSERT_EQ(expectedMaxterm, actualMaxterm);
    ASSERT_EQ(expectedCoveredMinterms, actualCoveredMinterms);
}

TEST(FindPrimeImplicantsTest, Test2) {
    // "ABC | A~BC = AC"
    Bitset mask{"111"_b};
    Maxterm maxterm{
        Minterm{"111"_b, mask},
        Minterm{"101"_b, mask},
    };
    Maxterm expectedMaxterm{Minterm{"101", "101"}};
    std::vector<CoveredOriginalMinterms> expectedCoveredMinterms{{0, 1}};

    auto [actualMaxterm, actualCoveredMinterms] = findPrimeImplicants(maxterm);
    ASSERT_EQ(expectedMaxterm, actualMaxterm);
    ASSERT_EQ(expectedCoveredMinterms, actualCoveredMinterms);
}

TEST(FindPrimeImplicantsTest, Test3) {
    // ABC | A~BC | AB~C = AC | AB
    Bitset mask{"111"_b};
    Maxterm maxterm{
        Minterm{"111"_b, mask},
        Minterm{"101"_b, mask},
        Minterm{"110"_b, mask},
    };

    Maxterm expectedMaxterm{
        Minterm{"101", "101"},
        Minterm{"110", "110"},
    };

    std::vector<CoveredOriginalMinterms> expectedCoveredMinterms{
        {0, 1},
        {0, 2},
    };

    auto [actualMaxterm, actualCoveredMinterms] = findPrimeImplicants(maxterm);
    ASSERT_EQ(expectedMaxterm, actualMaxterm);
    ASSERT_EQ(expectedCoveredMinterms, actualCoveredMinterms);
}

TEST(FindPrimeImplicantsTest, Test4) {
    // ~A~B~C~D | ~A~B~CD | ~AB~C~D | ~AB~CD = ~A~C
    Bitset mask{"1111"_b};
    Maxterm maxterm{
        Minterm{"0000"_b, mask},
        Minterm{"0001"_b, mask},
        Minterm{"0100"_b, mask},
        Minterm{"0101"_b, mask},
    };

    Maxterm expectedMaxterm{
        Minterm{"0000", "1010"},
    };

    std::vector<CoveredOriginalMinterms> expectedCoveredMinterms{
        {0, 1, 2, 3},
    };

    auto [actualMaxterm, actualCoveredMinterms] = findPrimeImplicants(maxterm);
    ASSERT_EQ(expectedMaxterm, actualMaxterm);
    ASSERT_EQ(expectedCoveredMinterms, actualCoveredMinterms);
}

TEST(FindPrimeImplicantsTest, Test5) {
    // ~A~B~C~D | ~A~B~CD | ~AB~C~D | ~AB~CD |~ABCD | A~BCD = A~BCD | ~ABD | ~A~C
    Bitset mask{"1111"_b};
    Maxterm maxterm{{"0000"_b, mask},
                    Minterm{"0001"_b, mask},
                    Minterm{"0100"_b, mask},
                    Minterm{"0101"_b, mask},
                    Minterm{"0111"_b, mask},
                    Minterm{"1011"_b, mask}};

    Maxterm expectedMaxterm{
        Minterm{"1011", "1111"},
        Minterm{"0101", "1101"},
        Minterm{"0000", "1010"},
    };

    std::vector<CoveredOriginalMinterms> expectedCoveredMinterms{
        {5},
        {3, 4},
        {0, 1, 2, 3},
    };

    auto [actualMaxterm, actualCoveredMinterms] = findPrimeImplicants(maxterm);
    ASSERT_EQ(expectedMaxterm, actualMaxterm);
    ASSERT_EQ(expectedCoveredMinterms, actualCoveredMinterms);
}

TEST(FindPrimeImplicantsTest, Test6) {
    // ~A~B~C | ~AB~C | A~B~C | ~ABC | A~BC | ABC = ~A~C | ~B~C | ~AB | A~B | BC | AC
    Bitset mask{"111"_b};
    Maxterm maxterm{
        Minterm{"000"_b, mask},
        Minterm{"010"_b, mask},
        Minterm{"100"_b, mask},
        Minterm{"011"_b, mask},
        Minterm{"101"_b, mask},
        Minterm{"111"_b, mask},
    };

    Maxterm expectedMaxterm{
        Minterm{"000", "101"},
        Minterm{"000", "011"},
        Minterm{"010", "110"},
        Minterm{"100", "110"},
        Minterm{"011", "011"},
        Minterm{"101", "101"},
    };

    std::vector<CoveredOriginalMinterms> expectedCoveredMinterms{
        {0, 1},
        {0, 2},
        {1, 3},
        {2, 4},
        {3, 5},
        {4, 5},
    };

    auto [actualMaxterm, actualCoveredMinterms] = findPrimeImplicants(maxterm);
    ASSERT_EQ(expectedMaxterm, actualMaxterm);
    ASSERT_EQ(expectedCoveredMinterms, actualCoveredMinterms);
}

TEST(QuineMcCluskeyTest, Test5) {
    // ~A~B~C~D | ~A~B~CD | ~AB~C~D | ~AB~CD |~ABCD | A~BCD = A~BCD | ~ABD | ~A~C
    Bitset mask{"1111"_b};
    Maxterm maxterm{{"0000"_b, mask},
                    Minterm{"0001"_b, mask},
                    Minterm{"0100"_b, mask},
                    Minterm{"0101"_b, mask},
                    Minterm{"0111"_b, mask},
                    Minterm{"1011"_b, mask}};

    Maxterm expectedMaxterm{
        Minterm{"1011", "1111"},
        Minterm{"0101", "1101"},
        Minterm{"0000", "1010"},
    };

    auto actualMaxterm = quineMcCluskey(maxterm);
    ASSERT_EQ(expectedMaxterm, actualMaxterm);
}

/**
 * This test simplifies the same expression as FindPrimeImplicantsTest::Test6 but because it employs
 * Petricks's method for further optimization the resulting expression is much smaller.
 */
TEST(QuineMcCluskeyTest, Test6) {
    // ~A~B~C | ~AB~C | A~B~C | ~ABC | A~BC | ABC = ~A~C | A~B | BC
    Bitset mask{"111"_b};
    Maxterm maxterm{
        Minterm{"000"_b, mask},
        Minterm{"010"_b, mask},
        Minterm{"100"_b, mask},
        Minterm{"011"_b, mask},
        Minterm{"101"_b, mask},
        Minterm{"111"_b, mask},
    };

    Maxterm expectedMaxterm{
        Minterm{"000", "101"},
        Minterm{"100", "110"},
        Minterm{"011", "011"},
    };

    auto actualMaxterm = quineMcCluskey(maxterm);
    // This test asserts on one possible output: ~A~C | A~B | BC, another possible output is ~A~C |
    // ~AB | AC. See the coverage output in FindPrimeImplicantsTest::Test6, the last uncovered
    // minterm #5 can be covered by BC or AC. It just happens that we select the first optimal
    // coverage, if we change quineMcCluskey the second one can be picked up.
    ASSERT_EQ(expectedMaxterm, actualMaxterm);
}
}  // namespace mongo::boolean_simplification
