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

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_bm_constants.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_shape/update_cmd_shape.h"
#include "mongo/db/query/query_stats/update_key.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/duration.h"

#include <climits>
#include <memory>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

static const NamespaceStringOrUUID kDefaultTestNss =
    NamespaceStringOrUUID{NamespaceString::createNamespaceString_forTest("test.coll")};

static constexpr auto kCollectionType = query_shape::CollectionType::kCollection;

// A simple replacement update used in the benchmarks.
const auto kReplacementUpdate = fromjson(R"({ name: "John", age: 30 })");

// Different types of shapifying update benchmarks.
// We have separate benchmark: One for computing QSH and another that computes the UpdateKey
// We will compute QSH all the time on the hot path, while $queryStats is sampling only.
enum class ShapifyUpdateTestType : int { kGenerateUpdateKey = 0, kSHA256Hash };

int shapifyAndGenerateKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          const ParsedUpdate& parsedUpdate,
                          const write_ops::UpdateCommandRequest& updateCommandRequest) {
    query_stats::UpdateKey key(
        expCtx,
        updateCommandRequest,
        boost::none /* hint */,
        std::make_unique<query_shape::UpdateCmdShape>(updateCommandRequest, parsedUpdate, expCtx),
        kCollectionType);

    [[maybe_unused]] auto hash = absl::HashOf(key);
    return 0;
}

int shapifyAndSHA256Hash(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const ParsedUpdate& parsedUpdate,
                         const write_ops::UpdateCommandRequest& updateCommandRequest) {
    auto updateQueryShape =
        std::make_unique<query_shape::UpdateCmdShape>(updateCommandRequest, parsedUpdate, expCtx);

    [[maybe_unused]] auto sha256hash = updateQueryShape->sha256Hash(
        expCtx->getOperationContext(), SerializationContext::stateDefault());
    return 0;
}

void runBenchmark(BSONObj predicate,
                  BSONObj update,
                  const ShapifyUpdateTestType& testType,
                  benchmark::State& state) {
    QueryFCVEnvironmentForTest::setUp();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("query_test");

    auto opCtx = client->makeOperationContext();
    ClientMetadata::setFromMetadata(
        opCtx->getClient(), query_benchmark_constants::kMockClientMetadataElem, false);

    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get());
    auto updateCommandRequest =
        std::make_unique<write_ops::UpdateCommandRequest>(expCtx->getNamespaceString());

    std::vector<mongo::write_ops::UpdateOpEntry> updates;
    write_ops::UpdateOpEntry updateOp;
    updateOp.setQ(predicate);
    updateOp.setU(update);
    updateOp.setMulti(false);
    updateOp.setCollation(BSON("locale" << "fr"));

    updates.push_back(updateOp);
    updateCommandRequest->setUpdates(updates);
    updateCommandRequest->setLet(BSON("z" << "abc"));

    mongo::UpdateRequest updateRequest(updateOp);
    updateRequest.setNamespaceString(expCtx->getNamespaceString());

    ParsedUpdate parsedUpdate(opCtx.get(),
                              &updateRequest,
                              CollectionPtr::null,
                              false /*forgoOpCounterIncrements*/,
                              false);
    uassertStatusOK(parsedUpdate.parseRequest());

    // Run the benchmark.
    for (auto keepRunning : state) {
        switch (testType) {
            case ShapifyUpdateTestType::kGenerateUpdateKey:
                benchmark::DoNotOptimize(
                    shapifyAndGenerateKey(expCtx, parsedUpdate, *updateCommandRequest));
                break;
            case ShapifyUpdateTestType::kSHA256Hash:
                benchmark::DoNotOptimize(
                    shapifyAndSHA256Hash(expCtx, parsedUpdate, *updateCommandRequest));
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }
}

void runBenchmark(ShapifyUpdateTestType testType, benchmark::State& state) {
    auto queryComplexity = static_cast<query_benchmark_constants::QueryComplexity>(state.range(0));
    runBenchmark(query_benchmark_constants::queryComplexityToJSON(queryComplexity),
                 kReplacementUpdate,
                 testType,
                 state);
}

void BM_ShapifyAndGenerateKey(benchmark::State& state) {
    runBenchmark(ShapifyUpdateTestType::kGenerateUpdateKey, state);
}

void BM_ShapifyAndSHA256Hash(benchmark::State& state) {
    runBenchmark(ShapifyUpdateTestType::kSHA256Hash, state);
}

// TODO SERVER-111930: Enable this benchmark once recording query stats for
// updates with simple ID query.
// static_cast<int>(query_benchmark_constants::QueryComplexity::kIDHack),
// TODO SERVER-114006 and SERVER-110344: Once shapifying modifier and pipeline update types are
// supported, add arguments for those as well.
#define ADD_ARGS()                                                                          \
    ArgNames({"queryComplexity"})                                                           \
        ->ArgsProduct(                                                                      \
            {{static_cast<int>(query_benchmark_constants::QueryComplexity::kMildlyComplex), \
              static_cast<int>(query_benchmark_constants::QueryComplexity::kMkComplex),     \
              static_cast<int>(query_benchmark_constants::QueryComplexity::kVeryComplex)}}) \
        ->Threads(1)


BENCHMARK(BM_ShapifyAndGenerateKey)->ADD_ARGS();
BENCHMARK(BM_ShapifyAndSHA256Hash)->ADD_ARGS();

}  // namespace
}  // namespace mongo
