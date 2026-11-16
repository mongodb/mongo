// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_query_stats.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <absl/hash/hash.h>
#include <fmt/format.h>

namespace mongo::query_stats_bm {

// No-evict store cap for benchmarks: 'kLargeStoreSize' entries at ~2KB each fit well within this,
// so nothing is evicted mid-run and the store-size benchmark can rely on the known store size. 1GB
// is also the production cap, so the largest point models big clusters (~300K distinct shapes).
inline constexpr size_t kStoreCacheSize = 1 * 1024 * 1024 * 1024;

// Store cache size small enough that a benchmark's population overflows it, forcing eviction on
// every write past capacity. Entries are ~2KB each (QueryStatsStoreEntryBudgetor), so this holds
// roughly kEvictingStoreCacheSize / 2KB ≈ 500 entries in total; the per-partition split
// (cacheSize / numPartitions) only changes their distribution, not that total.
inline constexpr size_t kEvictingStoreCacheSize = 1 << 20;

// Population sizes (distinct query shapes) for benchmarks that vary how full the store is.
inline constexpr size_t kSmallStoreSize = 1'000;
inline constexpr size_t kLargeStoreSize = 300'000;

// Fixed partition count, so results are comparable across hosts. Production uses
// max(ceil(cacheSize / 16MB), numLogicalCores), which is also 64 for a 1GB store on hosts with at
// most 64 cores.
inline constexpr size_t kNumPartitions = 64;

/**
 * Sets 'internalQueryStatsMatchPushdownOptimizationEnabled' for its lifetime and restores the
 * previous value on destruction.
 */
class PushdownKnobGuard {
public:
    explicit PushdownKnobGuard(bool enabled)
        : _previous(internalQueryStatsMatchPushdownOptimizationEnabled.load()) {
        internalQueryStatsMatchPushdownOptimizationEnabled.store(enabled);
    }
    ~PushdownKnobGuard() {
        internalQueryStatsMatchPushdownOptimizationEnabled.store(_previous);
    }

private:
    const bool _previous;
};

/**
 * Owns the process-global ServiceContext and the query stats store for one benchmark invocation.
 * 'pushdownEnabled' sets the keyHash pushdown knob, which the store manager reads at construction
 * to decide whether to maintain the KeyHashIndex.
 */
struct Environment {
    // Declared first so the knob is set before, and restored after, the store manager's lifetime.
    PushdownKnobGuard knobGuard;
    QueryTestScopedGlobalServiceContext scopedGlobalServiceContext;
    const size_t cacheSize;
    const size_t numPartitions;

    explicit Environment(bool pushdownEnabled,
                         size_t cacheSize = kStoreCacheSize,
                         size_t numPartitions = kNumPartitions)
        : knobGuard(pushdownEnabled), cacheSize(cacheSize), numPartitions(numPartitions) {
        QueryFCVEnvironmentForTest::setUp();
        resetStore();
    }

    // Replaces the store with a new, empty one of the same configuration.
    void resetStore() {
        query_stats::QueryStatsStoreManager::get(scopedGlobalServiceContext.getServiceContext()) =
            std::make_unique<query_stats::QueryStatsStoreManager>(cacheSize, numPartitions);
    }

    // Moves the store out of this Environment so it can be cached, leaving the Environment without
    // a store until one is adopted.
    std::unique_ptr<query_stats::QueryStatsStoreManager> takeStore() {
        return std::move(query_stats::QueryStatsStoreManager::get(
            scopedGlobalServiceContext.getServiceContext()));
    }

    // Installs 'store' as this Environment's store.
    void adoptStore(std::unique_ptr<query_stats::QueryStatsStoreManager> store) {
        query_stats::QueryStatsStoreManager::get(scopedGlobalServiceContext.getServiceContext()) =
            std::move(store);
    }
};

/**
 * A single operation's OperationContext + ExpressionContext.
 */
struct Operation {
    ServiceContext::UniqueOperationContext opCtx;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx;

    explicit Operation(Environment& env)
        : opCtx(env.scopedGlobalServiceContext.makeOperationContext()),
          expCtx(make_intrusive<ExpressionContextForTest>(
              opCtx.get(), NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin))) {
    }
};

/**
 * Builds an equality filter of 'numFields' predicates named "<prefix><i>_<field>". Putting 'i' in
 * the field names makes each 'i' a distinct query shape.
 */
inline BSONObj makeDistinctFilter(std::string_view prefix, size_t i, size_t numFields) {
    BSONObjBuilder filter;
    for (size_t field = 0; field < numFields; ++field) {
        std::string fieldName = str::stream() << prefix << i << "_" << field;
        filter << fieldName << BSON("$eq" << 1);
    }
    return filter.obj();
}

/**
 * Builds a find-command key over 'nss' from 'filter'.
 */
inline std::unique_ptr<const query_stats::Key> makeFindKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceStringOrUUID& nss,
    const BSONObj& filter) {
    auto fcr = std::make_unique<FindCommandRequest>(nss);
    fcr->setFilter(filter);
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcr)}));
    auto shape = uassertStatusOK(
        shape_helpers::tryMakeShape<query_shape::FindCmdShape>(*parsedFind, expCtx));
    return std::make_unique<query_stats::FindKey>(expCtx,
                                                  *parsedFind->findCommandRequest,
                                                  std::move(shape),
                                                  query_shape::CollectionType::kCollection);
}

/**
 * Builds a distinct single-predicate find-command key for shape 'i'.
 */
