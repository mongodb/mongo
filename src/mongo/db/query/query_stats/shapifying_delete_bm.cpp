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

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_bm_constants.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_shape/delete_cmd_shape.h"
#include "mongo/db/query/query_stats/write_key.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/rpc/metadata/client_metadata.h"

#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

// Two benchmark types: one measuring full key generation (used in $queryStats sampling),
// and one measuring only SHA-256 hashing of the query shape (used on every query on the hot path).
enum class ShapifyDeleteTestType : int { kGenerateDeleteKey = 0, kSHA256Hash };

auto shapifyAndGenerateKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           const ParsedDelete& parsedDelete,
                           const write_ops::DeleteCommandRequest& deleteCommandRequest) {
    query_stats::DeleteKey key(
        expCtx,
        deleteCommandRequest,
        boost::none /* hint */,
        std::make_unique<query_shape::DeleteCmdShape>(deleteCommandRequest, parsedDelete, expCtx),
        query_benchmark_constants::kCollectionType);

    return absl::HashOf(key);
}

auto shapifyAndSHA256Hash(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          const ParsedDelete& parsedDelete,
                          const write_ops::DeleteCommandRequest& deleteCommandRequest) {
    auto deleteQueryShape =
        std::make_unique<query_shape::DeleteCmdShape>(deleteCommandRequest, parsedDelete, expCtx);

    return deleteQueryShape->sha256Hash(expCtx->getOperationContext(),
                                        SerializationContext::stateDefault());
}

static const NamespaceString kBenchmarkNss =
    NamespaceString::createNamespaceString_forTest("test", "namespace");

// Holds the service context, client, and opCtx shared by all benchmarks.
struct BenchmarkContext {
    ServiceContext::UniqueServiceContext serviceCtx = ServiceContext::make();
    ServiceContext::UniqueClient client = serviceCtx->getService()->makeClient("query_test");
    ServiceContext::UniqueOperationContext opCtx = client->makeOperationContext();

    BenchmarkContext() {
        QueryFCVEnvironmentForTest::setUp();
        ClientMetadata::setFromMetadata(
            opCtx->getClient(), query_benchmark_constants::kMockClientMetadataElem, false);
    }
};

write_ops::DeleteCommandRequest buildDeleteCommandRequest(
    const query_benchmark_constants::DeleteSpec& spec) {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("delete", kBenchmarkNss.coll());
    cmdBuilder.append("$db", kBenchmarkNss.db_forSharding());
    BSONArrayBuilder deletesArr;
    for (const auto& d : spec.deletes) {
        deletesArr.append(BSON("q" << d << "limit" << 0 << "collation" << BSON("locale" << "fr")));
    }
    cmdBuilder.append("deletes", deletesArr.arr());
    if (spec.let) {
        cmdBuilder.append("let", *spec.let);
    }
    return write_ops::DeleteCommandRequest::parseOwned(cmdBuilder.obj());
}

// Populates outRequest from the command entry at opIndex, then creates outExpCtx via
// ExpressionContextBuilder::fromRequest so that any let variables in the request are injected
// into the expression context before parsing. outRequest must outlive the returned ParsedDelete.
ParsedDelete buildParsedDelete(OperationContext* opCtx,
                               const write_ops::DeleteCommandRequest& deleteCommandRequest,
                               DeleteRequest& outRequest,
                               boost::intrusive_ptr<ExpressionContext>& outExpCtx,
                               size_t opIndex = 0) {
    auto& deletes = deleteCommandRequest.getDeletes();
    tassert(12205800, "opIndex out of range", opIndex < deletes.size());
    auto& deleteOp = deletes[opIndex];

    outRequest.setNsString(kBenchmarkNss);
    outRequest.setQuery(deleteOp.getQ());
    outRequest.setMulti(deleteOp.getMulti());
    if (deleteOp.getCollation()) {
        outRequest.setCollation(deleteOp.getCollation()->getOwned());
    }
    outRequest.setHint(deleteOp.getHint().getOwned());
    if (deleteCommandRequest.getLet()) {
        outRequest.setLet(*deleteCommandRequest.getLet());
    }

    outExpCtx = ExpressionContextBuilder{}.fromRequest(opCtx, outRequest).build();
    return uassertStatusOK(parsed_delete_command::parse(
        outExpCtx,
        &outRequest,
        makeExtensionsCallback<ExtensionsCallbackReal>(opCtx, &outRequest.getNsString())));
}

void runBenchmark(const query_benchmark_constants::DeleteSpec& spec,
                  const ShapifyDeleteTestType& testType,
                  benchmark::State& state) {
    BenchmarkContext ctx;

    auto deleteCommandRequest = buildDeleteCommandRequest(spec);
    DeleteRequest deleteRequest;
    boost::intrusive_ptr<ExpressionContext> expCtx;
    auto parsedDelete =
        buildParsedDelete(ctx.opCtx.get(), deleteCommandRequest, deleteRequest, expCtx);

    for (auto keepRunning : state) {
        switch (testType) {
            case ShapifyDeleteTestType::kGenerateDeleteKey:
                benchmark::DoNotOptimize(
                    shapifyAndGenerateKey(expCtx, parsedDelete, deleteCommandRequest));
                break;
            case ShapifyDeleteTestType::kSHA256Hash:
                benchmark::DoNotOptimize(
                    shapifyAndSHA256Hash(expCtx, parsedDelete, deleteCommandRequest));
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(12205802);
        }
    }
}

