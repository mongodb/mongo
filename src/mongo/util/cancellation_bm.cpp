// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
