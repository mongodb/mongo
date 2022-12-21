/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <benchmark/benchmark.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/abt/abt_translate_bm_fixture.h"
#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/query_test_service_context.h"

namespace mongo::optimizer {
namespace {
/**
 * Benchmarks translation from optimized Pipeline to ABT.
 */
class PipelineABTTranslateBenchmark : public ABTTranslateBenchmarkFixture {
public:
    PipelineABTTranslateBenchmark() {}

    void benchmarkABTTranslate(benchmark::State& state,
                               BSONObj matchSpec,
                               BSONObj projectSpec) override final {
        std::vector<BSONObj> pipeline;
        if (!matchSpec.isEmpty()) {
            pipeline.push_back(BSON("$match" << matchSpec));
        }
        if (!projectSpec.isEmpty()) {
            pipeline.push_back(BSON("$project" << projectSpec));
        }
        benchmarkABTTranslate(state, pipeline);
    }

    void benchmarkABTTranslate(benchmark::State& state,
                               const std::vector<BSONObj>& pipeline) override final {
        QueryTestServiceContext testServiceContext;
        auto opCtx = testServiceContext.makeOperationContext();
        auto expCtx =
            make_intrusive<ExpressionContextForTest>(opCtx.get(), NamespaceString("test.bm"));

        Metadata metadata{{}};
        auto prefixId = PrefixId::createForTests();
        ProjectionName scanProjName{prefixId.getNextId("scan")};

        std::unique_ptr<Pipeline, PipelineDeleter> parsedPipeline =
            Pipeline::parse(pipeline, expCtx);
        parsedPipeline->optimizePipeline();

        if (!isEligibleForBonsai_forTesting(testServiceContext.getServiceContext(),
                                            *parsedPipeline.get())) {
            state.SkipWithError("Pipeline is not supported by CQF");
            return;
        }

        // This is where recording starts.
        for (auto keepRunning : state) {
            benchmark::DoNotOptimize(
                translatePipelineToABT(metadata,
                                       *parsedPipeline,
                                       scanProjName,
                                       make<ScanNode>(scanProjName, "collection"),
                                       prefixId));
            benchmark::ClobberMemory();
        }
    }
};

BENCHMARK_MQL_TRANSLATION(PipelineABTTranslateBenchmark)
BENCHMARK_MQL_PIPELINE_TRANSLATION(PipelineABTTranslateBenchmark)
}  // namespace
}  // namespace mongo::optimizer
