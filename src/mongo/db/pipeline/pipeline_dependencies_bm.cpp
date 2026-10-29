// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_optimization_bm_fixture.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

class PipelineDependenciesBMFixture : public PipelineOptimizationBMFixture {};

// Report cost of Pipeline::getDependencies().
BENCHMARK_DEFINE_F(PipelineDependenciesBMFixture, BM_PipelineGetDependencies)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(pipeline->getDependencies({}));
    }
}
BENCHMARK_REGISTER_F(PipelineDependenciesBMFixture, BM_PipelineGetDependencies)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// Report the cost to of calling getModifiedPaths() for each stage.
BENCHMARK_DEFINE_F(PipelineDependenciesBMFixture, BM_DocumentSourceGetModifiedPaths)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        for (auto&& stage : pipeline->getSources()) {
            benchmark::DoNotOptimize(stage->getModifiedPaths());
        }
    }
}
BENCHMARK_REGISTER_F(PipelineDependenciesBMFixture, BM_DocumentSourceGetModifiedPaths)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// Report the cost to of calling getDependencies() for each stage.
BENCHMARK_DEFINE_F(PipelineDependenciesBMFixture, BM_DocumentSourceGetDependencies)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        for (auto&& stage : pipeline->getSources()) {
            DepsTracker deps;
            benchmark::DoNotOptimize(stage->getDependencies(&deps));
        }
    }
}
BENCHMARK_REGISTER_F(PipelineDependenciesBMFixture, BM_DocumentSourceGetDependencies)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace mongo
