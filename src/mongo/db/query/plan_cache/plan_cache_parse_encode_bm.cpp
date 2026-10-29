// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_cache/plan_cache_bm_fixture.h"
#include "mongo/db/query/query_test_service_context.h"

#include <memory>

#include <benchmark/benchmark.h>

namespace mongo::optimizer {
namespace {
/**
 * Benchmarks parsing of BSON to CQ and encoding of CanonicalQuery to SBE PlanCacheKey.
 */
class CanonicalQueryParseAndEncodeSBE : public PlanCacheBenchmarkFixture {
public:
    CanonicalQueryParseAndEncodeSBE() {}

    static CanonicalQuery::QueryShapeString parseAndEncode(OperationContext* opCtx,
                                                           NamespaceString nss,
                                                           BSONObj matchSpec,
                                                           BSONObj projectSpec) {
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(matchSpec);
        findCommand->setProjection(projectSpec);
        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
        cq->setSbeCompatible(true);

        return canonical_query_encoder::encodeSBE(*cq);
    }

    void benchmarkPipeline(benchmark::State& state, const std::vector<BSONObj>& pipeline) final {
        state.SkipWithError("CanonicalQuery encoding fixture cannot encode a pipeline.");
        return;
    }

    void benchmarkQueryMatchProject(benchmark::State& state,
                                    BSONObj matchSpec,
                                    BSONObj projectSpec) final {
        QueryTestServiceContext testServiceContext;
        auto opCtx = testServiceContext.makeOperationContext();
        auto nss = NamespaceString::createNamespaceString_forTest("test.bm");

        // This is where recording starts.
        for (auto keepRunning : state) {
            benchmark::DoNotOptimize(parseAndEncode(opCtx.get(), nss, matchSpec, projectSpec));
            benchmark::ClobberMemory();
        }
    }
};

BENCHMARK_QUERY_ENCODING(CanonicalQueryParseAndEncodeSBE);
}  // namespace
}  // namespace mongo::optimizer
