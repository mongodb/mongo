/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_ranker_util.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"

#include <utility>
#include <vector>

using namespace mongo;

namespace {

using std::make_unique;
using std::string;
using std::unique_ptr;

unique_ptr<CanonicalQuery> makeCanonicalQuery() {
    auto expCtx = new ExpressionContextForTest();
    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceString());
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
}

unique_ptr<PlanStageStats> makeStats(const char* name,
                                     StageType type,
                                     unique_ptr<SpecificStats> specific,
                                     size_t works = 1,
                                     size_t advances = 1) {
    auto stats = make_unique<PlanStageStats>(name, type);
    stats->common.works = works;
    stats->common.advanced = advances;
    stats->specific = std::move(specific);
    return stats;
}

TEST(PlanRankerTest, NoFetchBonus) {
    // Two plans: one does a fetch, one does not. Assert the plan without the fetch has a higher
    // score. Note there is no projection involved: before SERVER-39241 was fixed we would give
    // these two plans the same score.

    auto goodPlan =
        makeStats("SHARDING_FILTER", STAGE_SHARDING_FILTER, make_unique<ShardingFilterStats>());
    goodPlan->children.emplace_back(
        makeStats("IXSCAN", STAGE_IXSCAN, make_unique<IndexScanStats>()));

    auto badPlan =
        makeStats("SHARDING_FILTER", STAGE_SHARDING_FILTER, make_unique<ShardingFilterStats>());
    badPlan->children.emplace_back(makeStats("FETCH", STAGE_FETCH, make_unique<FetchStats>()));
    badPlan->children[0]->children.emplace_back(
        makeStats("IXSCAN", STAGE_IXSCAN, make_unique<IndexScanStats>()));

    auto cq = makeCanonicalQuery();
    auto scorer = plan_ranker::makePlanScorer();
    auto goodScore = scorer->calculateScore(goodPlan.get(), *cq);
    auto badScore = scorer->calculateScore(badPlan.get(), *cq);

    ASSERT_GT(goodScore, badScore);
}

TEST(PlanRankerTest, DistinctBonus) {
    RAIIServerParameterControllerForTest shardFilteringDistinct(
        "featureFlagShardFilteringDistinctScan", true);

    // Two plans: both fetch, one is a DISTINCT_SCAN, other is an IXSCAN.
    // DISTINCT_SCAN does 2 advances / 10 works.
    auto dsStats = make_unique<DistinctScanStats>();
    dsStats->isFetching = true;
    dsStats->isShardFilteringDistinctScanEnabled = true;
    auto distinctScanPlan =
        makeStats("DISTINCT_SCAN", STAGE_DISTINCT_SCAN, std::move(dsStats), 10, 2);

    // IXSCAN plan does 2 advances / 10 works.
    auto ixscanPlan = makeStats("FETCH", STAGE_FETCH, make_unique<FetchStats>(), 10, 2);
    ixscanPlan->children.emplace_back(
        makeStats("IXSCAN", STAGE_IXSCAN, make_unique<IndexScanStats>(), 10, 2));

    auto cq = makeCanonicalQuery();
    cq->setDistinct(CanonicalDistinct("someKey"));
    auto scorer = plan_ranker::makePlanScorer();
    auto distinctScore = scorer->calculateScore(distinctScanPlan.get(), *cq);
    auto ixscanScore = scorer->calculateScore(ixscanPlan.get(), *cq);

    // Both plans should tie now- a tie-breaker will be applied at a later stage.
    ASSERT_EQ(distinctScore, ixscanScore);

    // Now we change to an aggregation context (simulate $groupByDistinct rewrite case).
    auto groupBson = BSON("$group" << BSON("_id" << "someKey"));
    cq->setCqPipeline(
        {DocumentSourceGroup::createFromBson(groupBson.firstElement(), cq->getExpCtx())}, true);

    // When in a distinct() context, productivity is considered larger in a distinct, even if both
    // plans have the same advances:work ratio. A DISTINCT_SCAN should now win by a large margin
    // (tie breaker).
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

plan_ranker::CandidatePlan makeCollScanCandidate() {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb.coll");
    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::make_unique<FetchNode>(std::make_unique<CollectionScanNode>(), nss));
    return plan_ranker::CandidatePlan{
        .solution = std::move(solution), .root = nullptr, .data = nullptr};
}

// Regression test: when a candidate plan fails during the multi-plan trial period, the surviving
// plans retain their original candidate indices, which may exceed the number of surviving plans.
// The tie-breaking heuristics must index the 'scores' and 'documentsExamined' vectors by candidate
// index rather than by a compacted survivor position; otherwise an out-of-bounds read occurs.
TEST(PlanRankerTest, TieBreakingWithFailedCandidateDoesNotReadOutOfBounds) {
    // Three candidate plans. Candidate 0 "failed" during the trial period and therefore does not
    // appear in 'scoresAndCandidateIndices'. Candidates 1 and 2 survived and tie in score.
    std::vector<plan_ranker::CandidatePlan> candidates;
    candidates.push_back(makeCollScanCandidate());
    candidates.push_back(makeCollScanCandidate());
    candidates.push_back(makeCollScanCandidate());

    // Surviving plans, sorted by score (tied), keyed by their original candidate index (1 and 2).
    std::vector<std::pair<double, size_t>> scoresAndCandidateIndices{{10.0, 1}, {10.0, 2}};

    // 'documentsExamined' is indexed by candidate index. Candidate 1 examined fewer documents than
    // candidate 2, so it should receive the docs-examined tie-breaking bonus.
    std::vector<size_t> documentsExamined{0, 5, 10};

    plan_ranker::addTieBreakingHeuristicsBonuses(
        scoresAndCandidateIndices, std::span(candidates), documentsExamined);

    // Candidate 1 (fewer docs examined) must now outscore candidate 2. Before the fix this either
    // crashed on debug/ASAN builds (out-of-bounds vector access) or applied the bonus to the wrong
    // plan on release builds.
    ASSERT_EQ(1, scoresAndCandidateIndices[0].second);
    ASSERT_EQ(2, scoresAndCandidateIndices[1].second);
    ASSERT_GT(scoresAndCandidateIndices[0].first, scoresAndCandidateIndices[1].first);
}
};  // namespace
