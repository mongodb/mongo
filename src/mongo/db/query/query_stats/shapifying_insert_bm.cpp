// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_bm_constants.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_shape/insert_cmd_shape.h"
#include "mongo/db/query/query_stats/write_key.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/duration.h"

#include <memory>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

// Different types of shapifying insert benchmarks.
// We have separate benchmarks: one for computing the InsertKey (absl hash) and another for the
// SHA-256 hash of InsertCmdShape. The InsertKey hash is on the hot path; SHA-256 is sampled only.
enum class ShapifyInsertTestType : int { kGenerateInsertKey = 0, kSHA256Hash };

auto shapifyAndGenerateKey(OperationContext* opCtx,
                           const write_ops::InsertCommandRequest& insertCommandRequest) {
    query_stats::InsertKey key(opCtx,
                               insertCommandRequest,
                               std::make_unique<query_shape::InsertCmdShape>(insertCommandRequest),
                               query_benchmark_constants::kCollectionType);

    return absl::HashOf(key);
}

auto shapifyAndSHA256Hash(OperationContext* opCtx,
                          const write_ops::InsertCommandRequest& insertCommandRequest) {
    auto insertQueryShape = std::make_unique<query_shape::InsertCmdShape>(insertCommandRequest);

    return insertQueryShape->sha256Hash(opCtx, SerializationContext::stateDefault());
}

void runBenchmark(int numDocuments,
                  const ShapifyInsertTestType& testType,
                  benchmark::State& state) {
    QueryFCVEnvironmentForTest::setUp();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("query_test");

    auto opCtx = client->makeOperationContext();
    ClientMetadata::setFromMetadata(
        opCtx->getClient(), query_benchmark_constants::kMockClientMetadataElem, false);

    BSONObjBuilder cmdBuilder;
    cmdBuilder.append("insert", "coll");
    {
        BSONArrayBuilder docsBuilder(cmdBuilder.subarrayStart("documents"));
        for (int i = 0; i < numDocuments; ++i) {
            docsBuilder.append(BSON("x" << i << "y"
                                        << "value"
                                        << "z" << true));
        }
    }
    cmdBuilder.append("$db", "test");
    auto insertCommandRequest = write_ops::InsertCommandRequest::parseOwned(cmdBuilder.obj());

    for (auto keepRunning : state) {
        switch (testType) {
            case ShapifyInsertTestType::kGenerateInsertKey:
                benchmark::DoNotOptimize(shapifyAndGenerateKey(opCtx.get(), insertCommandRequest));
                break;
            case ShapifyInsertTestType::kSHA256Hash:
                benchmark::DoNotOptimize(shapifyAndSHA256Hash(opCtx.get(), insertCommandRequest));
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(12205700);
        }
    }
}

void BM_Insert_ShapifyAndGenerateKey(benchmark::State& state) {
    runBenchmark(
        static_cast<int>(state.range(0)), ShapifyInsertTestType::kGenerateInsertKey, state);
}

void BM_Insert_ShapifyAndSHA256Hash(benchmark::State& state) {
    runBenchmark(static_cast<int>(state.range(0)), ShapifyInsertTestType::kSHA256Hash, state);
}

// We do not add a document complexity dimension because the 'documents' field is always shapified
// into '?array<?object>' (O(1)).
#define INSERT_ARGS() ArgNames({"numDocuments"})->Args({1})->Args({100})->Threads(1)

BENCHMARK(BM_Insert_ShapifyAndGenerateKey)->INSERT_ARGS();
BENCHMARK(BM_Insert_ShapifyAndSHA256Hash)->INSERT_ARGS();

}  // namespace
}  // namespace mongo
