// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_optimization_bm_fixture.h"

#include <cstddef>
#include <iterator>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

using DependencyGraph = pipeline::dependency_graph::DependencyGraph;

class PipelineDependencyGraphBMFixture : public PipelineOptimizationBMFixture {};

// Report the cost of building the dependency graph from scratch.
BENCHMARK_DEFINE_F(PipelineDependencyGraphBMFixture, BM_BuildDependencyGraph)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(DependencyGraph(pipeline->getSources()));
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphBMFixture, BM_BuildDependencyGraph)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// Report the cost of re-building the dependency graph from the middle of the pipeline.
BENCHMARK_DEFINE_F(PipelineDependencyGraphBMFixture, BM_RebuildDependencyGraphFromMiddle)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    size_t middle = state.range(0) / 2;
    auto middleIt = std::next(pipeline->getSources().cbegin(), middle);

    DependencyGraph graph(pipeline->getSources());
    for (auto keepRunning : state) {
        graph.recompute_forTest(middleIt);
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphBMFixture, BM_RebuildDependencyGraphFromMiddle)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace mongo
