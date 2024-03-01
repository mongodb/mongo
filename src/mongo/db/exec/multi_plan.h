/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once


#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/requires_collection_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_enumerator/plan_enumerator_explain_info.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/record_id.h"

namespace mongo {

/**
 * This stage outputs its mainChild, and possibly it's backup child
 * and also updates the cache.
 *
 * Preconditions: Valid RecordId.
 *
 * Owns the query solutions and PlanStage roots for all candidate plans.
 */
class MultiPlanStage final : public RequiresCollectionStage {
public:
    static const char* kStageType;

    /**
     * Takes no ownership.
     *
     * If 'shouldCache' is true, writes a cache entry for the winning plan to the plan cache
     * when possible. If 'shouldCache' is false, the plan cache will never be written.
     */
    MultiPlanStage(ExpressionContext* expCtx,
                   VariantCollectionPtrOrAcquisition collection,
                   CanonicalQuery* cq,
                   PlanCachingMode cachingMode = PlanCachingMode::AlwaysCache);

    bool isEOF() final;

    StageState doWork(WorkingSetID* out) final;

    StageType stageType() const final {
        return STAGE_MULTI_PLAN;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    const plan_ranker::CandidatePlan& getCandidate(size_t candidateIdx) const;
    boost::optional<double> getCandidateScore(size_t candidateIdx) const;

    /**
     * Adds a new candidate plan to be considered for selection by the MultiPlanStage trial period.
     */
    void addPlan(std::unique_ptr<QuerySolution> solution,
                 std::unique_ptr<PlanStage> root,
                 WorkingSet* sharedWs);

    /**
     * Runs all plans added by addPlan, ranks them, and picks a best.
     * All further calls to work(...) will return results from the best plan.
     *
     * If 'yieldPolicy' is non-NULL, then all locks may be yielded in between round-robin
     * works of the candidate plans. By default, 'yieldPolicy' is NULL and no yielding will
     * take place.
     *
     * Returns a non-OK status if query planning fails. In particular, this function returns
     * ErrorCodes::QueryPlanKilled if the query plan was killed during a yield.
     */
    Status pickBestPlan(PlanYieldPolicy* yieldPolicy);

    /** Return true if a best plan has been chosen  */
    bool bestPlanChosen() const;

    /** Return the index of the best plan chosen, or boost::none if there is no such plan. */
    boost::optional<size_t> bestPlanIdx() const;

    /**
     * Returns the QuerySolution for the best plan, or NULL if no best plan.
     *
     * The MultiPlanStage retains ownership of the winning QuerySolution and returns an
     * unowned pointer.
     */
    const QuerySolution* bestSolution() const;

    /**
     * Returns the QuerySolution for the best plan, or NULL if no best plan.
     *
     * The MultiPlanStage does not retain ownership of the winning QuerySolution and returns
     * a unique pointer.
     */
    std::unique_ptr<QuerySolution> extractBestSolution();

    /**
     * Returns true if the winning plan reached EOF during its trial period and false otherwise.
     * Illegal to call if the best plan has not yet been selected.
     */
    bool bestSolutionEof() const;

    /**
     * Returns the PlanRankingDecision which captures scoring information from the trial period.
     * Requires caching mode to be NeverCache and calling pickBestPlan() beforehand.
     */
    const plan_ranker::PlanRankingDecision& planRankingDecision() const {
        tassert(8524800,
                "The caching mode must be NeverCache to ensure planRankingDecision() is invoked by "
                "Classic runtime planner for SBE",
                _cachingMode == PlanCachingMode::NeverCache);
        tassert(8524801, "Ranking decision must be determined by calling pickBestPlan()", _ranking);
        return *_ranking;
    }

    /**
     * Returns the candidate plans. Each candidate plan includes a child PlanStage tree and
     * QuerySolution.
     */
    const std::vector<plan_ranker::CandidatePlan>& candidates() const {
        return _candidates;
    }

    /**
     * Returns true if a backup plan was picked.
     * This is the case when the best plan has a blocking stage.
     * Exposed for testing.
     */
    bool hasBackupPlan() const;

protected:
    void doSaveStateRequiresCollection() final {}

    void doRestoreStateRequiresCollection() final {}

private:
    //
    // Have all our candidate plans do something.
    // If all our candidate plans fail, *objOut will contain
    // information on the failure.
    //

    /**
     * Calls work on each child plan in a round-robin fashion. We stop when any plan hits EOF
     * or returns 'numResults' results.
     *
     * Returns true if we need to keep working the plans and false otherwise.
     */
    bool workAllPlans(size_t numResults, PlanYieldPolicy* yieldPolicy);

    /**
     * Checks whether we need to perform either a timing-based yield or a yield for a document
     * fetch. If so, then uses 'yieldPolicy' to actually perform the yield.
     *
     * Throws an exception if yield recovery fails.
     */
    void tryYield(PlanYieldPolicy* yieldPolicy);

    /**
     * Deletes all children, except for best and backup plans.
     *
     * This is necessary to release any resources that rejected plans might have.
     * For example, if multi-update can be done by scanning several indexes,
     * it will be slowed down by rejected index scans because of index cursors
     * that need to be reopeneed after every update.
     */
    void removeRejectedPlans();
    void rejectPlan(size_t planIdx);
    void switchToBackupPlan();
    void removeBackupPlan();

    static const int kNoSuchPlan = -1;

    // Describes the cases in which we should write an entry for the winning plan to the plan cache.
    const PlanCachingMode _cachingMode;

    // The query that we're trying to figure out the best solution to.
    // not owned here
    CanonicalQuery* _query;

    // Candidate plans. Each candidate includes a child PlanStage tree and QuerySolution. Ownership
    // of all QuerySolutions is retained here, and will *not* be tranferred to the PlanExecutor that
    // wraps this stage. Ownership of the PlanStages will be in PlanStage::_children which maps
    // one-to-one with _candidates.
    std::vector<plan_ranker::CandidatePlan> _candidates;

    // Captures scoring information from the trial period. Nullptr until 'pickBestPlan()' returns
    // with an OK status. Requires '_cachingMode' to be NeverCache to retain the ownership.
    std::unique_ptr<plan_ranker::PlanRankingDecision> _ranking;

    // Rejected plans in saved and detached state.
    std::vector<std::unique_ptr<PlanStage>> _rejected;

    // index into _candidates, of the winner of the plan competition
    // uses -1 / kNoSuchPlan when best plan is not (yet) known
    int _bestPlanIdx;

    // Because best solution may be "extracted", we need to cache best plan score for explain.
    boost::optional<double> _bestPlanScore;

    // index into _candidates, of the backup plan for sort
    // uses -1 / kNoSuchPlan when best plan is not (yet) known
    int _backupPlanIdx;

    // Count of the number of candidate plans that have failed during the trial period. The
    // multi-planner swallows resource exhaustion errors (QueryExceededMemoryLimitNoDiskUseAllowed).
    // This means that if one candidate involves a blocking sort, and the other does not, the entire
    // query will not fail if the blocking sort hits the limit on its allowed memory footprint.
    //
    // Arbitrary error codes are not swallowed by the multi-planner, since it is not know whether it
    // is safe for the query to continue executing.
    size_t _failureCount = 0u;

    // Stats
    MultiPlanStats _specificStats;
};

}  // namespace mongo
