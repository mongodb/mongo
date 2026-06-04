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

#include "mongo/db/query/query_bm_constants.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_shape/insert_cmd_shape.h"
#include "mongo/db/query/query_stats/insert_key.h"
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
