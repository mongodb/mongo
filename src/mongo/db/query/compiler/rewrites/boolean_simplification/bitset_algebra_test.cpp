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

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_algebra.h"

#include "mongo/unittest/unittest.h"

namespace mongo::boolean_simplification {
constexpr size_t nbits = 64;

TEST(BitsetTermOperationsTest, CanAbsorb) {
    ASSERT_TRUE(BitsetTerm("001", "001").canAbsorb({"011", "011"}));
    ASSERT_TRUE(BitsetTerm("001", "001").canAbsorb({"001", "001"}));
    ASSERT_TRUE(BitsetTerm("001", "001").canAbsorb({"001", "101"}));
    ASSERT_FALSE(BitsetTerm("001", "001").canAbsorb({"000", "001"}));
    ASSERT_FALSE(BitsetTerm("000", "001").canAbsorb({"001", "001"}));
}

TEST(MaxtermOperationsTest, ABOrC) {
    Maxterm ab{{"011", "011"}};
    Maxterm c{{"100", "100"}};
    Maxterm expectedResult{
        {"011", "011"},
        {"100", "100"},
    };

    ab |= c;
    ASSERT_EQ(ab, expectedResult);
}

TEST(MaxtermOperationsTest, ABOrA) {
    Maxterm ab{{"11", "11"}};
    Maxterm a{{"01", "01"}};
    Maxterm expectedResult{
        {"11", "11"},
        {"01", "01"},
    };

    ab |= a;
    ASSERT_EQ(ab, expectedResult);
}

// (AB | A ) |= (~AC | BD)
TEST(MaxtermOperationsTest, ComplexOr) {
    Maxterm abOrA{
        {"0011", "0011"},
        {"0001", "0001"},
    };
    Maxterm nacOrBd{
        {"0100", "0101"},
        {"1010", "1010"},
    };
    Maxterm expectedResult{
        {"0011", "0011"},  // A & B
        {"0001", "0001"},  // A
        {"0100", "0101"},  // ~A & C
        {"1010", "1010"},  // B & D
    };

    abOrA |= nacOrBd;
    ASSERT_EQ(abOrA, expectedResult);
}

// (A | B) & C
TEST(MaxtermOperationsTest, ComplexAnd) {
    Maxterm aOrB{
        {"001", "001"},
        {"010", "010"},
    };

    Maxterm c{
        {"100", "100"},
    };

    Maxterm expectedResult{
        {"101", "101"},
        {"110", "110"},
    };

    auto result = aOrB & c;
    ASSERT_EQ(expectedResult, result);
}

// "(A | B) &= C"
TEST(MaxtermOperationsTest, ComplexUsingAndAssignmentOperator) {
    Maxterm aOrB{
        {"001", "001"},
        {"010", "010"},
    };

    Maxterm c{
        {"100", "100"},
    };

    Maxterm expectedResult{
        {"101", "101"},
        {"110", "110"},
    };

    aOrB &= c;
    ASSERT_EQ(expectedResult, aOrB);
}

// (A | B) & (C | ~D)
TEST(MaxtermOperationsTest, ComplexAnd2) {
    Maxterm aOrB{
        {"0001", "0001"},
        {"0010", "0010"},
    };

    Maxterm cOrNd{
        {"0100", "0100"},
        {"0000", "1000"},
    };

    Maxterm expectedResult{
        {"0101", "0101"},  // A & C
        {"0001", "1001"},  // A & ~D
        {"0110", "0110"},  // B & C
        {"0010", "1010"},  // B & ~D
    };

    auto result = aOrB & cOrNd;
    ASSERT_EQ(expectedResult, result);
}

TEST(ExtractCommonPredicatesTest, PositiveAndNegativeCommonPredicates) {
    Maxterm maxterm{
        {"11000", "11010"},
        {"01000", "11010"},
        {"01001", "01011"},
        {"11000", "11011"},
        {"01000", "01111"},
    };

    Minterm expectedCommonPredicate{"01000", "01010"};

    Maxterm expectedMaxterm = {
        {"10000", "10000"},
        {"00000", "10000"},
        {"00001", "00001"},
        {"10000", "10001"},
        {"00000", "00101"},
    };

    auto [commonPredicates, outputMaxterm] = extractCommonPredicates(maxterm);

    ASSERT_EQ(expectedCommonPredicate, commonPredicates);
    ASSERT_EQ(expectedMaxterm, outputMaxterm);
};

TEST(ExtractCommonPredicatesTest, PositiveOnlyCommonPredicates) {
    Maxterm maxterm{
        {"00111", "10111"},
        {"10111", "10111"},
        {"10101", "11101"},
        {"01101", "11101"},
        {"00101", "11101"},
    };

    Minterm expectedCommonPredicate{"00101", "00101"};

    Maxterm expectedMaxterm{
        {"00010", "10010"},
        {"10010", "10010"},
        {"10000", "11000"},
        {"01000", "11000"},
        {"00000", "11000"},
    };

    auto [commonPredicates, outputMaxterm] = extractCommonPredicates(maxterm);

    ASSERT_EQ(expectedCommonPredicate, commonPredicates);
    ASSERT_EQ(expectedMaxterm, outputMaxterm);
}

TEST(ExtractCommonPredicatesTest, NegativeOnlyCommonPredicates) {
    Maxterm maxterm{
        {"00010", "10111"},
        {"00110", "10111"},
        {"00100", "11101"},
        {"01000", "11101"},
        {"00000", "11101"},
    };

    Minterm expectedCommonPredicate{"00000", "10001"};

    Maxterm expectedMaxterm{
        {"00010", "00110"},
        {"00110", "00110"},
        {"00100", "01100"},
        {"01000", "01100"},
        {"00000", "01100"},
    };

    auto [commonPredicates, outputMaxterm] = extractCommonPredicates(maxterm);

    ASSERT_EQ(expectedCommonPredicate, commonPredicates);
    ASSERT_EQ(expectedMaxterm, outputMaxterm);
}

TEST(ExtractCommonPredicatesTest, NoCommonPredicates) {
    Maxterm maxterm{
        {"00001", "00011"},
        {"00010", "00110"},
        {"00100", "01100"},
        {"01000", "11000"},
        {"10000", "11000"},
    };

    auto [commonPredicates, outputMaxterm] = extractCommonPredicates(maxterm);

    ASSERT_TRUE(commonPredicates.isConjunctionAlwaysTrue());
    ASSERT_EQ(maxterm, outputMaxterm);
}

TEST(ExtractCommonPredicatesTest, AlwaysFalseInput) {
    Maxterm maxterm{0};

    ASSERT_TRUE(maxterm.isAlwaysFalse());

    auto [commonPredicates, outputMaxterm] = extractCommonPredicates(maxterm);
    ASSERT_TRUE(commonPredicates.isConjunctionAlwaysTrue());
    ASSERT_TRUE(outputMaxterm.isAlwaysFalse());
}

TEST(ExtractCommonPredicatesTest, AlwaysTrueInput) {
    Maxterm maxterm{0};
    maxterm.appendEmpty();

    ASSERT_TRUE(maxterm.isAlwaysTrue());

    auto [commonPredicates, outputMaxterm] = extractCommonPredicates(maxterm);
    ASSERT_TRUE(commonPredicates.isConjunctionAlwaysTrue());
    ASSERT_TRUE(outputMaxterm.isAlwaysTrue());
}

TEST(ExtractCommonPredicatesTest, CommonPredicatesOnly) {
    Maxterm maxterm{
        {"00100", "00110"},
        {"00100", "00110"},
    };

    Minterm expectedCommonPredicates{"00100", "00110"};

    auto [commonPredicates, outputMaxterm] = extractCommonPredicates(maxterm);
    ASSERT_EQ(expectedCommonPredicates, commonPredicates);
    ASSERT_TRUE(outputMaxterm.isAlwaysTrue());
}
}  // namespace mongo::boolean_simplification
