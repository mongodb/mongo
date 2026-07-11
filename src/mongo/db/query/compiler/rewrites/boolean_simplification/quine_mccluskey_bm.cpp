// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/rewrites/boolean_simplification/quine_mccluskey.h"

#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_algebra.h"
#include "mongo/db/query/compiler/rewrites/boolean_simplification/petrick.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo::boolean_simplification {

/**
 * Benchmarks maxterm 'A' which simplifies to 'A'.
 */
void quineMcCluskey_1predicate(benchmark::State& state) {
    Bitset mask{"1"_b};
    Maxterm maxterm{
        Minterm{"1"_b, mask},
    };

    for (auto _ : state) {
        benchmark::DoNotOptimize(findPrimeImplicants(maxterm));
    }
}

/**
 * Benchmarks maxterm 'AB | A~B' which simplifies to 'A'.
 */
void quineMcCluskey_2predicates(benchmark::State& state) {
    Bitset mask{"11"_b};
    Maxterm maxterm{
        Minterm{"10"_b, mask},
        Minterm{"11"_b, mask},
    };

    for (auto _ : state) {
        benchmark::DoNotOptimize(findPrimeImplicants(maxterm));
    }
}

/**
 * Benchmarks maxterm '~A~B~C~D | ~A~B~CD | ~AB~C~D | ~AB~CD' which simplifies to '~A~C'.
 */
void quineMcCluskey_3predicates(benchmark::State& state) {
    // "ABC | A~BC = AC"
    Bitset mask{"111"_b};
    Maxterm maxterm{
        Minterm{"111"_b, mask},
        Minterm{"101"_b, mask},
    };

    for (auto _ : state) {
        benchmark::DoNotOptimize(findPrimeImplicants(maxterm));
    }
}

/**
 * Benchmarks maxterm '~A~B~C | ~AB~C | A~B~C | ~ABC | A~BC | ABC' which simplifies to '~A~C | ~B~C
 * | ~AB | A~B | BC | AC'.
 */
void quineMcCluskey_3predicates_complex(benchmark::State& state) {
    Bitset mask{"111"_b};
    Maxterm maxterm{
        Minterm{"000"_b, mask},
        Minterm{"010"_b, mask},
        Minterm{"100"_b, mask},
        Minterm{"011"_b, mask},
        Minterm{"101"_b, mask},
        Minterm{"111"_b, mask},
    };

    for (auto _ : state) {
        benchmark::DoNotOptimize(findPrimeImplicants(maxterm));
    }
}

/**
 * Benchmarks maxterm '~A~B~C~D | ~A~B~CD | ~AB~C~D | ~AB~CD' which simplifies to '~A~C'.
 */
void quineMcCluskey_4predicates_complex(benchmark::State& state) {
    Bitset mask{"1111"_b};
    Maxterm maxterm{
        Minterm{"0000"_b, mask},
        Minterm{"0001"_b, mask},
        Minterm{"0100"_b, mask},
        Minterm{"0101"_b, mask},
    };

    for (auto _ : state) {
        benchmark::DoNotOptimize(findPrimeImplicants(maxterm));
    }
}

/**
 * Benchmarks the case of N minterms of N predicates, every minterm has exactly 1 true
 * predicate so no simplifications is possible.
 */
void quineMcCluskey_noSimplifications(benchmark::State& state) {
    const auto numPredicates = static_cast<size_t>(state.range());
    Maxterm maxterm{numPredicates};
    for (size_t predicateIndex = 0; predicateIndex < numPredicates; ++predicateIndex) {
        maxterm.append(predicateIndex, true);
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(findPrimeImplicants(maxterm));
    }
}

/**
 * Benchmarks the case of N minterms of N predicates, every minterm is in form of 'AB' or 'A~B', so
 * the pair of such minterm can be simplified to just one minterm 'A'.
 */
void quineMcCluskey_someSimplifications(benchmark::State& state) {
    const auto numPredicates = static_cast<size_t>(state.range());
    Maxterm maxterm{numPredicates};
    for (size_t predicateIndex = 0; predicateIndex < numPredicates - 1; predicateIndex += 2) {
        maxterm.append(predicateIndex, true);
        maxterm.minterms.back().set(predicateIndex + 1, true);

        maxterm.append(predicateIndex, true);
        maxterm.minterms.back().set(predicateIndex + 1, false);
    }
    for (auto _ : state) {
        benchmark::DoNotOptimize(findPrimeImplicants(maxterm));
    }
}

BENCHMARK(quineMcCluskey_1predicate);
BENCHMARK(quineMcCluskey_2predicates);
BENCHMARK(quineMcCluskey_3predicates);
BENCHMARK(quineMcCluskey_3predicates_complex);
BENCHMARK(quineMcCluskey_4predicates_complex);
BENCHMARK(quineMcCluskey_noSimplifications)->DenseRange(5, 50, 5);
BENCHMARK(quineMcCluskey_someSimplifications)->DenseRange(5, 50, 5);

}  // namespace mongo::boolean_simplification
