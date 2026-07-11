// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_bm_constants.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/duration.h"

#include <climits>
#include <memory>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

/**
 * Builds a sort specification with 'count' fields, like {field_0: 1, field_1: -1, field_2: 1, ...}.
 */
BSONObj buildSortSpec(size_t count) {
    BSONObjBuilder builder;
    for (size_t i = 0; i < count; ++i) {
        builder.append((str::stream() << "field_" << i), (i % 2 == 0) ? 1 : -1);
    }
    return builder.obj();
}

auto makeFindKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                 const ParsedFindCommand& parsedFind) {
    return std::make_unique<const query_stats::FindKey>(
        expCtx,
        *parsedFind.findCommandRequest,
        std::make_unique<query_shape::FindCmdShape>(parsedFind, expCtx),
        query_benchmark_constants::kCollectionType);
}

auto shapifyAndHashRequest(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           const ParsedFindCommand& parsedFind) {
    auto key = makeFindKey(expCtx, parsedFind);
    return absl::HashOf(key);
}

void runBenchmark(BSONObj predicate,
                  BSONObj projection,
                  BSONObj sort,
                  boost::optional<int64_t> limit,
                  boost::optional<int64_t> skip,
                  benchmark::State& state) {
    QueryFCVEnvironmentForTest::setUp();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("query_test");

    auto opCtx = client->makeOperationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get());
    auto fcr = std::make_unique<FindCommandRequest>(expCtx->getNamespaceString());
    fcr->setFilter(predicate);
    if (!projection.isEmpty()) {
        fcr->setProjection(projection);
    }
    if (!sort.isEmpty()) {
        fcr->setSort(sort);
    }
    if (limit) {
        fcr->setLimit(limit);
    }
    if (skip) {
        fcr->setSkip(skip);
    }

    ClientMetadata::setFromMetadata(
        opCtx->getClient(), query_benchmark_constants::kMockClientMetadataElem, false);
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcr)}));

    // Run the benchmark.
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(shapifyAndHashRequest(expCtx, *parsedFind));
    }
}

void runBenchmark(BSONObj predicate, benchmark::State& state) {
    runBenchmark(predicate, BSONObj(), BSONObj(), boost::none, boost::none, state);
}

// Benchmark the performance of computing and hashing the query stats key for an IDHACK query.
void BM_ShapfiyIDHack(benchmark::State& state) {
    runBenchmark(query_benchmark_constants::kIDHackPredicate, state);
}

// Benchmark computing the query stats key and its hash for a mildly complex query predicate.
void BM_ShapfiyMildlyComplex(benchmark::State& state) {
    runBenchmark(query_benchmark_constants::kMildlyComplexPredicate, state);
}

// Benchmark computing the query stats key and its hash for a complex query predicate.
void BM_ShapifyComplex(benchmark::State& state) {
    runBenchmark(query_benchmark_constants::kComplexPredicate,
                 query_benchmark_constants::kComplexProjection,
                 buildSortSpec(5),
                 5,
                 10,
                 state);
}

// Benchmark computing the query stats key and its hash for a very complex query predicate
// (inspired by change stream predicate).
void BM_ShapifyVeryComplex(benchmark::State& state) {
    runBenchmark(query_benchmark_constants::kChangeStreamPredicate,
                 query_benchmark_constants::kVeryComplexProjection,
                 buildSortSpec(10),
                 5,
                 10,
                 state);
}

BENCHMARK(BM_ShapfiyIDHack)->Threads(1);
BENCHMARK(BM_ShapfiyMildlyComplex)->Threads(1);
BENCHMARK(BM_ShapifyComplex)->Threads(1);
BENCHMARK(BM_ShapifyVeryComplex)->Threads(1);

}  // namespace
}  // namespace mongo
