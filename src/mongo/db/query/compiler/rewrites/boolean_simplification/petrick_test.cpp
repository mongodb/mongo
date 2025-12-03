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

#include "mongo/db/query/compiler/rewrites/boolean_simplification/petrick.h"

#include "mongo/base/string_data.h"
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

    const auto result = petricksMethod(data);
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

    const auto result = petricksMethod(data);
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

    const auto result = petricksMethod(data);
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

    const auto result = petricksMethod(data);
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

    const auto result = petricksMethod(data);
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

    const auto result = petricksMethod(data);
    ASSERT_EQ(expectedResult, result);
}

TEST(PetrickTest, OneMinterm) {
    std::vector<CoveredOriginalMinterms> data{
        CoveredOriginalMinterms{"1"},
    };

    std::vector<PrimeImplicantIndices> expectedResult{
        {0},
    };

    const auto result = petricksMethod(data);
    ASSERT_EQ(expectedResult, result);
}

TEST(PetrickTest, NoMinterms) {
    std::vector<CoveredOriginalMinterms> data{};

    std::vector<PrimeImplicantIndices> expectedResult{};

    const auto result = petricksMethod(data);
    ASSERT_EQ(expectedResult, result);
}
}  // namespace mongo::boolean_simplification
