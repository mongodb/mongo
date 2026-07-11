// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/string_util.h"

#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo::query_string_util {

void assertLevenshteinDistanceBetweenStrings(const std::string s1,
                                             const std::string s2,
                                             unsigned int expectedDistance) {
    ASSERT_EQ(levenshteinDistance(s1, s2), expectedDistance);
}

TEST(LevenshteinDistanceTest, EmptyStrings) {
    assertLevenshteinDistanceBetweenStrings("", "", 0);
}

TEST(LevenshteinDistanceTest, EqualString) {
    assertLevenshteinDistanceBetweenStrings("foo", "foo", 0);
}

TEST(LevenshteinDistanceTest, OneStringEmpty) {
    assertLevenshteinDistanceBetweenStrings("foo", "", 3);
    assertLevenshteinDistanceBetweenStrings("", "bar", 3);
}


TEST(LevenshteinDistanceTest, UnequalNonEmptyStrings) {
    assertLevenshteinDistanceBetweenStrings("cat", "cut", 1);
    assertLevenshteinDistanceBetweenStrings("hat", "bat", 1);
    assertLevenshteinDistanceBetweenStrings("hat", "ha", 1);
    assertLevenshteinDistanceBetweenStrings("at", "hat", 1);
    assertLevenshteinDistanceBetweenStrings("h", "ros", 3);
    assertLevenshteinDistanceBetweenStrings("horse", "ros", 3);
    assertLevenshteinDistanceBetweenStrings("kitten", "sitting", 3);
    assertLevenshteinDistanceBetweenStrings("intention", "execution", 5);
    assertLevenshteinDistanceBetweenStrings("supercalifragilisticexpialidocious", "fragile", 27);
}

TEST(ComputeTypoSuggestionsTest, SingleValidString) {
    auto result = computeTypoSuggestions({"apple"}, {"app"});
    ASSERT_EQ(result.size(), 1U);
    ASSERT_EQ(result[0].first, "app");
    ASSERT_EQ(result[0].second, std::vector<std::string>({"apple"}));
}

TEST(ComputeTypoSuggestionsTest, MultipleValidStrings) {
    auto result = computeTypoSuggestions({"apple", "bat", "bag"}, {"app", "bam"});
    ASSERT_EQ(result.size(), 2U);
    ASSERT_EQ(result[0].first, "app");
    ASSERT_EQ(result[0].second, std::vector<std::string>({"apple"}));
    ASSERT_EQ(result[1].first, "bam");
    // "bat" and "bag" are equidistant from "bam"
    ASSERT_EQ(result[1].second, (std::vector<std::string>{"bat", "bag"}));
}

TEST(ComputeTypoSuggestionsWithMaxCellsTest, BelowBudgetUsesLevenshtein) {
    // Small strings, large budget: should rank by distance normally.
    auto result = computeTypoSuggestions({"apple", "bat", "bag"}, {"bam"}, 1000);
    ASSERT_EQ(result.size(), 1U);
    ASSERT_EQ(result[0].first, "bam");
    ASSERT_EQ(result[0].second, (std::vector<std::string>{"bat", "bag"}));
}

TEST(ComputeTypoSuggestionsWithMaxCellsTest, ExceedsBudgetFallsBackToAllValidStrings) {
    // maxCells = 1 forces the budget to be exceeded by any non-empty input pair.
    std::vector<std::string> valid = {"pipelineA", "pipelineB"};
    std::vector<std::string> typos = {"pipelineC"};
    auto result = computeTypoSuggestions(valid, typos, /*maxCells*/ 1);
    ASSERT_EQ(result.size(), 1U);
    ASSERT_EQ(result[0].first, "pipelineC");
    // All valid strings returned unranked.
    ASSERT_EQ(result[0].second, valid);
}

TEST(ComputeTypoSuggestionsWithMaxCellsTest, ExceedsBudgetMultipleTypos) {
    std::vector<std::string> valid = {"foo", "bar"};
    std::vector<std::string> typos = {"fo", "ba"};
    auto result = computeTypoSuggestions(valid, typos, /*maxCells*/ 1);
    ASSERT_EQ(result.size(), 2U);
    ASSERT_EQ(result[0].second, valid);
    ASSERT_EQ(result[1].second, valid);
}

TEST(SafeComputeTypoSuggestionsTest, NormalInputsRankedByDistance) {
    auto result = safeComputeTypoSuggestions({"apple", "bat", "bag"}, {"bam"});
    ASSERT_EQ(result.size(), 1U);
    ASSERT_EQ(result[0].second, (std::vector<std::string>{"bat", "bag"}));
}

TEST(SafeComputeTypoSuggestionsTest, VeryLongStringsFallBackToUnranked) {
    // Strings of 60K characters would allocate ~14 GB; safe variant must not OOM.
    std::string longA(60000, 'A');
    std::string longC(60000, 'C');
    std::string longB(60000, 'B');
    auto result = safeComputeTypoSuggestions({longA, longC}, {longB});
    ASSERT_EQ(result.size(), 1U);
    ASSERT_EQ(result[0].first, longB);
    // All valid strings returned unranked rather than computing distance.
    ASSERT_EQ(result[0].second, (std::vector<std::string>{longA, longC}));
}

}  // namespace mongo::query_string_util
