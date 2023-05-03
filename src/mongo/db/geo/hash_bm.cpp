/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <benchmark/benchmark.h>

#include <random>
#include <vector>

#include "mongo/db/geo/hash.h"

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
