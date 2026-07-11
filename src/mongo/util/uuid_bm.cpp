// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/uuid.h"

#include "mongo/util/processinfo.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

void BM_UuidGen(benchmark::State& state) {
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(UUID::gen());
    }
}

const auto kConcurrencyLimit = 2 * ProcessInfo::getNumAvailableCores();

BENCHMARK(BM_UuidGen)->ThreadRange(1, kConcurrencyLimit);

}  // namespace
}  // namespace mongo
