// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/util/intrusive_counter.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

namespace mongo {

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

inline void benchmarkOptimizePipeline(
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

}  // namespace mongo
