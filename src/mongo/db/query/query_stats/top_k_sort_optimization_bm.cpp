// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_shape/find_cmd_shape.h"
#include "mongo/db/query/query_stats/agg_key.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/query_stats_entry.h"
#include "mongo/db/shard_role/lock_manager/locker.h"

#include <map>
#include <memory>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {

namespace {

// Two query stats store sizes.
constexpr size_t kLargeStoreSize = 300'000;
constexpr size_t kSmallStoreSize = 1'000;

// The two stored query shapes.
enum class ShapeKind { kSimpleFind, kNestedPipeline };

/**
 * Builds an equality filter of 'numFields' predicates named "<prefix><i>_<field>", e.g. for
 * prefix "f_", i = 0, numFields = 10:
 *
 *   {f_0_0: {$eq: 1}, f_0_1: {$eq: 1}, ..., f_0_9: {$eq: 1}}
 *
 * Putting 'i' in the field names ensures each entry is a distinct query shape.
 */
BSONObj makeDistinctFilter(std::string_view prefix, size_t i, size_t numFields) {
    BSONObjBuilder filter;
    for (size_t field = 0; field < numFields; ++field) {
        std::string fieldName = str::stream() << prefix << i << "_" << field;
        filter << fieldName << BSON("$eq" << 1);
    }
    return filter.obj();
}

/**
 * Assembles a find command with a distinct 10-field equality filter, e.g. for i = 0:
 *
 *   db.bench.coll.find({f_0_0: {$eq: 1}, f_0_1: {$eq: 1}, ..., f_0_9: {$eq: 1}})
 */
std::unique_ptr<const query_stats::Key> makeSimpleFindKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, size_t i) {
    NamespaceStringOrUUID nss{NamespaceString::createNamespaceString_forTest("bench.coll")};
    auto fcr = std::make_unique<FindCommandRequest>(nss);
    fcr->setFilter(makeDistinctFilter("f_", i, 10));
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcr)}));
    return std::make_unique<query_stats::FindKey>(
        expCtx,
        *parsedFind->findCommandRequest,
        std::make_unique<query_shape::FindCmdShape>(*parsedFind, expCtx),
        query_shape::CollectionType::kCollection);
}

/**
 * Assembles an aggregate command whose shape nests a sub-pipeline, e.g. for i = 0:
 *
 *   db.bench.coll.aggregate([
 *     {$match: {key_0_0: {$eq: 1}, key_0_1: {$eq: 1}, key_0_2: {$eq: 1}, key_0_3: {$eq: 1}}},
 *     {$lookup: {
 *        from: "bench.other",
 *        pipeline: [
 *          {$match: {status: "active"}},
 *          {$project: {a: 1, b: 1, c: 1}},
 *          {$group: {_id: "$a", total: {$sum: "$b"}}},
 *          {$sort: {total: -1}},
 *        ],
 *        as: "joined",
 *     }},
 *     {$unwind: "$joined"},
 *   ])
 */
std::unique_ptr<const query_stats::Key> makeNestedPipelineKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, size_t i) {
    NamespaceString nss{NamespaceString::createNamespaceString_forTest("bench.coll")};

    std::vector<BSONObj> rawPipeline = {
        BSON("$match" << makeDistinctFilter("key_", i, 4)),
        BSON("$lookup" << BSON(
                 "from" << "bench.other"
                        << "pipeline"
                        << BSON_ARRAY(
                               BSON("$match" << BSON("status" << "active"))
                               << BSON("$project" << BSON("a" << 1 << "b" << 1 << "c" << 1))
                               << BSON("$group" << BSON("_id" << "$a"
                                                              << "total" << BSON("$sum" << "$b")))
                               << BSON("$sort" << BSON("total" << -1)))
                        << "as" << "joined")),
        BSON("$unwind" << "$joined"),
    };

    AggregateCommandRequest acr(nss);
    acr.setPipeline(rawPipeline);
    auto pipeline =
        pipeline_factory::makePipeline(rawPipeline, expCtx, pipeline_factory::kOptionsMinimal);
    auto aggShape = std::make_unique<query_shape::AggCmdShape>(
        acr, nss, pipeline->getInvolvedCollections(), *pipeline, expCtx);
    return std::make_unique<query_stats::AggKey>(expCtx,
                                                 acr,
                                                 std::move(aggShape),
                                                 pipeline->getInvolvedCollections(),
                                                 query_shape::CollectionType::kCollection);
}

/**
 * Test fixture: populates a QueryStatsStore with numEntries distinct entries, each assigned a
 * random workingTimeMillis value so the sort has real work to do.
 *
 * Each fixture owns its own ServiceContext so that getQueryStatsStore() resolves to the right
 * store when the pipeline runs against that fixture's opCtx.
 */
