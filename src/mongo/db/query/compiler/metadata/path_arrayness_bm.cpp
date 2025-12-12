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

#include "mongo/db/query/compiler/metadata/path_arrayness.h"

#include "mongo/db/query/compiler/metadata/path_arrayness_test_helpers.h"

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

/**
 * Preset values determining the number of paths per group based on the desired trie width.
 */
std::map<TrieWidth, int> trieWidthPresets = {
    {TrieWidth::kNarrow, 15}, {TrieWidth::kMediumWidth, 10}, {TrieWidth::kWide, 5}};

/**
 * Helper used to parse test parameters and generate fieldpaths using that configuration.
 */
std::vector<std::pair<std::string, MultikeyComponents>> generatePathsToInsert(
    benchmark::State& state, size_t seed, size_t seed2) {
    // Number of paths to insert.
    int numberOfPaths = static_cast<int>(state.range(0));

    // Width of trie generated field paths should create.
    TrieWidth trieWidth = static_cast<TrieWidth>(state.range(3));

    // Width of the generated trie. Paths generated with the same length will be identical, so the
    // number of distinct lengths controls the variety of the paths, and thus the width of the trie.
    // We increase the size of each identical group to decrease the width of the trie and vice
    // versa.
    int numPathsPerGroup = trieWidthPresets[trieWidth];

    // Depth of the generated trie. This is controlled by skewing the average path length higher or
    // lower to generate a deeper or shallower trie respectively.
    TrieDepth trieDepth = static_cast<TrieDepth>(state.range(4));

    // Number of distinct lengths of paths.
    int ndvLengths = numberOfPaths / numPathsPerGroup;

    // Maximum length of dotted field paths.
    size_t maxLength = static_cast<size_t>(state.range(1));

    // Maximum length of each component of a dotted field path
    // The size of the range of possible lengths we choose from is 10 by default, and the bottom
    // bound must always be at least 1.
    int maxFieldNameLength = static_cast<int>(2);
    std::pair<int, int> rangeFieldNameLength(std::max(maxFieldNameLength - 10, 1),
                                             maxFieldNameLength);

    // Generate the fieldpath, multikeycomponents info pairs that will be inserted into the path
    // arrayness data structure.
    std::vector<std::pair<std::string, MultikeyComponents>> pathsToInsert =
        generateRandomFieldPathsWithArraynessInfo(
            numberOfPaths, maxLength, ndvLengths, seed, seed2, rangeFieldNameLength, trieDepth);

    return pathsToInsert;
}

void BM_PathArraynessBuild(benchmark::State& state) {
    size_t seed = 1354754;
    size_t seed2 = 3421354754;

    auto pathsToInsert = generatePathsToInsert(state, seed, seed2);

    for (auto _ : state) {
        PathArrayness pathArrayness;
        for (size_t i = 0; i < pathsToInsert.size(); i++) {
            pathArrayness.addPath(pathsToInsert[i].first, pathsToInsert[i].second);
        }
    }
}

void BM_PathArraynessLookup(benchmark::State& state) {
    size_t seed = 1354754;
    size_t seed2 = 3421354754;

    auto pathsToInsert = generatePathsToInsert(state, seed, seed2);

    // Maximum length of dotted field paths.
    size_t maxLength = static_cast<size_t>(state.range(1));

    // Build the path arrayness data structure.
    PathArrayness pathArrayness;
    for (size_t i = 0; i < pathsToInsert.size(); i++) {
        pathArrayness.addPath(pathsToInsert[i].first, pathsToInsert[i].second);
    }

    // Number of paths to query.
    size_t numberOfPathsQuery = static_cast<size_t>(state.range(5));

    // Number of distinct lengths of paths to query.
    // By default we chose that we have 5 field paths for each length.
    size_t maxLengthQuery = static_cast<size_t>(state.range(6));

    // We extract a uniformly distributed selection of the fieldpaths used to build the
    // PathArrayness trie to be used as the fieldpaths to query, truncating any that exceed
    // maxLengthQuery. This ensures that we query only paths that exist in the tree while allowing
    // control over the maximum depth we search to.
    std::vector<std::string> pathsToQuery;
    pathsToQuery.reserve(pathsToInsert.size());

    int increment = std::max(pathsToInsert.size() / numberOfPathsQuery, static_cast<size_t>(1));

    std::string truncatedPath;
    for (size_t i = 0; i < pathsToInsert.size(); i += increment) {
        if (maxLengthQuery < maxLength) {
            truncatedPath = truncatePathToLength(pathsToInsert[i].first, maxLengthQuery);
        } else {
            truncatedPath = pathsToInsert[i].first;
        }
        pathsToQuery.push_back(truncatedPath);
    }

    for (auto _ : state) {
        for (size_t i = 0; i < numberOfPathsQuery; i++) {
            // numberOfPathsQuery could be larger than the number of paths we have, so we take the
            // modulo of the index in order to wrap back around to the start of the array if that's
            // the case.
            pathArrayness.isPathArray(pathsToQuery[i % pathsToQuery.size()]);
        }
    }
}

BENCHMARK(BM_PathArraynessBuild)
    ->ArgNames({
        "numberOfPaths",
        "maxLength",
        "maxFieldNameLength",
        "trieWidth",
        "trieDepth",
    })
    ->ArgsProduct({
        /*numberOfPaths*/
        {64, 512, 1024, 2048},
        /*maxLength*/
        {10, 50, 100},
        /*maxFieldNameLength: */
        {5, 125, 250},
        /*trieWidth*/
        {TrieWidth::kNarrow, TrieWidth::kMediumWidth, TrieWidth::kWide},
        /*trieDepth*/
        {TrieDepth::kShallow, TrieDepth::kMediumDepth, TrieDepth::kDeep},
    })
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);  // Restrict number of iterations to avoid time out.

BENCHMARK(BM_PathArraynessLookup)
    ->ArgNames({
        "numberOfPaths",
        "maxLength",
        "maxFieldNameLength",
        "trieWidth",
        "trieDepth",
        "numberOfPathsQuery",
        "maxLengthQuery",
    })
    ->ArgsProduct({
        /*numberOfPaths*/
        {64, 512, 1024, 2048},
        /*maxLength*/
        {10, 50, 100},
        /*maxFieldNameLength: */
        {5, 125, 250},
        /*trieWidth*/
        {TrieWidth::kNarrow, TrieWidth::kMediumWidth, TrieWidth::kWide},
        /*trieDepth*/
        {TrieDepth::kShallow, TrieDepth::kMediumDepth, TrieDepth::kDeep},
        /*numberOfPathsQuery*/
        {50, 100, 200},
        /*maxLengthQuery*/
        {10, 50, 100},
    })
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);  // Restrict number of iterations to avoid time out.
}  // namespace mongo
