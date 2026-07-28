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

#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/plan_ranker_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

using namespace mongo;

namespace {

using std::make_unique;
using std::string;
using std::unique_ptr;
using std::vector;

unique_ptr<PlanStageStats> makeStats(const char* name,
                                     StageType type,
                                     unique_ptr<SpecificStats> specific) {
    auto stats = make_unique<PlanStageStats>(name, type);
    stats->common.works = 1;
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
    badPlan->children.emplace_back(makeStats("FETCH", STAGE_FETCH, make_unique<IndexScanStats>()));
    badPlan->children[0]->children.emplace_back(
        makeStats("IXSCAN", STAGE_IXSCAN, make_unique<IndexScanStats>()));

    auto scorer = plan_ranker::makePlanScorer();
    auto goodScore = scorer->calculateScore(goodPlan.get());
    auto badScore = scorer->calculateScore(badPlan.get());

    ASSERT_GT(goodScore, badScore);
}

plan_ranker::CandidatePlan makeCollScanCandidate() {
    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::make_unique<FetchNode>(std::make_unique<CollectionScanNode>()));
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
        scoresAndCandidateIndices, candidates, documentsExamined);

    // Candidate 1 (fewer docs examined) must now outscore candidate 2. Before the fix this either
    // crashed on debug/ASAN builds (out-of-bounds vector access) or applied the bonus to the wrong
    // plan on release builds.
    ASSERT_EQ(1, scoresAndCandidateIndices[0].second);
    ASSERT_EQ(2, scoresAndCandidateIndices[1].second);
    ASSERT_GT(scoresAndCandidateIndices[0].first, scoresAndCandidateIndices[1].first);
}
};  // namespace
