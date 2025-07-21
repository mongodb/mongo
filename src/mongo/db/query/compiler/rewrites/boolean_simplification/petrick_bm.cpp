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

#include <benchmark/benchmark.h>

namespace mongo::boolean_simplification {
/**
 * The classic example from Petrick's method papers. No essential terms, the number of terms
 * is reduced by half.
 */
void petrick_classic(benchmark::State& state) {
    std::vector<CoveredOriginalMinterms> data{
        {0, 1},
        {0, 3},
        {1, 2},
        {3, 4},
        {2, 5},
        {4, 5},
    };

    for (auto _ : state) {
        benchmark::DoNotOptimize(petricksMethod(data));
    }
}

BENCHMARK(petrick_classic);

/**
 * Petrick can do no simplifications.
 */
void petrick_noSimplifications(benchmark::State& state) {
    std::vector<CoveredOriginalMinterms> data(100);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i].emplace_back(static_cast<CoveredOriginalMinterms::value_type>(i));
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(petricksMethod(data));
    }
}

BENCHMARK(petrick_noSimplifications);

/**
 * Some terms are essential and some simplifications are possible.
 */
void petrick_essentialWithSimplications(benchmark::State& state) {
    std::vector<CoveredOriginalMinterms> data{
        {0, 1, 2},
        {2, 3},
        {0, 3},
        {4},
        {5},
        {6},
    };

    for (auto _ : state) {
        benchmark::DoNotOptimize(petricksMethod(data));
    }
}

BENCHMARK(petrick_essentialWithSimplications);
}  // namespace mongo::boolean_simplification