inline std::unique_ptr<const query_stats::Key> makeKey(Operation& op, int i) {
    return makeFindKey(op.expCtx,
                       op.expCtx->getNamespaceString(),
                       makeDistinctFilter("field", static_cast<size_t>(i), 1));
}

/**
 * Inserts a distinct find-command shape 'i' into 'op's store the way writeQueryStats() does on a
 * miss: look up under the partition lock, serialize the representative shape off-lock, then
 * reacquire the lock and insert. The user-visible hash is computed under the lock and cached on the
 * entry only when the KeyHashIndex is maintained. Returns the cached hash or "" if none.
 */
inline std::string insertDistinctFindShape(Operation& op, int i) {
    auto key = makeKey(op, i);
    const std::size_t storeKey = absl::HashOf(*key);

    auto& manager = *query_stats::QueryStatsStoreManager::get(op.opCtx->getServiceContext());
    auto& store = manager.getQueryStatsStore();
    {
        auto&& [statusWithEntry, partitionLock] = store.getWithPartitionLock(storeKey);
    }

    const BSONObj representativeShapeKey =
        query_stats::computeRepresentativeShapeKey(op.opCtx.get(), *key);

    auto&& [statusWithEntry, partitionLock] = store.getWithPartitionLock(storeKey);
    std::string userVisibleHash = manager.hasSecondaryIndex()
        ? query_stats::computeUserVisibleHash(representativeShapeKey)
        : std::string{};
    store.put(
        storeKey, query_stats::QueryStatsEntry(std::move(key), userVisibleHash), partitionLock);
    return userVisibleHash;
}

/**
 * Populates 'env's store with 'numShapes' distinct find shapes and returns their user-visible
 * hashes.
 */
inline std::vector<std::string> populateStore(Environment& env, int numShapes) {
    Operation op(env);
    std::vector<std::string> userVisibleHashes;
    userVisibleHashes.reserve(numShapes);
    for (int i = 0; i < numShapes; ++i) {
        auto userVisibleHash = insertDistinctFindShape(op, i);
        // Without the KeyHashIndex the entry caches no hash, but callers still need it to target.
        if (userVisibleHash.empty()) {
            userVisibleHash = query_stats::computeUserVisibleHash(op.opCtx.get(), *makeKey(op, i));
        }
        userVisibleHashes.push_back(std::move(userVisibleHash));
    }
    return userVisibleHashes;
}

/**
 * A source pipeline and its built executable form.
 */
struct QueryStatsPipeline {
    std::unique_ptr<Pipeline> sourcePipeline;
    std::unique_ptr<exec::agg::Pipeline> execPipeline;
};

/**
 * Builds and optimizes [{$queryStats: {}}, {$match: {keyHash: <$eq|$in> targets}}].
 */
inline QueryStatsPipeline buildQueryStatsPipeline(Operation& op,
                                                  const std::vector<std::string>& targets,
                                                  bool eqOrInPredicate) {
    static const BSONObj kQueryStatsSpec = fromjson("{$queryStats: {}}");
    auto qsSource =
        DocumentSourceQueryStats::createFromBson(kQueryStatsSpec.firstElement(), op.expCtx);

    BSONArrayBuilder in;
    for (const auto& t : targets) {
        in.append(t);
    }

    auto matchSource = eqOrInPredicate
        ? DocumentSourceMatch::create(BSON("keyHash" << BSON("$eq" << targets.front())), op.expCtx)
        : DocumentSourceMatch::create(BSON("keyHash" << BSON("$in" << in.arr())), op.expCtx);

    auto sourcePipeline = Pipeline::create({qsSource, matchSource}, op.expCtx);
    pipeline_optimization::optimizePipeline(*sourcePipeline);

    auto execPipeline = exec::agg::buildPipeline(sourcePipeline->freeze());
    return {std::move(sourcePipeline), std::move(execPipeline)};
}

/**
 * Asserts that optimizing the pipeline captured exactly 'targets' as the $queryStats keyHash lookup
 * set when 'pushdownEnabled', and captured nothing when it is not. Called once outside the timed
 * loop.
 */
inline void assertPushdownFired(Operation& op,
                                const std::vector<std::string>& targets,
                                bool eqOrInPredicate,
                                bool pushdownEnabled) {
    auto pipeline = buildQueryStatsPipeline(op, targets, eqOrInPredicate);
    auto* queryStatsSource = dynamic_cast<DocumentSourceQueryStats*>(
        pipeline.sourcePipeline->getSources().front().get());
    invariant(queryStatsSource, "the first stage is not $queryStats");
    const auto& captured = queryStatsSource->getKeyHashLookupLocations_forTest();

    if (!pushdownEnabled) {
        invariant(!captured.has_value(), "the scan path captured keyHash targets");
        return;
    }
    invariant(captured.has_value(), "the pushdown path captured no keyHash targets");

    auto expected = targets;
    auto actual = *captured;
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    invariant(
        actual == expected,
        fmt::format("pushdown captured {} targets, expected {}", actual.size(), expected.size()));
}

/**
 * Builds, optimizes, runs, and destroys the keyHash $match pipeline once, returning the number of
 * documents produced (one per targeted keyHash). Teardown is part of each operation's cost, so it
 * is timed along with the rest.
 */
inline size_t runPipeline(Operation& op,
                          const std::vector<std::string>& targets,
                          bool eqOrInPredicate) {
    auto pipeline = buildQueryStatsPipeline(op, targets, eqOrInPredicate);
    size_t numDocs = 0;
    while (pipeline.execPipeline->getNext()) {
        ++numDocs;
    }
    return numDocs;
}

}  // namespace mongo::query_stats_bm
