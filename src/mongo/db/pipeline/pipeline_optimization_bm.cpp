/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/util/intrusive_counter.h"

#include <benchmark/benchmark.h>
#include <fmt/format.h>

namespace mongo {
namespace {

using DependencyGraph = pipeline::dependency_graph::DependencyGraph;

/**
 * In every benchmark defined using this fixture, the first argument denotes pipeline length.
 */
class PipelineOptimizationBMFixture : public benchmark::Fixture {
public:
    /**
     * Generates a pipeline by repeating the pattern defined in makeNextStages() until we reach the
     * desired number of stages. The goal is to stress the code that applies rewrites, rather than
     * to provide comprehensive coverage of many different rewrites. Hence we want a pipeline that
     * can have a large (relative to 'numStages') number of rewrites applied to it.
     */
    static std::vector<BSONObj> generateRawPipeline(size_t numStages) {
        size_t nextFieldSuffix{0};

        auto makeNextStages = [&]() -> std::vector<std::string> {
            size_t fieldSuffix = ++nextFieldSuffix;
            return {
                // Can only swap with '{$match: {f<n>: ...}}'
                R"({$sort: {_id: 1}})",
                R"({$addFields: {c1: {$toInt: "$c1"}}})",
                R"({$addFields: {c2: {$toInt: "$c2"}}})",
                R"({$addFields: {c3: {$toInt: "$c3"}, c4: {$toInt: "$c4"}, c5: {$toInt: "$c5"}}})",
                // The $match will be split and predicates prefixed with 'c' will be pushed right
                // after the corresponding $addFields. The predicate prefixed with 'f' can be pushed
                // to the front of the pipeline and merged with other such predicates.
                fmt::format(
                    fmt::runtime(R"({{$match: {{c1: 1, c2: 1, c3: 1, c4: 1, c5: 1, f{}: 1}}}})"),
                    fieldSuffix),
            };
        };

        std::vector<BSONObj> pipeline;
        while (true) {
            for (auto& stage : makeNextStages()) {
                pipeline.push_back(fromjson(stage));
                if (pipeline.size() >= numStages) {
                    return pipeline;
                }
            }
        }
    }

    std::unique_ptr<Pipeline> makePipeline(benchmark::State& state) {
        return pipeline_factory::makePipeline(
            generateRawPipeline(state.range(0)), expCtx, pipeline_factory::kOptionsMinimal);
    }

    void SetUp(benchmark::State& state) override {
        QueryFCVEnvironmentForTest::setUp();
        // Suppress logs.
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kDefault, mongo::logv2::LogSeverity::Error());
        // Keep the same default on debug builds.
        internalPipelineLengthLimit.store(1000);
        expCtx = make_intrusive<ExpressionContextForTest>(
            NamespaceString::createNamespaceString_forTest("test", "bm"));
    };

    void TearDown(benchmark::State& state) override {
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kDefault, mongo::logv2::LogSeverity::Log());
        internalPipelineLengthLimit.store(defaultInternalPipelineLengthLimit());
        expCtx = {};
    }

    boost::intrusive_ptr<ExpressionContextForTest> expCtx;
};

// Report optimization time.
BENCHMARK_DEFINE_F(PipelineOptimizationBMFixture, BM_OptimizePipeline)
(benchmark::State& state) {
    auto rawPipeline = generateRawPipeline(state.range(0));
    for (auto keepRunning : state) {
        state.PauseTiming();
        auto pipeline =
            pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
        state.ResumeTiming();

        pipeline_optimization::optimizePipeline(*pipeline);
    }
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

// Report cost of Pipeline::getDependencies().
BENCHMARK_DEFINE_F(PipelineOptimizationBMFixture, BM_PipelineGetDependencies)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(pipeline->getDependencies({}));
    }
}
BENCHMARK_REGISTER_F(PipelineOptimizationBMFixture, BM_PipelineGetDependencies)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// Report the cost to of calling getModifiedPaths() for each stage.
BENCHMARK_DEFINE_F(PipelineOptimizationBMFixture, BM_DocumentSourceGetModifiedPaths)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        for (auto&& stage : pipeline->getSources()) {
            benchmark::DoNotOptimize(stage->getModifiedPaths());
        }
    }
}
BENCHMARK_REGISTER_F(PipelineOptimizationBMFixture, BM_DocumentSourceGetModifiedPaths)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// Report the cost to of calling getDependencies() for each stage.
BENCHMARK_DEFINE_F(PipelineOptimizationBMFixture, BM_DocumentSourceGetDependencies)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        for (auto&& stage : pipeline->getSources()) {
            DepsTracker deps;
            benchmark::DoNotOptimize(stage->getDependencies(&deps));
        }
    }
}
BENCHMARK_REGISTER_F(PipelineOptimizationBMFixture, BM_DocumentSourceGetDependencies)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// Report the cost of building the dependency graph from scratch.
BENCHMARK_DEFINE_F(PipelineOptimizationBMFixture, BM_BuildDependencyGraph)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(DependencyGraph(pipeline->getSources()));
    }
}
BENCHMARK_REGISTER_F(PipelineOptimizationBMFixture, BM_BuildDependencyGraph)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// Report the cost of re-building the dependency graph from the middle of the pipeline.
BENCHMARK_DEFINE_F(PipelineOptimizationBMFixture, BM_RebuildDependencyGraphFromMiddle)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    size_t middle = state.range(0) / 2;
    auto middleIt = std::next(pipeline->getSources().cbegin(), middle);

    DependencyGraph graph(pipeline->getSources());
    for (auto keepRunning : state) {
        graph.recompute(pipeline->getSources(), middleIt);
    }
}
BENCHMARK_REGISTER_F(PipelineOptimizationBMFixture, BM_RebuildDependencyGraphFromMiddle)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace mongo
