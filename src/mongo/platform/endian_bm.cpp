/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "mongo/base/string_data.h"
#include "mongo/platform/endian.h"

namespace mongo {
namespace {

template <typename T>
void BM_NativeToBig(benchmark::State& state) {
    std::uint64_t items = 0;
    std::vector<T> in(1000);
    std::iota(in.begin(), in.end(), 0);
    for (auto _ : state) {
        for (auto& i : in)
            benchmark::DoNotOptimize(endian::nativeToBig(i));
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

template <typename T>
void BM_NativeToLittle(benchmark::State& state) {
    std::uint64_t items = 0;
    std::vector<T> in(1000);
    std::iota(in.begin(), in.end(), 0);
    for (auto _ : state) {
        for (auto& i : in)
            benchmark::DoNotOptimize(endian::nativeToLittle(i));
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK_TEMPLATE(BM_NativeToBig, uint16_t);
BENCHMARK_TEMPLATE(BM_NativeToBig, uint32_t);
BENCHMARK_TEMPLATE(BM_NativeToBig, uint64_t);
BENCHMARK_TEMPLATE(BM_NativeToLittle, uint16_t);
BENCHMARK_TEMPLATE(BM_NativeToLittle, uint32_t);
BENCHMARK_TEMPLATE(BM_NativeToLittle, uint64_t);

}  // namespace
}  // namespace mongo
