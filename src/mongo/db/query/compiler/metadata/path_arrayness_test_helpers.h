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

#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Enum used to refer to different possible widths of trie:
 *      kNarrow: less variation in fieldpaths
 *      kMediumWidth : moderate variation in fieldpaths
 *      kWide: more variation in fieldpaths
 */
enum TrieWidth { kNarrow, kMediumWidth, kWide };

/**
 * Enum used to refer to different possible depths of trie:
 *      kShallow: average length of fieldpaths skewed lower
 *      kMediumDepth : uniform distribution of fieldpath lengths
 *      kDeep: average length of fieldpaths skewed higher
 */
enum TrieDepth { kShallow, kMediumDepth, kDeep };

/**
 * Test helper generating a random vector of paths along with the corresponding multikeyness
 * information. The paths are generating according to the provided config. numberOfPaths dictates
 * the size of the vector, maxLength and ndvLengths dictate the depth of the dotted paths, and
 * rangeFieldNameLength determines the length of the individual components of the dotted paths. The
 * random generator uses the provided seeds and a distribution chosen based on the desired shape of
 * the trie, given by the trieDepth parameter.
 */
std::vector<std::pair<std::string, MultikeyComponents>> generateRandomFieldPathsWithArraynessInfo(
    int numberOfPaths,
    int maxLength,
    int ndvLengths,
    size_t seed,
    size_t seed2,
    std::pair<int, int> rangeFieldNameLength = std::pair(1, 4),
    TrieDepth trieDepth = TrieDepth::kMediumDepth);

/**
 * A simple helper combining the two vectors into a vector of pairs. This helper is used to simplify
 * declaring user defined tests, while using the same helpers that automatic generators use.
 */
std::vector<std::pair<std::string, MultikeyComponents>> combineVectors(
    const std::vector<FieldPath>& fieldPaths,
    const std::vector<MultikeyComponents>& multikeyComponents);

/**
 * Test helper transfofming a vector of pairs of fieldpaths and multikeyness info to a map where the
 * key is the fieldpath and the value is whether the final component is an array.
 */
stdx::unordered_map<std::string, bool> tranformVectorToMap(
    const std::vector<std::pair<std::string, MultikeyComponents>>& vectorOfFieldPaths);

/**
 * Given a dotted path and a maximum allowable length (number of levels of nesting), returns the
 * path truncated to that length.
 */
std::string truncatePathToLength(std::string path, size_t maxLength);

}  // namespace mongo
