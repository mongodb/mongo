// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/plan_cache/plan_cache_bm_fixture.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>

#include <benchmark/benchmark.h>

namespace mongo::optimizer {
namespace {
/**
 * Benchmarks parsing of BSON to CQ and encoding of Pipeline to SBE PlanCacheKey.
 */
class PipelineParseAndEncodeSBE : public PlanCacheBenchmarkFixture {
public:
    PipelineParseAndEncodeSBE() {}

    static CanonicalQuery::QueryShapeString parseAndEncode(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::vector<BSONObj>& pipeline) {
        std::unique_ptr<Pipeline> parsedPipeline =
            pipeline_factory::makePipeline(pipeline, expCtx, pipeline_factory::kOptionsMinimal);
        pipeline_optimization::optimizePipeline(*parsedPipeline);
        parsedPipeline->parameterize();

        std::vector<boost::intrusive_ptr<DocumentSource>> pipelineStages;
        for (auto&& source : parsedPipeline->getSources()) {
            pipelineStages.emplace_back(source);
        }

        return canonical_query_encoder::encodePipeline(expCtx.get(), pipelineStages);
    }

    void benchmarkQueryMatchProject(benchmark::State& state,
                                    BSONObj matchSpec,
                                    BSONObj projectSpec) final {
        std::vector<BSONObj> pipeline;
        if (!matchSpec.isEmpty()) {
            pipeline.push_back(BSON("$match" << matchSpec));
        }
        if (!projectSpec.isEmpty()) {
            pipeline.push_back(BSON("$project" << projectSpec));
        }
        benchmarkPipeline(state, pipeline);
    }

    void benchmarkPipeline(benchmark::State& state, const std::vector<BSONObj>& pipeline) final {
        QueryTestServiceContext testServiceContext;
        auto opCtx = testServiceContext.makeOperationContext();
        auto expCtx = make_intrusive<ExpressionContextForTest>(
            opCtx.get(), NamespaceString::createNamespaceString_forTest("test.bm"));

        // This is where recording starts.
        for (auto keepRunning : state) {
            benchmark::DoNotOptimize(parseAndEncode(expCtx, pipeline));
            benchmark::ClobberMemory();
        }
    }
};

BENCHMARK_QUERY_ENCODING(PipelineParseAndEncodeSBE);
BENCHMARK_PIPELINE_QUERY_ENCODING(PipelineParseAndEncodeSBE);
}  // namespace
}  // namespace mongo::optimizer
