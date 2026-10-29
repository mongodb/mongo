// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/base64.h"

#include <cstddef>
#include <iosfwd>
#include <string>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

namespace mongo {
namespace {


void BM_Base64EncodeString(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in(len, 'x');
    std::string out;

    for (auto _ : state) {
        benchmark::DoNotOptimize(out = base64::encode(in));
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64EncodeString)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);

void BM_Base64EncodeStringStream(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in(len, 'x');

    for (auto _ : state) {
        std::stringstream out;
        base64::encode(out, in);
        benchmark::DoNotOptimize(out.str());
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64EncodeStringStream)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);

void BM_Base64EncodeFmtMemoryBuffer(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in(len, 'x');

    for (auto _ : state) {
        fmt::memory_buffer buf;
        base64::encode(buf, in);
        benchmark::DoNotOptimize(fmt::to_string(buf));
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64EncodeFmtMemoryBuffer)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);


void BM_Base64DecodeString(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in = base64::encode(std::string(len, 'x'));
    for (auto _ : state) {
        benchmark::DoNotOptimize(base64::decode(in));
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64DecodeString)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);

void BM_Base64DecodeStringStream(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in = base64::encode(std::string(len, 'x'));
    for (auto _ : state) {
        std::stringstream out;
        base64::decode(out, in);
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64DecodeStringStream)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);

void BM_Base64DecodeFmtMemoryBuffer(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in = base64::encode(std::string(len, 'x'));
    for (auto _ : state) {
        fmt::memory_buffer buffer;
        base64::decode(buffer, in);
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64DecodeFmtMemoryBuffer)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);

}  // namespace
}  // namespace mongo
