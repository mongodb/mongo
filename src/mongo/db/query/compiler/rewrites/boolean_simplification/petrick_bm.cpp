// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/query/compiler/rewrites/boolean_simplification/petrick.h"

#include <benchmark/benchmark.h>

namespace mongo::boolean_simplification {
/**
 * The classic example from Petrick's method papers. No essential terms, the number of terms
 * is reduced by half.
 */
void petrick_classic(benchmark::State& state) {
    std::vector<CoveredOriginalMinterms> data{
        CoveredOriginalMinterms{"000011"},
        CoveredOriginalMinterms{"001001"},
        CoveredOriginalMinterms{"000110"},
        CoveredOriginalMinterms{"011000"},
        CoveredOriginalMinterms{"100100"},
        CoveredOriginalMinterms{"110000"},
    };

    for (auto _ : state) {
        benchmark::DoNotOptimize(petricksMethod(data, 1000));
    }
}

BENCHMARK(petrick_classic);

/**
 * Petrick can do no simplifications.
 */
void petrick_noSimplifications(benchmark::State& state) {
    std::vector<CoveredOriginalMinterms> data(100);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i].resize(100);
        data[i].set(i);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(petricksMethod(data, 1000));
    }
}

BENCHMARK(petrick_noSimplifications);

/**
 * Some terms are essential and some simplifications are possible.
 */
void petrick_essentialWithSimplications(benchmark::State& state) {
    std::vector<CoveredOriginalMinterms> data{
        CoveredOriginalMinterms{"0000111"},
        CoveredOriginalMinterms{"0001100"},
        CoveredOriginalMinterms{"0001001"},
        CoveredOriginalMinterms{"0010000"},
        CoveredOriginalMinterms{"0100000"},
        CoveredOriginalMinterms{"1000000"},
    };

    for (auto _ : state) {
        benchmark::DoNotOptimize(petricksMethod(data, 1000));
    }
}

BENCHMARK(petrick_essentialWithSimplications);
}  // namespace mongo::boolean_simplification
