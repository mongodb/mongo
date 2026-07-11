// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_algebra.h"

#include <benchmark/benchmark.h>

namespace mongo::boolean_simplification {
/**
 * Best case: create a maxterm with a single minterm.
 */
void bitsetAlgebra_createAndMaxterm(benchmark::State& state) {
    for (auto _ : state) {
        Maxterm maxterm{64};
        maxterm.append(10, true);
    }
}

/**
 * Worst case: create a maxterm with N minterms of size N and 1 set predicate.
 */
void bitsetAlgebra_createOrMaxterm(benchmark::State& state) {
    const size_t numPredicates = static_cast<size_t>(state.range());

    for (auto _ : state) {
        Maxterm maxterm{numPredicates};
        for (size_t predicateIndex = 0; predicateIndex < numPredicates; ++predicateIndex) {
            maxterm.append(predicateIndex % maxterm.numberOfBits(), true);
        }
    }
}

/**
 * Middle case: create a maxtern with N minterms and N % kBitsetNumberOfBits set predicates.
 */
void bitsetAlgebra_createMaxterm(benchmark::State& state) {
    const size_t numMinterms = static_cast<size_t>(state.range());
    const size_t numPredicates = numMinterms;

    for (auto _ : state) {
        Maxterm maxterm{numPredicates};
        for (size_t index = 0; index < numMinterms; ++index) {
            maxterm.appendEmpty();
            for (size_t predicateIndex = 0; predicateIndex < numPredicates; ++predicateIndex) {
                maxterm.minterms.back().set(predicateIndex, true);
            }
        }
    }
}

BENCHMARK(bitsetAlgebra_createAndMaxterm);
BENCHMARK(bitsetAlgebra_createOrMaxterm)->RangeMultiplier(10)->Range(10, 10000);
BENCHMARK(bitsetAlgebra_createMaxterm)->Args({3})->Args({7})->Args({10})->Args({13});
}  // namespace mongo::boolean_simplification
