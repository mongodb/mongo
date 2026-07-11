// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/future.h"

#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_impl.h"

#include <type_traits>
#include <utility>

#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {

MONGO_COMPILER_NOINLINE int makeReadyInt() {
    benchmark::ClobberMemory();
    return 1;
}

void BM_plainIntReady(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(makeReadyInt() + 1);
    }
}

MONGO_COMPILER_NOINLINE Future<int> makeReadyFut() {
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

MONGO_COMPILER_NOINLINE Future<int> makeReadyFutWithPromise() {
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
