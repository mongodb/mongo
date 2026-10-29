// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/pipeline_optimization_bm_fixture.h"

#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

BENCHMARK_DEFINE_F(PipelineOptimizationBMFixture, BM_OptimizePipeline)
(benchmark::State& state) {
    auto rawPipeline = generateRawPipeline(state.range(0));
    benchmarkOptimizePipeline(state, rawPipeline, expCtx);
}
BENCHMARK_REGISTER_F(PipelineOptimizationBMFixture, BM_OptimizePipeline)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// Report pipeline parsing time.
BENCHMARK_DEFINE_F(PipelineOptimizationBMFixture, BM_ParsePipeline)
(benchmark::State& state) {
    auto rawPipeline = generateRawPipeline(state.range(0));
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(
            pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal));
    }
}
BENCHMARK_REGISTER_F(PipelineOptimizationBMFixture, BM_ParsePipeline)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace mongo
