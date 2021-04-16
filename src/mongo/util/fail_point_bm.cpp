/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/fail_point.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failPointBench);
AtomicWord<bool> atomic;

void BM_AtomicLoadAndBranch(benchmark::State& state) {
    atomic.store(!!state.range(0));
    for (auto _ : state) {
        if (MONGO_unlikely(atomic.load())) {
            benchmark::DoNotOptimize(0);
        }
    }
}
BENCHMARK(BM_AtomicLoadAndBranch)->Range(0, 1);

void BM_FailPointShouldFail(benchmark::State& state) {
    failPointBench.setMode(state.range(0) ? FailPoint::alwaysOn : FailPoint::off);
    for (auto _ : state) {
        if (MONGO_unlikely(failPointBench.shouldFail())) {
            benchmark::DoNotOptimize(0);
        }
    }
    failPointBench.setMode(FailPoint::off);
}
BENCHMARK(BM_FailPointShouldFail)->Range(0, 1);

void BM_FailPointExecute(benchmark::State& state) {
    failPointBench.setMode(state.range(0) ? FailPoint::alwaysOn : FailPoint::off);
    for (auto _ : state)
        failPointBench.execute([](auto&&) { benchmark::DoNotOptimize(0); });
    failPointBench.setMode(FailPoint::off);
}
BENCHMARK(BM_FailPointExecute)->Range(0, 1);

void BM_FailPointExecuteIf(benchmark::State& state) {
    failPointBench.setMode(state.range(0) ? FailPoint::alwaysOn : FailPoint::off);
    for (auto _ : state)
        failPointBench.executeIf([](auto&&) { benchmark::DoNotOptimize(0); }, nullptr);
    failPointBench.setMode(FailPoint::off);
}
BENCHMARK(BM_FailPointExecuteIf)->Range(0, 1);

void BM_FailPointScoped(benchmark::State& state) {
    failPointBench.setMode(state.range(0) ? FailPoint::alwaysOn : FailPoint::off);
    for (auto _ : state) {
        auto scoped = failPointBench.scoped();
        if (MONGO_unlikely(scoped.isActive()))
            benchmark::DoNotOptimize(0);
    }
    failPointBench.setMode(FailPoint::off);
}
BENCHMARK(BM_FailPointScoped)->Range(0, 1);

void BM_FailPointScopedIf(benchmark::State& state) {
    failPointBench.setMode(state.range(0) ? FailPoint::alwaysOn : FailPoint::off);
    for (auto _ : state) {
        auto scoped = failPointBench.scopedIf([](auto&&) { return false; });
        if (MONGO_unlikely(scoped.isActive()))
            benchmark::DoNotOptimize(0);
    }
    failPointBench.setMode(FailPoint::off);
}
BENCHMARK(BM_FailPointScopedIf)->Range(0, 1);

}  // namespace
}  // namespace mongo
