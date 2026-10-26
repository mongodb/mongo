// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <random>
#include <utility>

#include <benchmark/benchmark.h>

namespace mongo::sbe {

namespace {
class UniqueRoaringDummyTest : public PlanStageTestFixture {
public:
    void TestBody() override {}
};

value::TagValueOwned makeInputWithDuplicates(int64_t size, int64_t duplicatePercent) {
    std::mt19937 random{1};
    std::uniform_int_distribution<int64_t> percent{0, 99};
    auto [arrTag, arrVal] = value::makeNewArray();
    value::TagValueOwned owned = value::TagValueOwned::fromRaw(arrTag, arrVal);
    auto* arr = value::getArrayView(arrVal);
    arr->reserve(size);
    int64_t nextNew = 0;
    for (int64_t i = 0; i < size; ++i) {
        int64_t id;
        if (nextNew > 0 && percent(random) < duplicatePercent) {
            std::uniform_int_distribution<int64_t> pick{0, nextNew - 1};
            id = pick(random);
        } else {
            id = nextNew++;
        }
        arr->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(id));
    }
    return owned;
}

class UniqueRoaringBenchmarkFixture : public benchmark::Fixture {
public:
    void run(benchmark::State& state, bool withUniqueStage) {
        const int64_t inputSize = state.range(0);
        const int64_t duplicatePercent = state.range(1);

        UniqueRoaringDummyTest dummyTest;
        dummyTest.setUp();

        value::TagValueOwned input = makeInputWithDuplicates(inputSize, duplicatePercent);

        for (auto _ : state) {
            state.PauseTiming();
            auto compileCtx = dummyTest.makeCompileCtx();
            auto [scanSlot, scanStage] = dummyTest.generateVirtualScan(
                value::TagValueMaybeOwned::fromRaw(false, input.tag(), input.value()),
                kEmptyPlanNodeId);
            std::unique_ptr<PlanStage> root = std::move(scanStage);
            if (withUniqueStage) {
                root = makeS<UniqueRoaringStage>(std::move(root), scanSlot, kEmptyPlanNodeId);
            }
            auto accessor = dummyTest.prepareTree(compileCtx.get(), root.get(), scanSlot);
            state.ResumeTiming();

            dummyTest.exhaustStage(root.get(), accessor);

            state.PauseTiming();
            root.reset();
            state.ResumeTiming();
        }

        state.SetItemsProcessed(state.iterations() * inputSize);

        dummyTest.tearDown();
    }
};

void allCombinations(benchmark::internal::Benchmark* b) {
    for (int64_t size : {100'000, 1'000'000, 10'000'000, 20'000'000}) {
        for (int64_t duplicatePercent : {0, 50, 90}) {
            b->Args({size, duplicatePercent});
        }
    }
}

BENCHMARK_DEFINE_F(UniqueRoaringBenchmarkFixture, UniqueRoaring)(benchmark::State& state) {
    run(state, /*withUniqueStage=*/true);
}

BENCHMARK_DEFINE_F(UniqueRoaringBenchmarkFixture, ScanOnlyBaseline)(benchmark::State& state) {
    run(state, /*withUniqueStage=*/false);
}

BENCHMARK_REGISTER_F(UniqueRoaringBenchmarkFixture, UniqueRoaring)
    ->Apply(allCombinations)
    ->Iterations(3)
    ->Repetitions(3)
    ->ReportAggregatesOnly()
    ->Unit(benchmark::kMillisecond);
BENCHMARK_REGISTER_F(UniqueRoaringBenchmarkFixture, ScanOnlyBaseline)
    ->Apply(allCombinations)
    ->Iterations(3)
    ->Repetitions(3)
    ->ReportAggregatesOnly()
    ->Unit(benchmark::kMillisecond);
}  // namespace

}  // namespace mongo::sbe
