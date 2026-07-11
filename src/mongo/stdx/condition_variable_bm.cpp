// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <benchmark/benchmark.h>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/stdx/condition_variable.h"

#include <condition_variable>
#include <mutex>

namespace mongo {

void BM_stdNotifyOne(benchmark::State& state) {
    std::condition_variable cv;  // NOLINT

    for (auto _ : state) {
        benchmark::ClobberMemory();
        cv.notify_one();
    }
}

void BM_stdxNotifyOneNoNotifyables(benchmark::State& state) {
    stdx::condition_variable cv;

    for (auto _ : state) {
        benchmark::ClobberMemory();
        cv.notify_one();
    }
}

volatile bool alwaysTrue = true;  // NOLINT

void BM_stdWaitWithTruePredicate(benchmark::State& state) {
    std::condition_variable cv;  // NOLINT
    std::mutex mutex;
    std::unique_lock<std::mutex> lk(mutex);

    for (auto _ : state) {
        benchmark::ClobberMemory();
        cv.wait(lk, [&] { return alwaysTrue; });
    }
}

void BM_stdxWaitWithTruePredicate(benchmark::State& state) {
    stdx::condition_variable cv;
    std::mutex mutex;
    std::unique_lock<std::mutex> lk(mutex);

    for (auto _ : state) {
        benchmark::ClobberMemory();
        cv.wait(lk, [&] { return alwaysTrue; });
    }
}

BENCHMARK(BM_stdNotifyOne);
BENCHMARK(BM_stdWaitWithTruePredicate);
BENCHMARK(BM_stdxNotifyOneNoNotifyables);
BENCHMARK(BM_stdxWaitWithTruePredicate);

}  // namespace mongo
