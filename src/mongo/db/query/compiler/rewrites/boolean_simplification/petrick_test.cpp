// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/rewrites/boolean_simplification/petrick.h"

#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo::boolean_simplification {
/**
 * The classic demonstration of how the Petric's method works. This example is available at the
 * Wikipedia page at the moment
 * (https://en.wikipedia.org/w/index.php?title=Petrick%27s_method&oldid=1142937196) and can be found
 * in a number of publications as well.
 */
TEST(PetrickTest, ClassicExample) {
    std::vector<CoveredOriginalMinterms> data{
        CoveredOriginalMinterms{"000011"},
        CoveredOriginalMinterms{"001001"},
        CoveredOriginalMinterms{"000110"},
        CoveredOriginalMinterms{"011000"},
        CoveredOriginalMinterms{"100100"},
        CoveredOriginalMinterms{"110000"},
    };

    std::vector<PrimeImplicantIndices> expectedResult{
        {0, 3, 4},
        {1, 2, 3, 4},
        {1, 2, 5},
        {0, 1, 4, 5},
        {0, 2, 3, 5},
    };

    const auto result = petricksMethod(data, 1000);
    ASSERT_EQ(expectedResult, result);
}

TEST(PetrickTest, OneCoverage) {
    std::vector<CoveredOriginalMinterms> data{
        CoveredOriginalMinterms{"001110"},
        CoveredOriginalMinterms{"011000"},
        CoveredOriginalMinterms{"110001"},
    };

    std::vector<PrimeImplicantIndices> expectedResult{
        {0, 2},
    };

    const auto result = petricksMethod(data, 1000);
    ASSERT_EQ(expectedResult, result);
}

TEST(PetrickTest, TwoCoverages) {
    std::vector<CoveredOriginalMinterms> data{
        CoveredOriginalMinterms{"0111"},
        CoveredOriginalMinterms{"1100"},
        CoveredOriginalMinterms{"1001"},
    };

    std::vector<PrimeImplicantIndices> expectedResult{
        {0, 1},
        {0, 2},
    };

    const auto result = petricksMethod(data, 1000);
    ASSERT_EQ(expectedResult, result);
}

TEST(PetrickTest, NoSimplifications) {
    std::vector<CoveredOriginalMinterms> data{
        CoveredOriginalMinterms{"0001"},
        CoveredOriginalMinterms{"0010"},
        CoveredOriginalMinterms{"0100"},
        CoveredOriginalMinterms{"1000"},
    };

    std::vector<PrimeImplicantIndices> expectedResult{
        {0, 1, 2, 3},
    };

    const auto result = petricksMethod(data, 1000);
    ASSERT_EQ(expectedResult, result);
}

TEST(PetrickTest, ManyEssentialsWithSimplifications) {
    std::vector<CoveredOriginalMinterms> data{
        CoveredOriginalMinterms{"0000111"},
        CoveredOriginalMinterms{"0001100"},
        CoveredOriginalMinterms{"0001001"},
        CoveredOriginalMinterms{"0010000"},
        CoveredOriginalMinterms{"0100000"},
        CoveredOriginalMinterms{"1000000"},
    };

    std::vector<PrimeImplicantIndices> expectedResult{
        {0, 1, 3, 4, 5},
        {0, 2, 3, 4, 5},
    };

    const auto result = petricksMethod(data, 1000);
    ASSERT_EQ(expectedResult, result);
}

TEST(PetrickTest, ReorderingMultipleEssentialsWithSimplifications) {
    std::vector<CoveredOriginalMinterms> data{CoveredOriginalMinterms{"0000001"},
                                              CoveredOriginalMinterms{"0001110"},
                                              CoveredOriginalMinterms{"1001000"},
                                              CoveredOriginalMinterms{"0111000"},
                                              CoveredOriginalMinterms{"0011010"},
                                              CoveredOriginalMinterms{"1010000"},
                                              CoveredOriginalMinterms{"0011000"}};

    std::vector<PrimeImplicantIndices> expectedResult{
        {0, 1, 2, 3},
        {0, 1, 3, 5},
    };

    const auto result = petricksMethod(data, 1000);
    ASSERT_EQ(expectedResult, result);
}

TEST(PetrickTest, OneMinterm) {
    std::vector<CoveredOriginalMinterms> data{
        CoveredOriginalMinterms{"1"},
    };

    std::vector<PrimeImplicantIndices> expectedResult{
        {0},
    };

    const auto result = petricksMethod(data, 1000);
    ASSERT_EQ(expectedResult, result);
}

TEST(PetrickTest, NoMinterms) {
    std::vector<CoveredOriginalMinterms> data{};

    std::vector<PrimeImplicantIndices> expectedResult{};

    const auto result = petricksMethod(data, 1000);
    ASSERT_EQ(expectedResult, result);
}
}  // namespace mongo::boolean_simplification