struct Fixture {
    explicit Fixture(size_t numEntries, ShapeKind shapeKind) {
        QueryFCVEnvironmentForTest::setUp();

        serviceCtx = ServiceContext::make();
        client = serviceCtx->getService()->makeClient("bench");
        opCtx = client->makeOperationContext();

        // Install a shard-role Locker on the opCtx to avoid failures when reading locker->stats()
        // mid-pipeline for slow query logging.
        opCtx->setLockState_DO_NOT_USE(std::make_unique<Locker>(opCtx->getServiceContext()));

        auto keyExpCtx = make_intrusive<ExpressionContextForTest>(opCtx.get());

        query_stats::QueryStatsStoreManager::get(serviceCtx.get()) =
            std::make_unique<query_stats::QueryStatsStoreManager>(
                /* cacheSize */ 4ULL << 30 /* 4 GiB — large enough for all entries */,
                /* numPartitions */ 128);

        auto& store = query_stats::getQueryStatsStore(opCtx.get());

        // Using 'std::mt19937_64' since we want a uniform distribution with a fixed seed to
        // assign 'workingTimeMillis' deterministically.
        std::mt19937_64 rng(0xC0FFEE);
        std::uniform_int_distribution<int64_t> timeDist(0, 10'000);

        size_t totalEvicted = 0;
        for (size_t i = 0; i < numEntries; ++i) {
            std::unique_ptr<const query_stats::Key> key = shapeKind == ShapeKind::kSimpleFind
                ? makeSimpleFindKey(keyExpCtx, i)
                : makeNestedPipelineKey(keyExpCtx, i);
            auto storeKey = absl::HashOf(*key);
            query_stats::QueryStatsEntry entry(std::move(key));
            // Assign a random value for 'workingTimeMillis' so the $sort has work to do.
            entry.workingTimeMillis.aggregate(timeDist(rng));
            totalEvicted += store.put(storeKey, std::move(entry));
        }
        tassert(13503200,
                "query stats store evicted entries; increase the fixture's cacheSize",
                totalEvicted == 0);
    }

    ServiceContext::UniqueServiceContext serviceCtx;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
};

Fixture& getFixture(size_t numEntries, ShapeKind shapeKind) {
    // Lazily create (and cache) one fixture per (store size, shape kind) pair. The same pair is
    // exercised by several $limit args, so caching avoids reloading the store each time.
    static std::map<std::pair<size_t, ShapeKind>, Fixture> fixtures;
    return fixtures.try_emplace({numEntries, shapeKind}, numEntries, shapeKind).first->second;
}

/**
 * Runs the topK pipeline end-to-end. Every benchmark parses, optimizes, and executes the pipeline
 * so that the full cost is reflected in the timing.
 *
 * Args: {limit, storeSize}
 */
void runTopKBenchmark(benchmark::State& state, ShapeKind shapeKind) {
    const long long limit = state.range(0);
    const size_t storeSize = static_cast<size_t>(state.range(1));
    auto& fixture = getFixture(storeSize, shapeKind);

    // The optimization also accepts [$queryStats, $project, $sort, $limit], but we chose to
    // benchmark the minimal shape.
    std::vector<BSONObj> raw = {
        BSON("$queryStats" << BSONObj{}),
        BSON("$sort" << BSON("metrics.workingTimeMillis.sum" << -1)),
        BSON("$limit" << limit),
    };

    size_t lastEmittedCount = 0;
    for (auto _ : state) {
        auto expCtx = make_intrusive<ExpressionContextForTest>(
            fixture.opCtx.get(),
            NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin));
        auto pipeline =
            pipeline_factory::makePipeline(raw, expCtx, pipeline_factory::kOptionsMinimal);
        pipeline_optimization::optimizePipeline(*pipeline);
        pipeline->freeze();
        auto execPipeline = exec::agg::buildPipeline(*pipeline);

        // Consume every emitted document and hand the results to DoNotOptimize so the compiler
        // cannot elide the whole observable-only getNext() loop as dead code, which would leave
        // the timing measuring nothing.
        size_t nEmitted = 0;
        while (auto doc = execPipeline->getNext()) {
            benchmark::DoNotOptimize(*doc);
            ++nEmitted;
        }
        benchmark::DoNotOptimize(nEmitted);
        lastEmittedCount = nEmitted;
    }
    const char* shapeName =
        shapeKind == ShapeKind::kSimpleFind ? "simpleFindShape" : "nestedPipelineShape";
    state.SetLabel(std::to_string(lastEmittedCount) + " docs / store=" + std::to_string(storeSize) +
                   " / " + shapeName);
}

void BM_TopKSort_SimpleFindShape(benchmark::State& state) {
    runTopKBenchmark(state, ShapeKind::kSimpleFind);
}

void BM_TopKSort_NestedPipelineShape(benchmark::State& state) {
    runTopKBenchmark(state, ShapeKind::kNestedPipeline);
}

// Args: {limit, storeSize}
BENCHMARK(BM_TopKSort_SimpleFindShape)
    ->Args({10000, kLargeStoreSize})
    ->Args({1000, kLargeStoreSize})
    ->Args({100, kLargeStoreSize})
    ->Args({10000, kSmallStoreSize})
    ->Args({1000, kSmallStoreSize})
    ->Args({100, kSmallStoreSize})
    ->Unit(benchmark::kMillisecond)
    ->Threads(1);

BENCHMARK(BM_TopKSort_NestedPipelineShape)
    ->Args({10000, kLargeStoreSize})
    ->Args({1000, kLargeStoreSize})
    ->Args({100, kLargeStoreSize})
    ->Args({10000, kSmallStoreSize})
    ->Args({1000, kSmallStoreSize})
    ->Args({100, kSmallStoreSize})
    ->Unit(benchmark::kMillisecond)
    ->Threads(1);

}  // namespace
}  // namespace mongo
