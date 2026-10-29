// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/geo/hash.h"

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

void BM_hashSpeed(benchmark::State& state) {
    auto length = state.range();
    std::vector<GeoHash> hashes(length);

    std::random_device device{};
    uint32_t valueX = device();
    uint32_t valueY = device();
    for (auto _ : state) {
        for (auto& geohash : hashes) {
            auto newHash = GeoHash(valueX, valueY, 32);
            benchmark::DoNotOptimize(newHash);
            geohash = std::move(newHash);
            benchmark::ClobberMemory();
        }
    }
}

void BM_unhashSpeed(benchmark::State& state) {
    auto length = state.range();
    std::vector<GeoHash> hashes(length);

    std::random_device device{};
    for (auto& geohash : hashes) {
        geohash = GeoHash(device(), device(), 32);
    }

    for (auto _ : state) {
        for (auto& geohash : hashes) {
            uint32_t x, y;
            geohash.unhash(&x, &y);
            benchmark::DoNotOptimize(x);
            benchmark::DoNotOptimize(y);
        }
    }
}

BENCHMARK(BM_hashSpeed)->Range(1, 1 << 10);
BENCHMARK(BM_unhashSpeed)->Range(1, 1 << 10);
}  // namespace
}  // namespace mongo
