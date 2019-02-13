/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/inline_decls.h"
#include "mongo/util/future.h"

namespace mongo {

NOINLINE_DECL int makeReadyInt() {
    benchmark::ClobberMemory();
    return 1;
}

void BM_plainIntReady(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(makeReadyInt() + 1);
    }
}

NOINLINE_DECL Future<int> makeReadyFut() {
    benchmark::ClobberMemory();
    return Future<int>::makeReady(1);
}

void BM_futureIntReady(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(makeReadyFut().get() + 1);
    }
}

void BM_futureIntReadyThen(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(makeReadyFut().then([](int i) { return i + 1; }).get());
    }
}

NOINLINE_DECL Future<int> makeReadyFutWithPromise() {
    benchmark::ClobberMemory();
    auto pf = makePromiseFuture<int>();
    pf.promise.emplaceValue(1);
    return std::move(pf.future);
}

void BM_futureIntReadyWithPromise(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(makeReadyFutWithPromise().get() + 1);
    }
}

void BM_futureIntReadyWithPromiseThen(benchmark::State& state) {
    for (auto _ : state) {
        int i = makeReadyFutWithPromise().then([](int i) { return i + 1; }).get();
        benchmark::DoNotOptimize(i);
    }
}

void BM_futureIntDeferredThen(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto pf = makePromiseFuture<int>();
        auto fut = std::move(pf.future).then([](int i) { return i + 1; });
        pf.promise.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}

void BM_futureIntDeferredThenImmediate(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto pf = makePromiseFuture<int>();
        auto fut = std::move(pf.future).then([](int i) { return Future<int>::makeReady(i + 1); });
        pf.promise.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}


void BM_futureIntDeferredThenReady(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto pf = makePromiseFuture<int>();
        auto fut = std::move(pf.future).then([](int i) { return makeReadyFutWithPromise(); });
        pf.promise.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}

void BM_futureIntDoubleDeferredThen(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto pf1 = makePromiseFuture<int>();
        auto pf2 = makePromiseFuture<int>();
        auto fut = std::move(pf1.future).then([&](int i) { return std::move(pf2.future); });
        pf1.promise.emplaceValue(1);
        pf2.promise.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}

void BM_futureInt3xDeferredThenNested(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto pf1 = makePromiseFuture<int>();
        auto pf2 = makePromiseFuture<int>();
        auto pf3 = makePromiseFuture<int>();
        auto fut = std::move(pf1.future).then([&](int i) {
            return std::move(pf2.future).then([&](int) { return std::move(pf3.future); });
        });
        pf1.promise.emplaceValue(1);
        pf2.promise.emplaceValue(1);
        pf3.promise.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}

void BM_futureInt3xDeferredThenChained(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto pf1 = makePromiseFuture<int>();
        auto pf2 = makePromiseFuture<int>();
        auto pf3 = makePromiseFuture<int>();
        auto fut = std::move(pf1.future)
                       .then([&](int i) { return std::move(pf2.future); })
                       .then([&](int i) { return std::move(pf3.future); });
        pf1.promise.emplaceValue(1);
        pf2.promise.emplaceValue(1);
        pf3.promise.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}


void BM_futureInt4xDeferredThenNested(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto pf1 = makePromiseFuture<int>();
        auto pf2 = makePromiseFuture<int>();
        auto pf3 = makePromiseFuture<int>();
        auto pf4 = makePromiseFuture<int>();
        auto fut = std::move(pf1.future).then([&](int i) {
            return std::move(pf2.future).then([&](int) {
                return std::move(pf3.future).then([&](int) { return std::move(pf4.future); });
            });
        });
        pf1.promise.emplaceValue(1);
        pf2.promise.emplaceValue(1);
        pf3.promise.emplaceValue(1);
        pf4.promise.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}

void BM_futureInt4xDeferredThenChained(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        auto pf1 = makePromiseFuture<int>();
        auto pf2 = makePromiseFuture<int>();
        auto pf3 = makePromiseFuture<int>();
        auto pf4 = makePromiseFuture<int>();
        auto fut = std::move(pf1.future)  //
                       .then([&](int i) { return std::move(pf2.future); })
                       .then([&](int i) { return std::move(pf3.future); })
                       .then([&](int i) { return std::move(pf4.future); });
        pf1.promise.emplaceValue(1);
        pf2.promise.emplaceValue(1);
        pf3.promise.emplaceValue(1);
        pf4.promise.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}


BENCHMARK(BM_plainIntReady);
BENCHMARK(BM_futureIntReady);
BENCHMARK(BM_futureIntReadyThen);
BENCHMARK(BM_futureIntReadyWithPromise);
BENCHMARK(BM_futureIntReadyWithPromiseThen);
BENCHMARK(BM_futureIntDeferredThen);
BENCHMARK(BM_futureIntDeferredThenImmediate);
BENCHMARK(BM_futureIntDeferredThenReady);
BENCHMARK(BM_futureIntDoubleDeferredThen);
BENCHMARK(BM_futureInt3xDeferredThenNested);
BENCHMARK(BM_futureInt3xDeferredThenChained);
BENCHMARK(BM_futureInt4xDeferredThenNested);
BENCHMARK(BM_futureInt4xDeferredThenChained);

}  // namespace mongo
