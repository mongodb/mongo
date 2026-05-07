/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/sorter/sorter.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/sorter/file.h"
#include "mongo/db/sorter/file_based_spiller.h"
#include "mongo/db/sorter/sorter_checksum_calculator.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/platform/random.h"
#include "mongo/util/bufreader.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/optional.hpp>

namespace mongo::sorter {
namespace {

class IntKV {
public:
    IntKV(int64_t i = 0) : _i(i) {}
    operator int64_t() const {
        return _i;
    }

    struct SorterDeserializeSettings {};
    void serializeForSorter(BufBuilder& buf) const {
        buf.appendNum(_i);
    }
    static IntKV deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
        return IntKV(buf.read<LittleEndian<int64_t>>().value);
    }
    int memUsageForSorter() const {
        return sizeof(IntKV);
    }
    IntKV getOwned() const {
        return *this;
    }
    void makeOwned() {}

private:
    int64_t _i;
};

class IntKVComparator {
public:
    int operator()(const IntKV& lhs, const IntKV& rhs) const {
        const int64_t l = lhs;
        const int64_t r = rhs;
        return (l > r) - (l < r);
    }
};

enum class InputPattern { kRandom, kPresorted, kReverseSorted };

std::vector<int64_t> makeInput(size_t n, InputPattern pattern, int32_t seed = 1) {
    std::vector<int64_t> data(n);
    PseudoRandom rnd(seed);
    for (size_t i = 0; i < n; ++i) {
        data[i] = rnd.nextInt64();
    }
    switch (pattern) {
        case InputPattern::kRandom:
            break;
        case InputPattern::kPresorted:
            std::sort(data.begin(), data.end());
            break;
        case InputPattern::kReverseSorted:
            std::sort(data.begin(), data.end(), std::greater<int64_t>());
            break;
    }
    return data;
}

class ScopedSpillDir {
public:
    ScopedSpillDir() {
        static std::atomic<uint64_t> counter{0};
        auto id = counter.fetch_add(1, std::memory_order_relaxed);
        _path = boost::filesystem::temp_directory_path() /
            ("mongo_sorter_bm_" + std::to_string(::getpid()) + "_" + std::to_string(id));
        boost::filesystem::create_directories(_path);
    }
    ~ScopedSpillDir() {
        boost::filesystem::remove_all(_path);
    }
    ScopedSpillDir(const ScopedSpillDir&) = delete;
    ScopedSpillDir& operator=(const ScopedSpillDir&) = delete;

    const boost::filesystem::path& path() const {
        return _path;
    }

private:
    boost::filesystem::path _path;
};

using IntSorter = Sorter<IntKV, IntKV>;
using IntSpiller = FileBasedSpiller<IntKV, IntKV, IntKVComparator>;

// Large enough that ensureSufficientDiskSpaceForSpilling always passes during the benchmark.
constexpr int64_t kBenchmarkMinAvailableDiskBytes = 500LL * 1024 * 1024;

std::shared_ptr<IntSpiller> makeFileSpiller(const boost::filesystem::path& spillDir) {
    return std::make_shared<IntSpiller>(spillDir,
                                        /*fileStats=*/nullptr,
                                        /*dbName=*/boost::none,
                                        kLatestChecksumVersion,
                                        kBenchmarkMinAvailableDiskBytes);
}

class SorterBenchmark : public benchmark::Fixture {
public:
    void SetUp(benchmark::State&) override {
        _spillDir.emplace();
    }
    void TearDown(benchmark::State&) override {
        _spillDir.reset();
    }

    const boost::filesystem::path& spillDir() const {
        return _spillDir->path();
    }

private:
    boost::optional<ScopedSpillDir> _spillDir;
};

// Drives the full add()-then-done() lifecycle and consumes every output pair. Returns the number
// of pairs produced so the caller can validate the benchmark did real work.
template <typename SorterT>
size_t runSorter(SorterT& sorter, const std::vector<int64_t>& input) {
    for (int64_t v : input) {
        sorter.add(IntKV(v), IntKV(-v));
    }
    auto iter = sorter.done();
    size_t produced = 0;
    while (iter->more()) {
        auto pair = iter->next();
        benchmark::DoNotOptimize(pair);
        ++produced;
    }
    return produced;
}

BENCHMARK_DEFINE_F(SorterBenchmark, BM_NoLimitSortInMemory)(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const auto input = makeInput(n, InputPattern::kRandom);
    for (auto _ : state) {
        SortOptions opts;
        opts.MaxMemoryUsageBytes(static_cast<size_t>(1) << 30);
        auto sorter = IntSorter::make<IntKVComparator>(
            opts, IntKVComparator(), /*spiller=*/nullptr, /*settings=*/{});
        auto produced = runSorter(*sorter, input);
        benchmark::DoNotOptimize(produced);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n));
}
BENCHMARK_REGISTER_F(SorterBenchmark, BM_NoLimitSortInMemory)
    ->Arg(1 << 10)
    ->Arg(1 << 14)
    ->Arg(1 << 18)
    ->Unit(benchmark::kMillisecond);

BENCHMARK_DEFINE_F(SorterBenchmark, BM_NoLimitSortByInputPattern)(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const auto pattern = static_cast<InputPattern>(state.range(1));
    const auto input = makeInput(n, pattern);

    for (auto _ : state) {
        SortOptions opts;
        opts.MaxMemoryUsageBytes(static_cast<size_t>(1) << 30);
        auto sorter = IntSorter::template make<IntKVComparator>(
            opts, IntKVComparator(), /*spiller=*/nullptr, /*settings=*/{});
        auto produced = runSorter(*sorter, input);
        benchmark::DoNotOptimize(produced);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n));
}
BENCHMARK_REGISTER_F(SorterBenchmark, BM_NoLimitSortByInputPattern)
    ->Args({1 << 16, static_cast<int>(InputPattern::kRandom)})
    ->Args({1 << 16, static_cast<int>(InputPattern::kPresorted)})
    ->Args({1 << 16, static_cast<int>(InputPattern::kReverseSorted)})
    ->Unit(benchmark::kMillisecond);