// Benchmarks the cost of shapifying all ops in a multi-op delete command in a single iteration.
void runMultiOpBenchmark(const ShapifyDeleteTestType& testType, benchmark::State& state) {
    BenchmarkContext ctx;

    const auto& spec = query_benchmark_constants::kMultiOpDeleteSpec;
    const size_t n = spec.deletes.size();
    auto deleteCommandRequest = buildDeleteCommandRequest(spec);

    // pre-size so ParsedDelete's raw pointer into deleteRequests remains valid.
    std::vector<DeleteRequest> deleteRequests(n);
    std::vector<boost::intrusive_ptr<ExpressionContext>> expCtxs(n);

    std::vector<ParsedDelete> parsedDeletes;
    for (size_t i = 0; i < n; ++i) {
        parsedDeletes.emplace_back(buildParsedDelete(
            ctx.opCtx.get(), deleteCommandRequest, deleteRequests[i], expCtxs[i], i));
    }

    for (auto keepRunning : state) {
        for (size_t i = 0; i < n; ++i) {
            switch (testType) {
                case ShapifyDeleteTestType::kGenerateDeleteKey:
                    benchmark::DoNotOptimize(
                        shapifyAndGenerateKey(expCtxs[i], parsedDeletes[i], deleteCommandRequest));
                    break;
                case ShapifyDeleteTestType::kSHA256Hash:
                    benchmark::DoNotOptimize(
                        shapifyAndSHA256Hash(expCtxs[i], parsedDeletes[i], deleteCommandRequest));
                    break;
                default:
                    MONGO_UNREACHABLE_TASSERT(12205803);
            }
        }
    }
}

// --- Benchmarks: plain query-predicate complexity ---

void runBenchmark(ShapifyDeleteTestType testType, benchmark::State& state) {
    auto queryComplexity = static_cast<query_benchmark_constants::QueryComplexity>(state.range(0));
    query_benchmark_constants::DeleteSpec spec{
        {query_benchmark_constants::queryComplexityToJSON(queryComplexity)}};
    runBenchmark(spec, testType, state);
}

void BM_ShapifyAndGenerateKey(benchmark::State& state) {
    runBenchmark(ShapifyDeleteTestType::kGenerateDeleteKey, state);
}

void BM_ShapifyAndSHA256Hash(benchmark::State& state) {
    runBenchmark(ShapifyDeleteTestType::kSHA256Hash, state);
}

// --- Benchmarks: let/$expr complexity ---

void BM_LetPredicate_ShapifyAndGenerateKey(benchmark::State& state) {
    auto complexity = static_cast<query_benchmark_constants::LetDeleteComplexity>(state.range(0));
    runBenchmark(query_benchmark_constants::getDeleteWithLetSpec(complexity),
                 ShapifyDeleteTestType::kGenerateDeleteKey,
                 state);
}

void BM_LetPredicate_ShapifyAndSHA256Hash(benchmark::State& state) {
    auto complexity = static_cast<query_benchmark_constants::LetDeleteComplexity>(state.range(0));
    runBenchmark(query_benchmark_constants::getDeleteWithLetSpec(complexity),
                 ShapifyDeleteTestType::kSHA256Hash,
                 state);
}

// --- Benchmarks: multi-op delete (all ops shapified per iteration) ---

void BM_MultiOpDelete_ShapifyAndGenerateKey(benchmark::State& state) {
    runMultiOpBenchmark(ShapifyDeleteTestType::kGenerateDeleteKey, state);
}

void BM_MultiOpDelete_ShapifyAndSHA256Hash(benchmark::State& state) {
    runMultiOpBenchmark(ShapifyDeleteTestType::kSHA256Hash, state);
}

#define QUERY_COMPLEXITY_ARGS()                                                                \
    ArgNames({"queryComplexity"})                                                              \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kIDHack)})        \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kMildlyComplex)}) \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kMkComplex)})     \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kVeryComplex)})   \
        ->Threads(1)

#define LET_ARGS()                                                                           \
    ArgNames({"letComplexity"})                                                              \
        ->Args({static_cast<int>(query_benchmark_constants::LetDeleteComplexity::kSimple)})  \
        ->Args({static_cast<int>(query_benchmark_constants::LetDeleteComplexity::kComplex)}) \
        ->Threads(1)

BENCHMARK(BM_ShapifyAndGenerateKey)->QUERY_COMPLEXITY_ARGS();
BENCHMARK(BM_ShapifyAndSHA256Hash)->QUERY_COMPLEXITY_ARGS();
BENCHMARK(BM_LetPredicate_ShapifyAndGenerateKey)->LET_ARGS();
BENCHMARK(BM_LetPredicate_ShapifyAndSHA256Hash)->LET_ARGS();
BENCHMARK(BM_MultiOpDelete_ShapifyAndGenerateKey)->Threads(1);
BENCHMARK(BM_MultiOpDelete_ShapifyAndSHA256Hash)->Threads(1);

}  // namespace
}  // namespace mongo
