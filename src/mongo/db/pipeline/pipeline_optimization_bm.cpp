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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/server_parameter.h"
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

static void benchmarkOptimizePipeline(
    benchmark::State& state,
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContextForTest>& expCtx) {
    for (auto keepRunning : state) {
        state.PauseTiming();
        auto pipeline =
            pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
        state.ResumeTiming();
        pipeline_optimization::optimizePipeline(*pipeline);
    }
}

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
        graph.recompute(middleIt);
    }
}
BENCHMARK_REGISTER_F(PipelineOptimizationBMFixture, BM_RebuildDependencyGraphFromMiddle)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

/// Generates field names a, b, ... aa, ab and so on.
static std::string generateFieldName(size_t index) {
    constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyz";
    std::string name;
    for (size_t n = index + 1; n > 0; n = (n - 1) / 26) {
        name += kAlphabet[(n - 1) % 26];
    }
    return {name.rbegin(), name.rend()};
}

template <typename MakeField>
static BSONObj makeAddFields(size_t numFields, MakeField makeField) {
    BSONObjBuilder bob;
    BSONObjBuilder spec(bob.subobjStart("$addFields"));
    for (size_t i = 0; i < numFields; ++i) {
        makeField(spec, i);
    }
    spec.done();
    return bob.obj();
}

/// All fields are independent constants: {$addFields: {a: 1, b: 1, ...}}.
static BSONObj makeConstantsAddFields(size_t numFields) {
    return makeAddFields(
        numFields, [](BSONObjBuilder& spec, size_t i) { spec.append(generateFieldName(i), 1); });
}

/// Each field reads the next, forming a dependency chain: {$addFields: {a: "$b", b: "$c", ...}}.
static BSONObj makeChainAddFields(size_t numFields) {
    return makeAddFields(numFields, [](BSONObjBuilder& spec, size_t i) {
        spec.append(generateFieldName(i), "$" + generateFieldName(i + 1));
    });
}

static std::vector<BSONObj> makeHoistBenchmarkPipeline(const BSONObj& projection) {
    // The $sort creates a dependency, so that we cannot hoist {$set: {a: ...}}.
    return {fromjson("{$sort: {a: 1}}"), projection};
}

/// Returns stage names of the optimized pipeline.
static std::vector<StringData> stageNamesAfterOptimization(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContextForTest>& expCtx) {
    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

    std::vector<StringData> names;
    for (auto& source : pipeline->getSources()) {
        names.push_back(source->getSourceName());
    }
    return names;
}

template <typename MakeProjection>
static void runHoistBenchmark(benchmark::State& state,
                              boost::intrusive_ptr<ExpressionContextForTest>& expCtx,
                              std::vector<StringData> expectedStageNames,
                              MakeProjection makeProjection) {
    // Always perform the rewrite for any number of paths.
    feature_flags::gFeatureFlagImprovedDepsAnalysis.setForServerParameter(true);
    auto& param = *ServerParameterSet::getNodeParameterSet()->get<TransformHoistPolicy>(
        "internalQueryTransformHoistPolicy");
    param._data = TransformHoistPolicyEnum::kAlways;
    internalQueryTransformHoistMaximumPaths.store(std::numeric_limits<int>::max());

    auto rawPipeline = makeHoistBenchmarkPipeline(makeProjection(state.range(0)));
    auto actualStageNames = stageNamesAfterOptimization(rawPipeline, expCtx);
    invariant(actualStageNames == expectedStageNames);
    benchmarkOptimizePipeline(state, rawPipeline, expCtx);
}

static void hoistBenchmarkArgs(benchmark::internal::Benchmark* b) {
    b->RangeMultiplier(4)->Range(4, 16384)->Unit(benchmark::kMicrosecond);
}

// All fields are constants, therefore we can hoist them all except the sort key.
BENCHMARK_DEFINE_F(PipelineOptimizationBMFixture, BM_HoistComputationConstants)
(benchmark::State& state) {
    runHoistBenchmark(state, expCtx, {"$addFields", "$sort", "$addFields"}, makeConstantsAddFields);
}
BENCHMARK_REGISTER_F(PipelineOptimizationBMFixture, BM_HoistComputationConstants)
    ->Apply(hoistBenchmarkArgs);


// Fields depend on each other, which creates a chain preventing any rewrite.
BENCHMARK_DEFINE_F(PipelineOptimizationBMFixture, BM_HoistComputationChain)
(benchmark::State& state) {
    runHoistBenchmark(state, expCtx, {"$sort", "$addFields"}, makeChainAddFields);
}
BENCHMARK_REGISTER_F(PipelineOptimizationBMFixture, BM_HoistComputationChain)
    ->Apply(hoistBenchmarkArgs);

}  // namespace
}  // namespace mongo