BENCHMARK_DEFINE_F(SorterBenchmark, BM_NoLimitSortWithSpilling)(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const size_t targetSpills = static_cast<size_t>(state.range(1));
    const auto input = makeInput(n, InputPattern::kRandom);

    // Pick a memory budget that yields roughly `targetSpills` spills. The sorter accounts
    // key.memUsageForSorter() + value.memUsageForSorter() = 16 bytes per IntKV pair.
    constexpr size_t kBytesPerPair = 16;
    const size_t memBudget = std::max<size_t>(64 * 1024, (n * kBytesPerPair) / targetSpills);

    for (auto _ : state) {
        SortOptions opts;
        opts.MaxMemoryUsageBytes(memBudget);
        auto sorter = IntSorter::template make<IntKVComparator>(
            opts, IntKVComparator(), makeFileSpiller(spillDir()), /*settings=*/{});
        auto produced = runSorter(*sorter, input);
        benchmark::DoNotOptimize(produced);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n));
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n) *
                            static_cast<int64_t>(sizeof(IntKV) * 2));
}
BENCHMARK_REGISTER_F(SorterBenchmark, BM_NoLimitSortWithSpilling)
    ->Args({1 << 16, 4})   // 4 spills
    ->Args({1 << 16, 16})  // 16 spills, exercises multi-level merge
    ->Args({1 << 18, 16})
    ->Unit(benchmark::kMillisecond);

BENCHMARK_DEFINE_F(SorterBenchmark, BM_TopKSort)(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const size_t k = static_cast<size_t>(state.range(1));
    const auto input = makeInput(n, InputPattern::kRandom);
    for (auto _ : state) {
        SortOptions opts;
        opts.Limit(k).MaxMemoryUsageBytes(static_cast<size_t>(1) << 30);
        auto sorter = IntSorter::make<IntKVComparator>(
            opts, IntKVComparator(), /*spiller=*/nullptr, /*settings=*/{});
        auto produced = runSorter(*sorter, input);
        benchmark::DoNotOptimize(produced);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n));
}
BENCHMARK_REGISTER_F(SorterBenchmark, BM_TopKSort)
    ->Args({1 << 16, 10})
    ->Args({1 << 16, 100})
    ->Args({1 << 16, 1000})
    ->Args({1 << 18, 100})
    ->Unit(benchmark::kMillisecond);

BENCHMARK_DEFINE_F(SorterBenchmark, BM_LimitOneSort)(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    const auto input = makeInput(n, InputPattern::kRandom);
    for (auto _ : state) {
        SortOptions opts;
        opts.Limit(1);
        auto sorter = IntSorter::make<IntKVComparator>(
            opts, IntKVComparator(), /*spiller=*/nullptr, /*settings=*/{});
        auto produced = runSorter(*sorter, input);
        benchmark::DoNotOptimize(produced);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n));
}
BENCHMARK_REGISTER_F(SorterBenchmark, BM_LimitOneSort)
    ->Arg(1 << 14)
    ->Arg(1 << 18)
    ->Arg(1 << 20)
    ->Unit(benchmark::kMillisecond);

BENCHMARK_DEFINE_F(SorterBenchmark, BM_MergeIterator)(benchmark::State& state) {
    const size_t numIters = static_cast<size_t>(state.range(0));
    const size_t perIter = static_cast<size_t>(state.range(1));
    const size_t totalPairs = numIters * perIter;

    for (auto _ : state) {
        // Rebuild spilled files inside PauseTiming each iteration so only the merge phase is timed.
        state.PauseTiming();
        std::vector<std::shared_ptr<Iterator<IntKV, IntKV>>> iters;
        iters.reserve(numIters);
        {
            // Interleaved keys per iterator: run i produces i, i+numIters, i+2*numIters, ...
            // This guarantees the merge actually interleaves across sources rather than draining
            // them one at a time.
            SortOptions opts;
            for (size_t i = 0; i < numIters; ++i) {
                auto file = std::make_shared<File>(nextFileName(spillDir()), /*fileStats=*/nullptr);
                FileBasedStorage<IntKV, IntKV> storage(
                    file, /*dbName=*/boost::none, kLatestChecksumVersion);
                auto writer = storage.makeWriter(opts, /*settings=*/{});
                for (size_t j = 0; j < perIter; ++j) {
                    int64_t key = static_cast<int64_t>(j * numIters + i);
                    writer->addAlreadySorted(IntKV(key), IntKV(-key));
                }
                iters.push_back(writer->done());
            }
        }
        state.ResumeTiming();

        SortOptions opts;
        auto mergeIter = merge<IntKV, IntKV>(std::span{iters}, opts, IntKVComparator());
        size_t produced = 0;
        while (mergeIter->more()) {
            auto pair = mergeIter->next();
            benchmark::DoNotOptimize(pair);
            ++produced;
        }
        benchmark::DoNotOptimize(produced);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                            static_cast<int64_t>(totalPairs));
}
BENCHMARK_REGISTER_F(SorterBenchmark, BM_MergeIterator)
    ->Args({4, 1 << 14})
    ->Args({16, 1 << 14})
    ->Args({64, 1 << 12})
    ->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace mongo::sorter
