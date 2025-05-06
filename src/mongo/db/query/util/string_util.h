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
#pragma once

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
 */
std::vector<std::pair<std::string, std::vector<std::string>>> computeTypoSuggestions(
    const std::vector<std::string>& validStrings, const std::vector<std::string>& typos);

}  // namespace mongo::query_string_util
