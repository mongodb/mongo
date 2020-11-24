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

#include "mongo/platform/basic.h"

#include <benchmark/benchmark.h>
#include <forward_list>

#include "mongo/util/cancelation.h"

namespace mongo {


void BM_create_single_token_from_source(benchmark::State& state) {
    CancelationSource source;
    for (auto _ : state) {
        benchmark::DoNotOptimize(source.token());
    }
}

void BM_uncancelable_token_ctor(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(CancelationToken::uncancelable());
    }
}

void BM_cancel_tokens_from_single_source(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();  // Do not time the construction and set-up of the source + tokens.
        CancelationSource source;
        for (int i = 0; i < state.range(0); ++i) {
            source.token().onCancel().unsafeToInlineFuture().getAsync([](auto) {});
        }
        state.ResumeTiming();
        source.cancel();
    }
}

void BM_check_if_token_from_source_canceled(benchmark::State& state) {
    CancelationSource source;
    auto token = source.token();
    for (auto _ : state) {
        benchmark::DoNotOptimize(token.isCanceled());
    }
}

void BM_cancelation_source_from_token_ctor(benchmark::State& state) {
    CancelationSource source;
    for (auto _ : state) {
        CancelationSource child(source.token());
        benchmark::DoNotOptimize(child);
    }
}

void BM_cancelation_source_default_ctor(benchmark::State& state) {
    for (auto _ : state) {
        CancelationSource source;
        benchmark::DoNotOptimize(source);
    }
}

/**
 * Constructs a cancelation 'hierarchy' of depth state.range(0), with one cancelation source at
 * each level and one token obtained from each source. When root.cancel() is called, the whole
 * hierarchy (all sources in the hierarchy, and any tokens obtained from any source in the
 * hierarchy) will be canceled.
 */
void BM_ranged_depth_cancelation_hierarchy(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        CancelationSource root;
        CancelationSource parent = root;
        // We use list to keep every cancelation source in the hierarchy in scope.
        std::forward_list<CancelationSource> list;
        for (int i = 0; i < state.range(0); ++i) {
            list.push_front(parent);
            CancelationSource child(parent.token());
            child.token().onCancel().unsafeToInlineFuture().getAsync([](auto) {});
            parent = child;
        }
        state.ResumeTiming();
        root.cancel();
    }
}

BENCHMARK(BM_create_single_token_from_source);
BENCHMARK(BM_uncancelable_token_ctor);
BENCHMARK(BM_cancel_tokens_from_single_source)->RangeMultiplier(10)->Range(1, 100 * 100 * 100);
BENCHMARK(BM_check_if_token_from_source_canceled);
BENCHMARK(BM_cancelation_source_from_token_ctor);
BENCHMARK(BM_cancelation_source_default_ctor);
BENCHMARK(BM_ranged_depth_cancelation_hierarchy)->RangeMultiplier(10)->Range(1, 100 * 100);

}  // namespace mongo