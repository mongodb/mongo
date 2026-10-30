// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/dependency_analysis/pipeline_dependency_graph.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_optimization_bm_fixture.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/str.h"

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
        benchmark::DoNotOptimize(
            DependencyGraph(pipeline->getSources(), pipeline->getContext()->getNamespaceString()));
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

    DependencyGraph graph(pipeline->getSources(), pipeline->getContext()->getNamespaceString());
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

    DependencyGraph graph(pipeline->getSources(), pipeline->getContext()->getNamespaceString());
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

    DependencyGraph graph(pipeline->getSources(), pipeline->getContext()->getNamespaceString());
    for (auto keepRunning : state) {
        graph.recompute_forTest(middleIt);
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphBMFixture, BM_RebuildDependencyGraphFieldRefs)
    ->Arg(16)
    ->Arg(48)
    ->Arg(96)
    ->Unit(benchmark::kMicrosecond);

/// Benchmarks the cost of type inference used for arrayness checks.
class PipelineDependencyGraphTypeInferenceBMFixture : public PipelineOptimizationBMFixture {
public:
    void SetUp(benchmark::State& state) override {
        PipelineOptimizationBMFixture::SetUp(state);
        _originalTypeInference =
            feature_flags::gFeatureFlagQueryTypeInference.getForServerParameter();
        feature_flags::gFeatureFlagQueryTypeInference.setForServerParameter(state.range(1));
    }

    void TearDown(benchmark::State& state) override {
        feature_flags::gFeatureFlagQueryTypeInference.setForServerParameter(_originalTypeInference);
        PipelineOptimizationBMFixture::TearDown(state);
    }

