// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/hex.h"

#include <cstddef>
#include <string>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

void BM_HexEncode(benchmark::State& state) {
    std::string in(state.range(0), 'x');
    size_t outBytes = 0;
    for (auto _ : state) {
        std::string out;
        benchmark::DoNotOptimize(out = hexblob::encode(in));
        outBytes += out.size();
    }
    state.counters["outBytes"] = outBytes;
}

void BM_HexDecode(benchmark::State& state) {
    const size_t length = static_cast<size_t>(state.range(0));
    std::string in;
    for (size_t i = 0; i < length; ++i)
        in.push_back("0123456789abcdef"[i & 0xf]);
    size_t outBytes = 0;
    for (auto _ : state) {
        std::string out;
        benchmark::DoNotOptimize(out = hexblob::decode(in));
        outBytes += out.size();
    }
    state.counters["outBytes"] = outBytes;
}

void BM_HexDecodeFromValidSizedInput(benchmark::State& state) {
    const size_t length = static_cast<size_t>(state.range(0));
    std::string in;
    for (size_t i = 0; i < length; ++i)
        in.push_back("0123456789abcdef"[i & 0xf]);
    size_t outBytes = 0;
    for (auto _ : state) {
        std::string out;
        benchmark::DoNotOptimize(out = hexblob::decodeFromValidSizedInput(in));
        outBytes += out.size();
    }
    state.counters["outBytes"] = outBytes;
}

void customRanges(benchmark::internal::Benchmark* b) {
    b->Arg(0)->RangeMultiplier(2)->Range(8, 128 << 10);
}

BENCHMARK(BM_HexEncode)->Apply(customRanges);
BENCHMARK(BM_HexDecode)->Apply(customRanges);
BENCHMARK(BM_HexDecodeFromValidSizedInput)->Apply(customRanges);

}  // namespace
}  // namespace mongo
