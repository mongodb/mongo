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
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/util/intrusive_counter.h"

#include <benchmark/benchmark.h>
#include <fmt/format.h>

namespace mongo {
namespace {

class PipelineOptimizationBMFixture : public benchmark::Fixture {
public:
    void runPipelineOptimization(std::vector<BSONObj> rawPipeline, benchmark::State& state) {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "bm");
        auto expCtx = make_intrusive<ExpressionContextForTest>(nss);

        for (auto keepRunning : state) {
            state.PauseTiming();
            auto pipeline = Pipeline::parse(rawPipeline, expCtx);
            state.ResumeTiming();

            pipeline_optimization::optimizePipeline(*pipeline);
        }
    }

    void SetUp(benchmark::State& state) override {
        QueryFCVEnvironmentForTest::setUp();
        // Keep the same default on debug builds.
        internalPipelineLengthLimit.store(1000);
    };

    void TearDown(benchmark::State& state) override {
        internalPipelineLengthLimit.store(defaultInternalPipelineLengthLimit());
    }

    static std::vector<BSONObj> makePipeline(size_t numStages) {
        size_t nextFieldSuffix{0};

        // The pipeline repeats this pattern until we reach the desired number of stages. The goal
        // is to stress the code that applies rewrites, rather than to provide comprehensive
        // coverage of many different rewrites. Hence we want a pipeline that can have a large
        // (relative to 'numStages') number of rewrites applied to it.
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
};

BENCHMARK_F(PipelineOptimizationBMFixture, BM_OptimizePipeline10Stages)
(benchmark::State& state) {
    runPipelineOptimization(makePipeline(10), state);
}

BENCHMARK_F(PipelineOptimizationBMFixture, BM_OptimizePipeline100Stages)
(benchmark::State& state) {
    runPipelineOptimization(makePipeline(100), state);
}

BENCHMARK_F(PipelineOptimizationBMFixture, BM_OptimizePipeline1000Stages)
(benchmark::State& state) {
    runPipelineOptimization(makePipeline(1000), state);
}

}  // namespace
}  // namespace mongo