    /// A $match which proves the base field 'x' is not an array, followed by renames of 'x'.
    std::unique_ptr<Pipeline> makeRenamePipeline(size_t numStages) {
        std::vector<BSONObj> rawPipeline{fromjson("{$match: {x: {$not: {$type: 'array'}}}}")};
        for (size_t i = 1; i < numStages; ++i) {
            rawPipeline.push_back(BSON("$set" << BSON(fmt::format("y{}", i) << "$x")));
        }
        return pipeline_factory::makePipeline(
            rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    }

private:
    bool _originalTypeInference{};
};

// Report the cost of the public canPathBeArray() API when it runs type inference.
BENCHMARK_DEFINE_F(PipelineDependencyGraphTypeInferenceBMFixture, BM_CanPathBeArray)
(benchmark::State& state) {
    auto pipeline = makeRenamePipeline(state.range(0));
    DependencyGraph graph(pipeline->getSources(), pipeline->getContext()->getNamespaceString());
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(graph.canPathBeArray(nullptr, "x"));
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphTypeInferenceBMFixture, BM_CanPathBeArray)
    ->ArgNames({"stages", "typeInference"})
    ->ArgsProduct({{10, 100, 1000} /*stages*/, {0, 1} /*typeInference*/})
    ->Unit(benchmark::kMicrosecond);

// Report the cost of building the dependency graph when renames of a base field with unknown
// arrayness run type inference.
BENCHMARK_DEFINE_F(PipelineDependencyGraphTypeInferenceBMFixture, BM_BuildDependencyGraphRenames)
(benchmark::State& state) {
    auto pipeline = makeRenamePipeline(state.range(0));
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(
            DependencyGraph(pipeline->getSources(), pipeline->getContext()->getNamespaceString()));
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphTypeInferenceBMFixture, BM_BuildDependencyGraphRenames)
    ->ArgNames({"stages", "typeInference"})
    ->ArgsProduct({{10, 100, 1000} /*stages*/, {0, 1} /*typeInference*/})
    ->Unit(benchmark::kMicrosecond);

/// Returns a dotted path of 'depth' components, e.g. 'x.p1.p2' for depth 3.
std::string makeDottedPath(size_t depth) {
    str::stream path;
    path << "x";
    for (size_t i = 1; i < depth; ++i) {
        path << ".p" << i;
    }
    return path;
}

// Report the cost of canPathBeArray() for a pipeline with a single $match that proves that no
// component in a long dotted path can contain arrays. For example:
// {$match: {
//   x: {$not: {$type: 'array'}},
//   'x.p1': {$not: {$type: 'array'}},
//   'x.p1.2': {$not: {$type: 'array'}},
//   ...,
//   'x.p1.p2...pN': {$not: {$type: 'array'}}
// }}
BENCHMARK_DEFINE_F(PipelineDependencyGraphTypeInferenceBMFixture, BM_CanPathBeArrayDottedPath)
(benchmark::State& state) {
    const auto path = makeDottedPath(state.range(0));
    BSONObjBuilder predicates;
    for (size_t depth = 1; depth <= static_cast<size_t>(state.range(0)); ++depth) {
        predicates.append(makeDottedPath(depth), fromjson("{$not: {$type: 'array'}}"));
    }
    auto pipeline = pipeline_factory::makePipeline(
        {BSON("$match" << predicates.obj())}, expCtx, pipeline_factory::kOptionsMinimal);
    DependencyGraph graph(pipeline->getSources(), pipeline->getContext()->getNamespaceString());
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(graph.canPathBeArray(nullptr, path));
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphTypeInferenceBMFixture, BM_CanPathBeArrayDottedPath)
    ->ArgNames({"depth", "typeInference"})
    ->ArgsProduct({{1, 4, 16} /*depth*/, {0, 1} /*typeInference*/})
    ->Unit(benchmark::kMicrosecond);

// Report the cost of canPathBeArray() for a pipeline with a single $match that proves that 'x'
// cannot be an array and also constrains the types of many unrelated fields. For example:
// {$match: {
//   x: {$not: {$type: 'array'}},
//   u0: {$type: 'number'},
//   u1: {$type: 'number'},
//   ...,
//   uN: {$type: 'number'}
// }}
BENCHMARK_DEFINE_F(PipelineDependencyGraphTypeInferenceBMFixture,
                   BM_CanPathBeArrayUnrelatedPredicates)
(benchmark::State& state) {
    BSONObjBuilder predicates;
    predicates.append("x", fromjson("{$not: {$type: 'array'}}"));
    for (int64_t i = 0; i < state.range(0); ++i) {
        predicates.append(fmt::format("u{}", i), fromjson("{$type: 'number'}"));
    }
    auto pipeline = pipeline_factory::makePipeline(
        {BSON("$match" << predicates.obj())}, expCtx, pipeline_factory::kOptionsMinimal);
    DependencyGraph graph(pipeline->getSources(), pipeline->getContext()->getNamespaceString());
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(graph.canPathBeArray(nullptr, "x"));
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphTypeInferenceBMFixture,
                     BM_CanPathBeArrayUnrelatedPredicates)
    ->ArgNames({"predicates", "typeInference"})
    ->ArgsProduct({{0, 10, 100, 1000} /*predicates*/, {0, 1} /*typeInference*/})
    ->Unit(benchmark::kMicrosecond);

// Report the cost of canPathBeArray() for a pipeline with a single $match that proves that 'x'
// cannot be an array and also constrains the types of many unrelated subpaths of 'x'. For example:
// {$match: {
//   x: {$not: {$type: 'array'}},
//   'x.u0': {$type: 'number'},
//   'x.u1': {$type: 'number'},
//   ...,
//   'x.uN': {$type: 'number'}
// }}
BENCHMARK_DEFINE_F(PipelineDependencyGraphTypeInferenceBMFixture,
                   BM_CanPathBeArrayUnrelatedSubpathPredicates)
(benchmark::State& state) {
    BSONObjBuilder predicates;
    predicates.append("x", fromjson("{$not: {$type: 'array'}}"));
    for (int64_t i = 0; i < state.range(0); ++i) {
        predicates.append(fmt::format("x.u{}", i), fromjson("{$type: 'number'}"));
    }
    auto pipeline = pipeline_factory::makePipeline(
        {BSON("$match" << predicates.obj())}, expCtx, pipeline_factory::kOptionsMinimal);
    DependencyGraph graph(pipeline->getSources(), pipeline->getContext()->getNamespaceString());
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(graph.canPathBeArray(nullptr, "x"));
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphTypeInferenceBMFixture,
                     BM_CanPathBeArrayUnrelatedSubpathPredicates)
    ->ArgNames({"predicates", "typeInference"})
    ->ArgsProduct({{0, 10, 100, 1000} /*predicates*/, {0, 1} /*typeInference*/})
    ->Unit(benchmark::kMicrosecond);

// Report the cost of canPathBeArray() for a pipeline with a $match that proves that 'x' cannot be
// an array, followed by many $match stages that constrain unrelated fields. For example:
// [
//   {$match: {x: {$not: {$type: 'array'}}}},
//   {$match: {u0: {$type: 'number'}}},
//   {$match: {u1: {$type: 'number'}}},
//   ...,
//   {$match: {uN: {$type: 'number'}}}
// ]
BENCHMARK_DEFINE_F(PipelineDependencyGraphTypeInferenceBMFixture, BM_CanPathBeArrayUnrelatedMatches)
(benchmark::State& state) {
    std::vector<BSONObj> rawPipeline{fromjson("{$match: {x: {$not: {$type: 'array'}}}}")};
    for (int64_t i = 0; i < state.range(0); ++i) {
        rawPipeline.push_back(
            BSON("$match" << BSON(fmt::format("u{}", i) << BSON("$type" << "number"))));
    }
    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    DependencyGraph graph(pipeline->getSources(), pipeline->getContext()->getNamespaceString());
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(graph.canPathBeArray(nullptr, "x"));
    }
}
BENCHMARK_REGISTER_F(PipelineDependencyGraphTypeInferenceBMFixture,
                     BM_CanPathBeArrayUnrelatedMatches)
    ->ArgNames({"matches", "typeInference"})
    ->ArgsProduct({{0, 10, 100, 999} /*matches*/, {0, 1} /*typeInference*/})
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace mongo
