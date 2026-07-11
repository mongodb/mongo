// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo::query_string_util {

/**
 * Computes the "Levenshtein Distance" between two strings, that is, the minimum number of edits
 * needed to transform one into the other.
 * So, two strings that are equal would have a distance of 0, and generally two strings are more
 * similar the shorter the distance is between them.
 * This function is used to rank/compare similarities between one string, and a set of others.
 */
unsigned int levenshteinDistance(const std::string& s1, const std::string& s2);

/**
 * Given a set of strings that are typos (don't match any strings in validStrings), compute a list
 * of suggestions from the set of valid strings for each typo string. The return vector has one
 * entry per typo, with the first entry in the pair being typo and the second being a list of
 * suggestions.
 *
 * For example: say the user passes in {'apple', 'bat', 'bag'} for the validStrings
 * and {'app', 'bam'} for the set of identified typos. This function, using the underlying
 * "Levenshtein Distance" algorithm to compare each typo with each valid string to find the closest
 * suggestions, will output {{"app": {"apple"}}, {"bam": {"bat", "bag"}}}.
 *
 * This overload performs no bounds checking on the total memory allocated for the Levenshtein
 * matrices. Callers are responsible for ensuring inputs are reasonably sized, or should use
 * computeTypoSuggestions(validStrings, typos, maxCells) or safeComputeTypoSuggestions() instead.
 */
std::vector<std::pair<std::string, std::vector<std::string>>> computeTypoSuggestions(
    const std::vector<std::string>& validStrings, const std::vector<std::string>& typos);

/**
 * Same as computeTypoSuggestions() above, but skips Levenshtein computation and falls back to
 * returning all valid strings as unranked suggestions if the total matrix cell count across all
 * (typo, validString) pairs would exceed maxCells. This bounds the aggregate allocation to
 * maxCells * sizeof(unsigned int) bytes.
 */
std::vector<std::pair<std::string, std::vector<std::string>>> computeTypoSuggestions(
    const std::vector<std::string>& validStrings,
    const std::vector<std::string>& typos,
    size_t maxCells);

// Default cell budget for safeComputeTypoSuggestions: 16M cells * 4 bytes = ~64 MB.
constexpr size_t kDefaultMaxLevenshteinTotalCells = 16 * 1024 * 1024;

/**
 * Convenience wrapper that calls computeTypoSuggestions with kDefaultMaxLevenshteinTotalCells.
 * Prefer this over the unbounded overload for any path reachable from user-supplied input.
 */
std::vector<std::pair<std::string, std::vector<std::string>>> safeComputeTypoSuggestions(
    const std::vector<std::string>& validStrings, const std::vector<std::string>& typos);

}  // namespace mongo::query_string_util
