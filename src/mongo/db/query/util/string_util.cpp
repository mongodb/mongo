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

#include "mongo/db/query/util/string_util.h"

#include <algorithm>
#include <vector>

namespace mongo {

namespace query_string_util {
unsigned int levenshteinDistance(const std::string& s1, const std::string& s2) {
    if (s1.empty()) {
        return s2.size();
    }
    if (s2.empty()) {
        return s1.size();
    }

    // editDistance[s1_i][s2_i] represents the minimum number of edits for
    // s1[0 : s1_i - 1] to transform into s2[0 : s2_i - 1], or vice versa. Said another way,
    // each entry in this matrix keeps track of the smallest number of edits needed for the
    // prefixes of each string up until that point to be transformed into each other.
    // Note: each string could be transformed into the other with the same number of edits,
    // with an addition of a character in one direction being equivalent to a removal of a character
    // in another (with character substitutions being flipped as well).
    std::vector<std::vector<unsigned int>> editDistance(
        s1.size() + 1, std::vector<unsigned int>(s2.size() + 1, 0));

    // Initialize the prefixes of each string against an empty string.
    // Each successive character is always an additional insertion.
    for (std::size_t s1_i = 1; s1_i < editDistance.size(); s1_i++) {
        editDistance[s1_i][0] = s1_i;
    }
    for (std::size_t s2_i = 1; s2_i < editDistance.front().size(); s2_i++) {
        editDistance[0][s2_i] = s2_i;
    }

    // After initialization of each string's prefixes being compared to the empty string,
    // we can fill in each entry in the matrix with dynamic programming.
    // editDistance[s1_i][s2_i] is always 1 edit distance (a substitution, addition or removal)
    // away from 3 other known prefix pairs on the matrix:
    //  > (s1_i - 1, s2_i - 1): which represents editing the character at (s1_i, s2_i) in place,
    //                          or keeping it as is, if the characters in s1 and s2 are equal.
    //  > (s1_i, s2_i - 1):     which represents removing s1[s1_i] when transforming s1 -> s2,
    //                          or adding s1[s1_i] when transforming s2 -> s1.
    //  > (s1_i - 1, s2_i):     which represents adding s2[s2_i] when transforming s1 -> s2,
    //                          or removing s2[s2_i] when transforming s2 -> s1.
    //
    // This means that for each entry editDistance[s1_i][s2_i] its possible to transform any of the
    // 3 above other prefix pairs into the one at (s1_i, s2_i) with a single edit;
    // or with no edits if s1[s1_i - 1] & s2[s2_i - 1] are equal.
    //
    // We can therefore calculate the proper value of editDistance[s1_i][s2_i]
    // (the minimum number of edits to transform s1[0 : s2_i - 1] into s2[0 : s2_i - 1],
    // or vice versa)by picking the prefix pair that has the least number of edits itself,
    // and then by adding 1 to represent the edit it would take to transform that prefix pair into
    // the one at (s1_i, s2_i).
    // Or if the current characters in the string are equal, we can pick number of edits
    // it took to make the prefixes of each string without the last character to be equal, which
    // is at (s1_i - 1, s2_i - 1), without adding in a cost for any additional edits.
    for (std::size_t s1_i = 1; s1_i < editDistance.size(); s1_i++) {
        for (std::size_t s2_i = 1; s2_i < editDistance[s1_i].size(); s2_i++) {
            if (s1[s1_i - 1] == s2[s2_i - 1]) {
                editDistance[s1_i][s2_i] = editDistance[s1_i - 1][s2_i - 1];
            } else {
                editDistance[s1_i][s2_i] = 1 +
                    std::min({editDistance[s1_i - 1][s2_i - 1],
                              editDistance[s1_i][s2_i - 1],
                              editDistance[s1_i - 1][s2_i]});
            }
        }
    }

    // The last entry in both dimensions of the matrix represents both full strings.
    return editDistance.back().back();
}

std::vector<std::pair<std::string, std::vector<std::string>>> computeTypoSuggestions(
    const std::vector<std::string>& validStrings, const std::vector<std::string>& typos) {
    std::vector<std::pair<std::string, std::vector<std::string>>> suggestions;
    // First, check a special, but also likely, case where there is only a single unmatched
    // entry. If so, this is the only possible suggestion, and there is no need to
    // waste time computing the levenshtein distance.
    if (validStrings.size() == 1) {
        suggestions.push_back({typos.front(), {validStrings.front()}});
    } else {
        for (const std::string& typo : typos) {
            // There are multiple unmatched entries, so find the best suggestion.
            // 'shortestDistance' is the levenshtein distance of the best suggestion found so far.
            // Initialize with the first unmatched entry, then compare to the rest.
            unsigned int shortestDistance = levenshteinDistance(typo, validStrings[0]);
            std::vector<std::string> bestSuggestions = {validStrings[0]};
            for (std::size_t i = 1; i < validStrings.size(); i++) {
                unsigned int ld = levenshteinDistance(typo, validStrings[i]);
                if (ld == shortestDistance) {
                    // Equally good suggestion found.
                    bestSuggestions.push_back(validStrings[i]);
                } else if (ld < shortestDistance) {
                    // Better suggestion found.
                    shortestDistance = ld;
                    bestSuggestions = {validStrings[i]};
                }
            }
            // Record best suggestion for this invalid weight.
            suggestions.push_back({typo, bestSuggestions});
        }
    }
    return suggestions;
}

}  // namespace query_string_util
}  // namespace mongo
