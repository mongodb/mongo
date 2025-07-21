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
