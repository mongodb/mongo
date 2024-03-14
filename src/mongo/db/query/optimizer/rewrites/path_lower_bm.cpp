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

#include <benchmark/benchmark.h>
#include <cstdint>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>

#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/util/str.h"

namespace mongo {
namespace optimizer {
namespace {

void benchmarkPath(benchmark::State& state, ABT& path) {
    auto prefixId = PrefixId::createForTests();
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(PathLowering{prefixId}.optimize(path));
        benchmark::ClobberMemory();
    }
}

template <typename T>
ABT makePath(int64_t nGets, int64_t nAnds, ABT base) {
    auto makeBranch = [&](int i) {
        auto branch = make<PathGet>(FieldNameType{"a"}, base);

        for (int j = 1; j < nGets; j++) {
            branch =
                make<PathGet>(FieldNameType{std::string(str::stream() << "a" << std::to_string(i))},
                              std::move(branch));
        }
        return branch;
    };

    auto path = makeBranch(-1);

    for (int i = 0; i < nAnds; i++) {
        path = make<PathComposeM>(makeBranch(i), std::move(path));
    }
    return make<T>(std::move(path), make<Variable>(ProjectionName("proj0")));
}

void BM_EvalFilter_PathGet(benchmark::State& state) {
    auto nLevels = state.range(0);
    auto path = makePath<EvalFilter>(
        nLevels, 0, make<PathCompare>(Operations::Eq, Constant::boolean(true)));
    benchmarkPath(state, path);
}

void BM_EvalPath_PathGet(benchmark::State& state) {
    auto nLevels = state.range(0);
    auto path = makePath<EvalPath>(nLevels, 0, make<PathIdentity>());
    benchmarkPath(state, path);
}

void BM_EvalFilter_PathComposeM(benchmark::State& state) {
    auto nLevels = state.range(0);
    auto path = makePath<EvalFilter>(
        1, nLevels, make<PathCompare>(Operations::Eq, Constant::boolean(true)));
    benchmarkPath(state, path);
}

void BM_EvalPath_PathComposeM(benchmark::State& state) {
    auto nLevels = state.range(0);
    auto path = makePath<EvalPath>(1, nLevels, make<PathIdentity>());
    benchmarkPath(state, path);
}

void BM_EvalFilter_GetAndComposeM(benchmark::State& state) {
    auto nGets = state.range(0);
    auto nAnds = state.range(1);
    auto path = makePath<EvalFilter>(
        nGets, nAnds, make<PathCompare>(Operations::Eq, Constant::boolean(true)));
    benchmarkPath(state, path);
}

void BM_EvalPath_GetAndComposeM(benchmark::State& state) {
    auto nGets = state.range(0);
    auto nAnds = state.range(1);
    auto path = makePath<EvalPath>(nGets, nAnds, make<PathIdentity>());
    benchmarkPath(state, path);
}

BENCHMARK(BM_EvalFilter_PathGet)->Arg(1)->Arg(10)->Arg(100)->Arg(200)->Arg(225)->Arg(250);
BENCHMARK(BM_EvalPath_PathGet)->Arg(1)->Arg(10)->Arg(100)->Arg(200)->Arg(225)->Arg(250);
BENCHMARK(BM_EvalFilter_PathComposeM)->Arg(1)->Arg(10)->Arg(100)->Arg(150);
BENCHMARK(BM_EvalPath_PathComposeM)->Arg(1)->Arg(10)->Arg(100)->Arg(150);
BENCHMARK(BM_EvalFilter_GetAndComposeM)
    ->Args({1, 1})
    ->Args({2, 1})
    ->Args({1, 2})
    ->Args({2, 2})
    ->Args({5, 5})
    ->Args({10, 5})
    ->Args({5, 10})
    ->Args({10, 10})
    ->Args({10, 15})
    ->Args({15, 10})
    ->Args({15, 15});
BENCHMARK(BM_EvalPath_GetAndComposeM)
    ->Args({1, 1})
    ->Args({2, 1})
    ->Args({1, 2})
    ->Args({2, 2})
    ->Args({5, 5})
    ->Args({10, 5})
    ->Args({5, 10})
    ->Args({10, 10})
    ->Args({10, 15})
    ->Args({15, 10})
    ->Args({15, 15});

}  // namespace
}  // namespace optimizer
}  // namespace mongo
