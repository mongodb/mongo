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

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"

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

volatile bool alwaysTrue = true;

void BM_stdWaitWithTruePredicate(benchmark::State& state) {
    std::condition_variable cv;  // NOLINT
    stdx::mutex mutex;
    stdx::unique_lock<stdx::mutex> lk(mutex);

    for (auto _ : state) {
        benchmark::ClobberMemory();
        cv.wait(lk, [&] { return alwaysTrue; });
    }
}

void BM_stdxWaitWithTruePredicate(benchmark::State& state) {
    stdx::condition_variable cv;
    stdx::mutex mutex;
    stdx::unique_lock<stdx::mutex> lk(mutex);

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
