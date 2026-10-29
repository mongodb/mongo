// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
