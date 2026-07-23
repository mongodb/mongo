// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_optimization_bm_fixture.h"

#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

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

/// Generates field names a, b, ... aa, ab and so on.
std::string generateFieldName(size_t index) {
    constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyz";
    std::string name;
    for (size_t n = index + 1; n > 0; n = (n - 1) / 26) {
        name += kAlphabet[(n - 1) % 26];
    }
    return {name.rbegin(), name.rend()};
}

template <typename MakeField>
BSONObj makeAddFields(size_t numFields, MakeField makeField) {
    BSONObjBuilder bob;
    BSONObjBuilder spec(bob.subobjStart("$addFields"));
    for (size_t i = 0; i < numFields; ++i) {
        makeField(spec, i);
    }
    spec.done();
    return bob.obj();
}

// Report the cost of re-building the dependency graph when every stage declares many dotted
// paths under the same base fields. The first argument is the total number of declared paths
// across all stages, distributed evenly between them.
BENCHMARK_DEFINE_F(PipelineDependencyGraphBMFixture, BM_RebuildDependencyGraphWideStages)
(benchmark::State& state) {
    constexpr size_t kNumStages = 4;
    const size_t fieldsPerStage = state.range(0) / kNumStages;
    std::vector<BSONObj> rawPipeline;
    for (size_t stage = 0; stage < kNumStages; ++stage) {
        rawPipeline.push_back(makeAddFields(fieldsPerStage, [&](BSONObjBuilder& spec, size_t i) {
            spec.append(fmt::format("{}.s{}", generateFieldName(i), stage), 1);
        }));
    }
    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    auto middleIt = std::next(pipeline->getSources().cbegin(), kNumStages / 2);

    DependencyGraph graph(pipeline->getSources());
    for (auto keepRunning : state) {
        graph.recompute_forTest(middleIt);
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphBMFixture, BM_RebuildDependencyGraphWideStages)
    ->Arg(16)
    ->Arg(48)
    ->Arg(96)
    ->Unit(benchmark::kMicrosecond);

// Report the cost of re-building the dependency graph when every declared field references
// another field, so per-field and per-stage dependency sets are populated on each rebuild.
// The first argument is the total number of declared paths.
BENCHMARK_DEFINE_F(PipelineDependencyGraphBMFixture, BM_RebuildDependencyGraphFieldRefs)
(benchmark::State& state) {
    constexpr size_t kNumStages = 4;
    const size_t fieldsPerStage = state.range(0) / kNumStages;
    // Declare 'src' with an expression that is not folded to a constant, so references to it
    // stay in the graph.
    std::vector<BSONObj> rawPipeline{fromjson("{$addFields: {src: {$add: [1, 2]}}}")};
    const BSONObj addExpr = BSON("$add" << BSON_ARRAY("$src" << 1));
    for (size_t stage = 0; stage < kNumStages; ++stage) {
        rawPipeline.push_back(makeAddFields(fieldsPerStage, [&](BSONObjBuilder& spec, size_t i) {
            // Alternate between renames and computed expressions referencing 'src'.
            if (i % 2 == 0) {
                spec.append(generateFieldName(i), "$src");
            } else {
                spec.append(generateFieldName(i), addExpr);
            }
        }));
    }
    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    auto middleIt = std::next(pipeline->getSources().cbegin(), 1 + kNumStages / 2);

    DependencyGraph graph(pipeline->getSources());
    for (auto keepRunning : state) {
        graph.recompute_forTest(middleIt);
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphBMFixture, BM_RebuildDependencyGraphFieldRefs)
    ->Arg(16)
    ->Arg(48)
    ->Arg(96)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace mongo
