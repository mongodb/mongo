/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
    Promise<int> p;
    p.emplaceValue(1);  // before getFuture().
    return p.getFuture();
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

NOINLINE_DECL Future<int> makeReadyFutWithPromise2() {
    // This is the same as makeReadyFutWithPromise() except that this gets the Future first.
    benchmark::ClobberMemory();
    Promise<int> p;
    auto fut = p.getFuture();
    p.emplaceValue(1);  // after getFuture().
    return fut;
}

void BM_futureIntReadyWithPromise2(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(makeReadyFutWithPromise().then([](int i) { return i + 1; }).get());
    }
}

void BM_futureIntDeferredThen(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Promise<int> p;
        auto fut = p.getFuture().then([](int i) { return i + 1; });
        p.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}

void BM_futureIntDeferredThenImmediate(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Promise<int> p;
        auto fut = p.getFuture().then([](int i) { return Future<int>::makeReady(i + 1); });
        p.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}


void BM_futureIntDeferredThenReady(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Promise<int> p1;
        auto fut = p1.getFuture().then([&](int i) { return makeReadyFutWithPromise(); });
        p1.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}

void BM_futureIntDoubleDeferredThen(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Promise<int> p1;
        Promise<int> p2;
        auto fut = p1.getFuture().then([&](int i) { return p2.getFuture(); });
        p1.emplaceValue(1);
        p2.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}

void BM_futureInt3xDeferredThenNested(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Promise<int> p1;
        Promise<int> p2;
        Promise<int> p3;
        auto fut = p1.getFuture().then(
            [&](int i) { return p2.getFuture().then([&](int) { return p3.getFuture(); }); });
        p1.emplaceValue(1);
        p2.emplaceValue(1);
        p3.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}

void BM_futureInt3xDeferredThenChained(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Promise<int> p1;
        Promise<int> p2;
        Promise<int> p3;
        auto fut = p1.getFuture().then([&](int i) { return p2.getFuture(); }).then([&](int i) {
            return p3.getFuture();
        });
        p1.emplaceValue(1);
        p2.emplaceValue(1);
        p3.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}


void BM_futureInt4xDeferredThenNested(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Promise<int> p1;
        Promise<int> p2;
        Promise<int> p3;
        Promise<int> p4;
        auto fut = p1.getFuture().then([&](int i) {
            return p2.getFuture().then(
                [&](int) { return p3.getFuture().then([&](int) { return p4.getFuture(); }); });
        });
        p1.emplaceValue(1);
        p2.emplaceValue(1);
        p3.emplaceValue(1);
        p4.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}

void BM_futureInt4xDeferredThenChained(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Promise<int> p1;
        Promise<int> p2;
        Promise<int> p3;
        Promise<int> p4;
        auto fut = p1.getFuture()  //
                       .then([&](int i) { return p2.getFuture(); })
                       .then([&](int i) { return p3.getFuture(); })
                       .then([&](int i) { return p4.getFuture(); });
        p1.emplaceValue(1);
        p2.emplaceValue(1);
        p3.emplaceValue(1);
        p4.emplaceValue(1);
        benchmark::DoNotOptimize(std::move(fut).get());
    }
}


BENCHMARK(BM_plainIntReady);
BENCHMARK(BM_futureIntReady);
BENCHMARK(BM_futureIntReadyThen);
BENCHMARK(BM_futureIntReadyWithPromise);
BENCHMARK(BM_futureIntReadyWithPromiseThen);
BENCHMARK(BM_futureIntReadyWithPromise2);
BENCHMARK(BM_futureIntDeferredThen);
BENCHMARK(BM_futureIntDeferredThenImmediate);
BENCHMARK(BM_futureIntDeferredThenReady);
BENCHMARK(BM_futureIntDoubleDeferredThen);
BENCHMARK(BM_futureInt3xDeferredThenNested);
BENCHMARK(BM_futureInt3xDeferredThenChained);
BENCHMARK(BM_futureInt4xDeferredThenNested);
BENCHMARK(BM_futureInt4xDeferredThenChained);

}  // namespace mongo
