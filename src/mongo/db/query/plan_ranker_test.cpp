// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for mongo/db/query/plan_ranker.h
 */

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_ranker_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <string_view>
#include <utility>
#include <vector>

namespace mongo {
namespace {

std::unique_ptr<CanonicalQuery> makeCanonicalQuery() {
    auto expCtx = new ExpressionContextForTest();
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceString());
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
}

std::unique_ptr<PlanStageStats> makeStats(std::string_view name,
                                          StageType type,
                                          std::unique_ptr<SpecificStats> specific,
                                          size_t works = 1,
                                          size_t advances = 1) {
    auto stats = std::make_unique<PlanStageStats>(name, type);
    stats->common.works = works;
    stats->common.advanced = advances;
    stats->specific = std::move(specific);
    return stats;
}

TEST(PlanRankerTest, NoFetchBonus) {
    // Two plans: one does a fetch, one does not. Assert the plan without the fetch has a higher
    // score. Note there is no projection involved: before SERVER-39241 was fixed we would give
    // these two plans the same score.

    auto goodPlan = makeStats(
        "SHARDING_FILTER", STAGE_SHARDING_FILTER, std::make_unique<ShardingFilterStats>());
    goodPlan->children.emplace_back(
        makeStats("IXSCAN", STAGE_IXSCAN, std::make_unique<IndexScanStats>()));

    auto badPlan = makeStats(
        "SHARDING_FILTER", STAGE_SHARDING_FILTER, std::make_unique<ShardingFilterStats>());
    badPlan->children.emplace_back(makeStats("FETCH", STAGE_FETCH, std::make_unique<FetchStats>()));
    badPlan->children[0]->children.emplace_back(
        makeStats("IXSCAN", STAGE_IXSCAN, std::make_unique<IndexScanStats>()));

    auto cq = makeCanonicalQuery();
    auto scorer = plan_ranker::makePlanScorer();
    auto goodScore = scorer->calculateScore(goodPlan.get(), *cq);
    auto badScore = scorer->calculateScore(badPlan.get(), *cq);

    ASSERT_GT(goodScore, badScore);
}

TEST(PlanRankerTest, DistinctBonus) {
    unittest::ServerParameterGuard shardFilteringDistinct("featureFlagShardFilteringDistinctScan",
                                                          true);

    for (bool deferredExecEnabled : {false, true}) {
        unittest::ServerParameterGuard deferredEngineChoice(
            "featureFlagGetExecutorDeferredEngineChoice", deferredExecEnabled);
        // Two plans: both fetch, one is a DISTINCT_SCAN, other is an IXSCAN.
        // DISTINCT_SCAN does 2 advances / 10 works.
        auto dsStats = std::make_unique<DistinctScanStats>();
        dsStats->isFetching = true;
        dsStats->isShardFilteringDistinctScanEnabled = true;
        auto distinctScanPlan =
            makeStats("DISTINCT_SCAN", STAGE_DISTINCT_SCAN, std::move(dsStats), 10, 2);

        // IXSCAN plan does 2 advances / 10 works.
        auto ixscanPlan = makeStats("FETCH", STAGE_FETCH, std::make_unique<FetchStats>(), 10, 2);
        ixscanPlan->children.emplace_back(
            makeStats("IXSCAN", STAGE_IXSCAN, std::make_unique<IndexScanStats>(), 10, 2));

        auto cq = makeCanonicalQuery();
        cq->setDistinct(CanonicalDistinct("someKey"));
        auto scorer = plan_ranker::makePlanScorer();
        auto distinctScore = scorer->calculateScore(distinctScanPlan.get(), *cq);
        auto ixscanScore = scorer->calculateScore(ixscanPlan.get(), *cq);

        // Both plans should tie now- a tie-breaker will be applied at a later stage.
        ASSERT_EQ(distinctScore, ixscanScore);

        // Now we change to an aggregation context.
        cq->setAggWithNonEmptyPipeline(true);

        // When in a distinct() context, productivity is considered larger in a distinct, even if
        // both plans have the same advances:work ratio. A DISTINCT_SCAN should now win by a large
        // margin (tie breaker).
        distinctScore = scorer->calculateScore(distinctScanPlan.get(), *cq);
        ixscanScore = scorer->calculateScore(ixscanPlan.get(), *cq);
        ASSERT_GT(distinctScore, ixscanScore);

        // If we make the IXSCAN 5x more productive, it will tie with the DISTINCT_SCAN.
        ixscanPlan->children[0]->common.advanced = 10;
        ixscanPlan->common.advanced = 10;
        distinctScore = scorer->calculateScore(distinctScanPlan.get(), *cq);
        ixscanScore = scorer->calculateScore(ixscanPlan.get(), *cq);
        ASSERT_EQ(ixscanScore, distinctScore);

        // If we make the IXSCAN 5.5x more productive, it will win!
        ixscanPlan->children[0]->common.advanced = 11;
        ixscanPlan->common.advanced = 11;
        distinctScore = scorer->calculateScore(distinctScanPlan.get(), *cq);
        ixscanScore = scorer->calculateScore(ixscanPlan.get(), *cq);
        ASSERT_GT(ixscanScore, distinctScore);
    }
}

}  // namespace
}  // namespace mongo
