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
#include "mongo/db/query/query_shape/update_cmd_builder.h"
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
                  const query_benchmark_constants::UpdateSpec& updateSpec,
                  const ShapifyUpdateTestType& testType,
                  benchmark::State& state) {
    QueryFCVEnvironmentForTest::setUp();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("query_test");

    auto opCtx = client->makeOperationContext();
    ClientMetadata::setFromMetadata(
        opCtx->getClient(), query_benchmark_constants::kMockClientMetadataElem, false);

    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get());

    query_shape::UpdateCmdBuilder builder;
    builder.database = "test";
    builder.collection = "coll";
    builder.q = predicate;
    builder.u = updateSpec.u;
    builder.c = updateSpec.c;
    builder.multi = false;
    builder.collation = BSON("locale" << "fr");
    builder.let = BSON("z" << "abc");

    auto updateCommandRequest = write_ops::UpdateCommandRequest::parseOwned(builder.toBSON());

    auto& updates = updateCommandRequest.getUpdates();
    tassert(
        11400600, "UpdateCommandRequest must contain at least one update entry", !updates.empty());
    auto& updateOp = updates[0];

    mongo::UpdateRequest updateRequest(updateOp);
    updateRequest.setNamespaceString(expCtx->getNamespaceString());

    auto parsedUpdate = uassertStatusOK(parsed_update_command::parse(
        expCtx,
        &updateRequest,
        makeExtensionsCallback<ExtensionsCallbackReal>(opCtx.get(), &updateRequest.getNsString())));

    // Run the benchmark.
    for (auto keepRunning : state) {
        switch (testType) {
            case ShapifyUpdateTestType::kGenerateUpdateKey:
                benchmark::DoNotOptimize(
                    shapifyAndGenerateKey(expCtx, parsedUpdate, updateCommandRequest));
                break;
            case ShapifyUpdateTestType::kSHA256Hash:
                benchmark::DoNotOptimize(
                    shapifyAndSHA256Hash(expCtx, parsedUpdate, updateCommandRequest));
                break;
            default:
                MONGO_UNREACHABLE_TASSERT(1140062);
        }
    }
}

// Benchmark functions for replacement updates.
void BM_ReplacementUpdate_ShapifyAndGenerateKey(benchmark::State& state) {
    auto queryComplexity = static_cast<query_benchmark_constants::QueryComplexity>(state.range(0));
    runBenchmark(query_benchmark_constants::queryComplexityToJSON(queryComplexity),
                 query_benchmark_constants::kReplacementUpdate,
                 ShapifyUpdateTestType::kGenerateUpdateKey,
                 state);
}

void BM_ReplacementUpdate_ShapifyAndSHA256Hash(benchmark::State& state) {
    auto queryComplexity = static_cast<query_benchmark_constants::QueryComplexity>(state.range(0));
    runBenchmark(query_benchmark_constants::queryComplexityToJSON(queryComplexity),
                 query_benchmark_constants::kReplacementUpdate,
                 ShapifyUpdateTestType::kSHA256Hash,
                 state);
}

// Benchmark functions for pipeline updates.
void BM_PipelineUpdate_ShapifyAndGenerateKey(benchmark::State& state) {
    auto queryComplexity = static_cast<query_benchmark_constants::QueryComplexity>(state.range(0));
    auto pipelineComplexity =
        static_cast<query_benchmark_constants::PipelineComplexity>(state.range(1));

    auto updateSpec = query_benchmark_constants::getUpdateSpec(pipelineComplexity);
    runBenchmark(query_benchmark_constants::queryComplexityToJSON(queryComplexity),
                 updateSpec,
                 ShapifyUpdateTestType::kGenerateUpdateKey,
                 state);
}

void BM_PipelineUpdate_ShapifyAndSHA256Hash(benchmark::State& state) {
    auto queryComplexity = static_cast<query_benchmark_constants::QueryComplexity>(state.range(0));
    auto pipelineComplexity =
        static_cast<query_benchmark_constants::PipelineComplexity>(state.range(1));

    auto updateSpec = query_benchmark_constants::getUpdateSpec(pipelineComplexity);
    runBenchmark(query_benchmark_constants::queryComplexityToJSON(queryComplexity),
                 updateSpec,
                 ShapifyUpdateTestType::kSHA256Hash,
                 state);
}

// TODO SERVER-111930: Enable this benchmark once recording query stats for
// updates with simple ID query.
// static_cast<int>(query_benchmark_constants::QueryComplexity::kIDHack),
// TODO SERVER-110351: Evaluate the performance of using the query stats for modifier updates

// We do not add a complexity dimension for replacement updates because the 'u' statement will
// always get shapified into '?object'. In other words, the shapification work done for the 'u'
// field for replacement updates is O(1).
#define REPLACEMENT_ARGS()                                                                     \
    ArgNames({"queryComplexity"})                                                              \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kMildlyComplex)}) \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kMkComplex)})     \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kVeryComplex)})   \
        ->Threads(1)

#define PIPELINE_ARGS()                                                                        \
    ArgNames({"queryComplexity", "pipelineComplexity"})                                        \
        ->ArgsProduct(                                                                         \
            {{static_cast<int>(query_benchmark_constants::QueryComplexity::kMildlyComplex),    \
              static_cast<int>(query_benchmark_constants::QueryComplexity::kMkComplex),        \
              static_cast<int>(query_benchmark_constants::QueryComplexity::kVeryComplex)},     \
             {static_cast<int>(query_benchmark_constants::PipelineComplexity::kSimple),        \
              static_cast<int>(query_benchmark_constants::PipelineComplexity::kWithConstants), \
              static_cast<int>(                                                                \
                  query_benchmark_constants::PipelineComplexity::kWithMultipleStages),         \
              static_cast<int>(query_benchmark_constants::PipelineComplexity::                 \
                                   kWithMultipleStagesAndExpressions)}})                       \
        ->Threads(1)

BENCHMARK(BM_ReplacementUpdate_ShapifyAndGenerateKey)->REPLACEMENT_ARGS();
BENCHMARK(BM_ReplacementUpdate_ShapifyAndSHA256Hash)->REPLACEMENT_ARGS();
BENCHMARK(BM_PipelineUpdate_ShapifyAndGenerateKey)->PIPELINE_ARGS();
BENCHMARK(BM_PipelineUpdate_ShapifyAndSHA256Hash)->PIPELINE_ARGS();

}  // namespace
}  // namespace mongo
