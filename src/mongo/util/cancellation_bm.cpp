/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/util/cancellation.h"

#include "mongo/util/future.h"

#include <forward_list>
#include <utility>

#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {


void BM_CreateSingleTokenFromSource(benchmark::State& state) {
    CancellationSource source;
    for (auto _ : state) {
        benchmark::DoNotOptimize(source.token());
    }
}

void BM_UncancelableTokenCtor(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(CancellationToken::uncancelable());
    }
}

void BM_CancelTokensFromSingleSource(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();  // Do not time the construction and set-up of the source + tokens.
        CancellationSource source;
        for (int i = 0; i < state.range(0); ++i) {
            source.token().onCancel().unsafeToInlineFuture().getAsync([](auto) {});
        }
        state.ResumeTiming();
        source.cancel();
    }
}

void BM_CheckIfTokenFromSourceCanceled(benchmark::State& state) {
    CancellationSource source;
    auto token = source.token();
    for (auto _ : state) {
        benchmark::DoNotOptimize(token.isCanceled());
    }
}

void BM_CancellationSourceFromTokenCtor(benchmark::State& state) {
    CancellationSource source;
    for (auto _ : state) {
        CancellationSource child(source.token());
        benchmark::DoNotOptimize(child);
    }
}

void BM_CancellationSourceDefaultCtor(benchmark::State& state) {
    for (auto _ : state) {
        CancellationSource source;
        benchmark::DoNotOptimize(source);
    }
}

/**
 * Constructs a cancellation 'hierarchy' of depth state.range(0), with one cancellation source at
 * each level and one token obtained from each source. When root.cancel() is called, the whole
 * hierarchy (all sources in the hierarchy, and any tokens obtained from any source in the
 * hierarchy) will be canceled.
 */
void BM_RangedDepthCancellationHierarchy(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        CancellationSource root;
        CancellationSource parent = root;
        // We use list to keep every cancellation source in the hierarchy in scope.
        std::forward_list<CancellationSource> list;
        for (int i = 0; i < state.range(0); ++i) {
            list.push_front(parent);
            CancellationSource child(parent.token());
            child.token().onCancel().unsafeToInlineFuture().getAsync([](auto) {});
            parent = child;
        }
        state.ResumeTiming();
        root.cancel();
    }
}

BENCHMARK(BM_CreateSingleTokenFromSource);
BENCHMARK(BM_UncancelableTokenCtor);
BENCHMARK(BM_CancelTokensFromSingleSource)->RangeMultiplier(10)->Range(1, 100 * 100 * 100);
BENCHMARK(BM_CheckIfTokenFromSourceCanceled);
BENCHMARK(BM_CancellationSourceFromTokenCtor);
BENCHMARK(BM_CancellationSourceDefaultCtor);
BENCHMARK(BM_RangedDepthCancellationHierarchy)->RangeMultiplier(10)->Range(1, 1000);

}  // namespace mongo
