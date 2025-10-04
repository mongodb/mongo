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
        SorterChecksumVersion_parse(state.range(0),
                                    IDLParserContext("SorterChecksumCalculatorBenchmark")),
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
