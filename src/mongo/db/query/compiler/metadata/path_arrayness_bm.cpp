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

void BM_PathArraynessBuild(benchmark::State& state) {
    size_t seed = 1354754;
    size_t seed2 = 3421354754;

    // Number of paths to insert.
    int numberOfPaths = static_cast<int>(state.range(0));

    // Number of distinct lengths of paths.
    // by default we chose that we have 5 field paths for each length.
    auto ndvLengths = numberOfPaths / 5;
    // Maximum length of dotted field paths.
    int maxLength = static_cast<int>(state.range(1));

    // Generate the fieldpath, multikeycomponents info pairs that will be inserted into the path
    // arrayness data structure.
    std::vector<std::pair<std::string, MultikeyComponents>> pathsToInsert =
        generateRandomFieldPathsWithArraynessInfo(
            numberOfPaths, maxLength, ndvLengths, seed, seed2);

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

    // Number of paths to insert.
    int numberOfPathsInTrie = static_cast<int>(state.range(0));

    // Number of distinct lengths of paths.
    // by default we chose that we have 5 field paths for each length.
    auto ndvLengthsInTrie = numberOfPathsInTrie / 5;
    // Maximum length of dotted field paths.
    int maxLengthInTrie = static_cast<int>(state.range(1));

    // Generate the fieldpath, multikeycomponents info pairs that will be inserted into the path
    // arrayness data structure.
    std::vector<std::pair<std::string, MultikeyComponents>> pathsToInsert =
        generateRandomFieldPathsWithArraynessInfo(
            numberOfPathsInTrie, maxLengthInTrie, ndvLengthsInTrie, seed, seed2);

    // Build the path arrayness data structure.
    PathArrayness pathArrayness;
    for (size_t i = 0; i < pathsToInsert.size(); i++) {
        pathArrayness.addPath(pathsToInsert[i].first, pathsToInsert[i].second);
    }

    // Number of paths to query.
    int numberOfPathsQuery = static_cast<int>(state.range(2));

    // Number of distinct lengths of paths to query.
    // by default we chose that we have 5 field paths for each length.
    auto ndvLengthsQuery = numberOfPathsQuery / 5;
    int maxLengthQuery = static_cast<int>(state.range(3));

    // Generate the fieldpath, multikeycomponents info pairs that will be used to query the
    // arrayness structure. Here we use only the fieldpath names and discard the multikeycomponents.
    std::vector<std::pair<std::string, MultikeyComponents>> pathsToQuery =
        generateRandomFieldPathsWithArraynessInfo(
            numberOfPathsQuery, maxLengthQuery, ndvLengthsQuery, seed, seed2);

    for (auto _ : state) {
        for (size_t i = 0; i < pathsToQuery.size(); i++) {
            pathArrayness.isPathArray(pathsToQuery[i].first);
        }
    }
}

BENCHMARK(BM_PathArraynessBuild)
    ->ArgNames({
        "numberOfPaths",
        "maxLength",
    })
    ->ArgsProduct({
        /*numberOfPaths*/ {
            64  //, 512, 1024, 2048
        },
        /*maxLength*/
        {
            10  //, 50, 100
        },
    })
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);  // Restrict number of iterations to avoid time out.

BENCHMARK(BM_PathArraynessLookup)
    ->ArgNames({
        "numberOfPaths",
        "maxLength",
        "numberOfPathsQuery",
        "maxLengthQuery",
    })
    ->ArgsProduct({
        /*numberOfPaths*/ {
            64  //, 512, 1024, 2048
        },
        /*maxLength*/
        {
            10  //, 50, 100
        },
        /*numberOfPathsQuery*/
        {
            50  //, 100, 200
        },
        /*maxLengthQuery*/
        {
            10  //, 50, 100
        },
    })
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);  // Restrict number of iterations to avoid time out.
}  // namespace mongo
