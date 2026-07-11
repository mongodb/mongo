// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_bm_constants.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_shape/update_cmd_builder.h"
#include "mongo/db/query/query_shape/update_cmd_shape.h"
#include "mongo/db/query/query_stats/write_key.h"
#include "mongo/db/query/write_ops/parsed_update.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/duration.h"

#include <climits>
#include <memory>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

// Different types of shapifying update benchmarks.
// We have separate benchmark: One for computing QSH and another that computes the UpdateKey
// We will compute QSH all the time on the hot path, while $queryStats is sampling only.
enum class ShapifyUpdateTestType : int { kGenerateUpdateKey = 0, kSHA256Hash };

auto shapifyAndGenerateKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           const ParsedUpdate& parsedUpdate,
                           const write_ops::UpdateCommandRequest& updateCommandRequest) {
    query_stats::UpdateKey key(
        expCtx,
        updateCommandRequest,
        boost::none /* hint */,
        std::make_unique<query_shape::UpdateCmdShape>(updateCommandRequest, parsedUpdate, expCtx),
        query_benchmark_constants::kCollectionType);

    return absl::HashOf(key);
}

auto shapifyAndSHA256Hash(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          const ParsedUpdate& parsedUpdate,
                          const write_ops::UpdateCommandRequest& updateCommandRequest) {
    auto updateQueryShape =
        std::make_unique<query_shape::UpdateCmdShape>(updateCommandRequest, parsedUpdate, expCtx);

    return updateQueryShape->sha256Hash(expCtx->getOperationContext(),
                                        SerializationContext::stateDefault());
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
    builder.arrayFilters = updateSpec.arrayFilters;

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
                MONGO_UNREACHABLE_TASSERT(11400602);
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

// Benchmark functions for modifier updates.
void BM_ModifierUpdate_ShapifyAndGenerateKey(benchmark::State& state) {
    auto queryComplexity = static_cast<query_benchmark_constants::QueryComplexity>(state.range(0));
    auto modifierUpdateComplexity =
        static_cast<query_benchmark_constants::ModifierUpdateComplexity>(state.range(1));
    bool useArrayFilters = state.range(2) != 0;

    auto updateSpec =
        query_benchmark_constants::getUpdateSpec(modifierUpdateComplexity, useArrayFilters);
    runBenchmark(query_benchmark_constants::queryComplexityToJSON(queryComplexity),
                 updateSpec,
                 ShapifyUpdateTestType::kGenerateUpdateKey,
                 state);
}

void BM_ModifierUpdate_ShapifyAndSHA256Hash(benchmark::State& state) {
    auto queryComplexity = static_cast<query_benchmark_constants::QueryComplexity>(state.range(0));
    auto modifierUpdateComplexity =
        static_cast<query_benchmark_constants::ModifierUpdateComplexity>(state.range(1));
    bool useArrayFilters = state.range(2) != 0;

    auto updateSpec =
        query_benchmark_constants::getUpdateSpec(modifierUpdateComplexity, useArrayFilters);
    runBenchmark(query_benchmark_constants::queryComplexityToJSON(queryComplexity),
                 updateSpec,
                 ShapifyUpdateTestType::kSHA256Hash,
                 state);
}

// We do not add a complexity dimension for replacement updates because the 'u' statement will
// always get shapified into '?object'. In other words, the shapification work done for the 'u'
// field for replacement updates is O(1).
#define REPLACEMENT_ARGS()                                                                     \
    ArgNames({"queryComplexity"})                                                              \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kIDHack)})        \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kMildlyComplex)}) \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kMkComplex)})     \
        ->Args({static_cast<int>(query_benchmark_constants::QueryComplexity::kVeryComplex)})   \
        ->Threads(1)

#define PIPELINE_ARGS()                                                                        \
    ArgNames({"queryComplexity", "pipelineComplexity"})                                        \
        ->ArgsProduct(                                                                         \
            {{static_cast<int>(query_benchmark_constants::QueryComplexity::kIDHack),           \
              static_cast<int>(query_benchmark_constants::QueryComplexity::kMildlyComplex),    \
              static_cast<int>(query_benchmark_constants::QueryComplexity::kMkComplex),        \
              static_cast<int>(query_benchmark_constants::QueryComplexity::kVeryComplex)},     \
             {static_cast<int>(query_benchmark_constants::PipelineComplexity::kSimple),        \
              static_cast<int>(query_benchmark_constants::PipelineComplexity::kWithConstants), \
              static_cast<int>(                                                                \
                  query_benchmark_constants::PipelineComplexity::kWithMultipleStages),         \
              static_cast<int>(query_benchmark_constants::PipelineComplexity::                 \
                                   kWithMultipleStagesAndExpressions)}})                       \
        ->Threads(1)
#define MODIFIER_UPDATE_ARGS()                                                                 \
    ArgNames({"queryComplexity", "modifierUpdateComplexity", "useArrayFilters"})               \
        ->ArgsProduct(                                                                         \
            {{static_cast<int>(query_benchmark_constants::QueryComplexity::kIDHack),           \
              static_cast<int>(query_benchmark_constants::QueryComplexity::kMildlyComplex),    \
              static_cast<int>(query_benchmark_constants::QueryComplexity::kMkComplex),        \
              static_cast<int>(query_benchmark_constants::QueryComplexity::kVeryComplex)},     \
             {static_cast<int>(query_benchmark_constants::ModifierUpdateComplexity::kSimple),  \
              static_cast<int>(                                                                \
                  query_benchmark_constants::ModifierUpdateComplexity::kMildlyComplex),        \
              static_cast<int>(query_benchmark_constants::ModifierUpdateComplexity::kComplex), \
              static_cast<int>(                                                                \
                  query_benchmark_constants::ModifierUpdateComplexity::kVeryComplex)},         \
             {false, true}})                                                                   \
        ->Threads(1)

BENCHMARK(BM_ReplacementUpdate_ShapifyAndGenerateKey)->REPLACEMENT_ARGS();
BENCHMARK(BM_ReplacementUpdate_ShapifyAndSHA256Hash)->REPLACEMENT_ARGS();
BENCHMARK(BM_PipelineUpdate_ShapifyAndGenerateKey)->PIPELINE_ARGS();
BENCHMARK(BM_PipelineUpdate_ShapifyAndSHA256Hash)->PIPELINE_ARGS();
BENCHMARK(BM_ModifierUpdate_ShapifyAndGenerateKey)->MODIFIER_UPDATE_ARGS();
BENCHMARK(BM_ModifierUpdate_ShapifyAndSHA256Hash)->MODIFIER_UPDATE_ARGS();

}  // namespace
}  // namespace mongo
