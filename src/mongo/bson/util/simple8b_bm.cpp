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

#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <iterator>

#include <boost/cstdint.hpp>

#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/simple8b.h"
#include "mongo/bson/util/simple8b_builder.h"
#include "mongo/util/shared_buffer.h"

namespace mongo {

void BM_increasingValues(benchmark::State& state) {
    size_t totalBytes = 0;
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Simple8bBuilder<uint64_t> s8bBuilder(
            [&totalBytes](uint64_t simple8bBlock) { totalBytes += sizeof(simple8bBlock); });
        for (auto j = 0; j < state.range(0); j++)
            s8bBuilder.append((uint64_t)j);

        s8bBuilder.flush();
    }
    state.SetBytesProcessed(totalBytes);
}

void BM_rle(benchmark::State& state) {
    size_t totalBytes = 0;
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Simple8bBuilder<uint64_t> s8bBuilder(
            [&totalBytes](uint64_t simple8bBlock) { totalBytes += sizeof(simple8bBlock); });
        for (auto j = 0; j < state.range(0); j++)
            s8bBuilder.append(0);

        s8bBuilder.flush();
    }
    state.SetBytesProcessed(totalBytes);
}

void BM_changingSmallValues(benchmark::State& state) {
    size_t totalBytes = 0;
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Simple8bBuilder<uint64_t> s8bBuilder(
            [&totalBytes](uint64_t simple8bBlock) { totalBytes += sizeof(simple8bBlock); });
        for (auto j = 0; j < state.range(0); j++)
            s8bBuilder.append(j % 2);

        s8bBuilder.flush();
    }
    state.SetBytesProcessed(totalBytes);
}

void BM_changingLargeValues(benchmark::State& state) {
    size_t totalBytes = 0;
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Simple8bBuilder<uint64_t> s8bBuilder(
            [&totalBytes](uint64_t simple8bBlock) { totalBytes += sizeof(simple8bBlock); });
        for (auto j = 0; j < state.range(0); j++) {
            uint64_t value = j % 2 ? 0xE0 : 0xFF;
            s8bBuilder.append(value);
        }

        s8bBuilder.flush();
    }
    state.SetBytesProcessed(totalBytes);
}

void BM_selectorSeven(benchmark::State& state) {
    size_t totalBytes = 0;
    for (auto _ : state) {
        benchmark::ClobberMemory();
        Simple8bBuilder<uint64_t> s8bBuilder(
            [&totalBytes](uint64_t simple8bBlock) { totalBytes += sizeof(simple8bBlock); });

        for (auto j = 0; j < state.range(0); j++) {
            uint64_t value = j % 2 ? 0b1000000000000 : 0b11000000000000;
            s8bBuilder.append(value);
        }
        s8bBuilder.flush();
    }
    state.SetBytesProcessed(totalBytes);
}

void BM_decode(benchmark::State& state) {
    size_t totalBytes = 0;

    BufBuilder _buffer;
    Simple8bBuilder<uint64_t> s8bBuilder(
        [&_buffer](uint64_t simple8bBlock) { _buffer.appendNum(simple8bBlock); });

    // Small values.
    for (auto j = 0; j < 100; j++)
        s8bBuilder.append(j % 2);

    // RLE.
    for (auto j = 0; j < 200; j++)
        s8bBuilder.append(0);

    // Large Values.
    for (auto j = 0; j < 100; j++) {
        uint64_t value = j % 2 ? 0xE0 : 0xFF;
        s8bBuilder.append(value);
    }

    s8bBuilder.flush();

    auto size = _buffer.len();
    auto buf = _buffer.release();
    Simple8b<uint64_t> s8b(buf.get(), size);

    for (auto _ : state) {
        benchmark::ClobberMemory();
        // This will iterate over the whole range and require decompression.
        benchmark::DoNotOptimize(std::distance(s8b.begin(), s8b.end()));
        totalBytes += size;
    }

    state.SetBytesProcessed(totalBytes);
}

BENCHMARK(BM_increasingValues)->Arg(100);
BENCHMARK(BM_rle)->Arg(100);
BENCHMARK(BM_changingSmallValues)->Arg(100);
BENCHMARK(BM_changingLargeValues)->Arg(100);
BENCHMARK(BM_selectorSeven)->Arg(100);
BENCHMARK(BM_decode);

}  // namespace mongo
