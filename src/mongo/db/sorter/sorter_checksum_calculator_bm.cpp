// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sorter/sorter_checksum_calculator.h"

#include "mongo/platform/random.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

class SorterChecksumCalculatorBenchmark : public benchmark::Fixture {
private:
    static constexpr int32_t kSeed = 1;

public:
    SorterChecksumCalculatorBenchmark() : _random(kSeed) {}

    void benchmarkSorterChecksumCalculator(SorterChecksumVersion version,
                                           size_t dataSize,
                                           benchmark::State& state) {
        std::vector<char> data(dataSize);
        _random.fill(data.data(), dataSize);

        for (auto keepRunning : state) {
            SorterChecksumCalculator calculator(version);
            calculator.addData(data.data(), dataSize);
            benchmark::DoNotOptimize(calculator.checksum());
            benchmark::ClobberMemory();
        }
    };

private:
    PseudoRandom _random;
};

BENCHMARK_DEFINE_F(SorterChecksumCalculatorBenchmark, BM_SorterChecksumCalculator)
(benchmark::State& state) {
    benchmarkSorterChecksumCalculator(
        idl::deserialize<SorterChecksumVersion>(
            state.range(0), IDLParserContext("SorterChecksumCalculatorBenchmark")),
        state.range(1),
        state);
}

BENCHMARK_REGISTER_F(SorterChecksumCalculatorBenchmark, BM_SorterChecksumCalculator)
    ->Args({1, 128})
    ->Args({1, 1024})
    ->Args({1, 128 * 1024})
    ->Args({2, 128})
    ->Args({2, 1024})
    ->Args({2, 128 * 1024});

}  // namespace
}  // namespace mongo
