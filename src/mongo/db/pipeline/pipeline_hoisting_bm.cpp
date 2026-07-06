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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/pipeline_optimization_bm_fixture.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/server_parameter.h"

#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

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
static std::vector<std::string> stageNamesAfterOptimization(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContextForTest>& expCtx) {
    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    pipeline_optimization::optimizePipeline(*pipeline);

    std::vector<std::string> names;
    for (auto& source : pipeline->getSources()) {
        names.emplace_back(source->getSourceName());
    }
    return names;
}

// Enables the hoisting rewrite for any number of paths. Restores the modified knobs on teardown so
// they do not leak into the other benchmarks, whose run order relative to these is not guaranteed.
class PipelineHoistingBMFixture : public PipelineOptimizationBMFixture {
public:
    void SetUp(benchmark::State& state) override {
        PipelineOptimizationBMFixture::SetUp(state);

        auto& policy = hoistPolicyParam();
        _savedHoistPolicy = policy._data.get();
        _savedMaximumPaths = internalQueryTransformHoistMaximumPaths.load();

        // Always perform the rewrite for any number of paths.
        feature_flags::gFeatureFlagImprovedDepsAnalysis.setForServerParameter(true);
        policy._data = TransformHoistPolicyEnum::kAlways;
        internalQueryTransformHoistMaximumPaths.store(std::numeric_limits<int>::max());
    }

    void TearDown(benchmark::State& state) override {
        hoistPolicyParam()._data = _savedHoistPolicy;
        internalQueryTransformHoistMaximumPaths.store(_savedMaximumPaths);
        PipelineOptimizationBMFixture::TearDown(state);
    }

private:
    static TransformHoistPolicy& hoistPolicyParam() {
        return *ServerParameterSet::getNodeParameterSet()->get<TransformHoistPolicy>(
            "internalQueryTransformHoistPolicy");
    }

    TransformHoistPolicyEnum _savedHoistPolicy{};
    int _savedMaximumPaths{};
};

template <typename MakeProjection>
static void runHoistBenchmark(benchmark::State& state,
                              boost::intrusive_ptr<ExpressionContextForTest>& expCtx,
                              std::vector<std::string> expectedStageNames,
                              MakeProjection makeProjection) {
    auto rawPipeline = makeHoistBenchmarkPipeline(makeProjection(state.range(0)));
    auto actualStageNames = stageNamesAfterOptimization(rawPipeline, expCtx);
    invariant(actualStageNames == expectedStageNames);
    benchmarkOptimizePipeline(state, rawPipeline, expCtx);
}

static void hoistBenchmarkArgs(benchmark::internal::Benchmark* b) {
    b->RangeMultiplier(4)->Range(4, 16384)->Unit(benchmark::kMicrosecond);
}

// All fields are constants, therefore we can hoist them all except the sort key.
BENCHMARK_DEFINE_F(PipelineHoistingBMFixture, BM_HoistComputationConstants)
(benchmark::State& state) {
    runHoistBenchmark(state, expCtx, {"$addFields", "$sort", "$addFields"}, makeConstantsAddFields);
}
BENCHMARK_REGISTER_F(PipelineHoistingBMFixture, BM_HoistComputationConstants)
    ->Apply(hoistBenchmarkArgs);


// Fields depend on each other, which creates a chain preventing any rewrite.
BENCHMARK_DEFINE_F(PipelineHoistingBMFixture, BM_HoistComputationChain)
(benchmark::State& state) {
    runHoistBenchmark(state, expCtx, {"$sort", "$addFields"}, makeChainAddFields);
}
BENCHMARK_REGISTER_F(PipelineHoistingBMFixture, BM_HoistComputationChain)
    ->Apply(hoistBenchmarkArgs);

}  // namespace
}  // namespace mongo
