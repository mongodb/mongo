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


#include "mongo/base/status.h"
#include "mongo/db/exec/classic/multi_plan_rate_limiter.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_collection_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/plan_yield_policy.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

extern FailPoint sleepWhileMultiplanning;

/**
 * A PlanStage for performing runtime plan selection. The caller is expected to construct a
 * 'MultiPlanStage', add candidate plans using the 'addPlan()' method, and then trigger runtime plan
 * selection by calling the 'pickBestPlan()' method.
 *
 * Partial result sets for each candidate are maintained in separate buffers. Once plan selection is
 * complete, the 'doWork()' method can be used to execute the winning plan. This will first unspool
 * the buffered results associated with the winning plan and then will continue execution of the
 * winning plan.
 */
class MultiPlanStage final : public RequiresCollectionStage {
public:
    static const char* kStageType;

    /**
     * Callback function which gets called from 'pickBestPlan()'. The 'PlanRankingDecision' and
     * vector of candidate plans describe the outcome of multi-planning.
     */
    using OnPickBestPlan = std::function<void(const CanonicalQuery&,
                                              MultiPlanStage& mps,
                                              std::unique_ptr<plan_ranker::PlanRankingDecision>,
                                              std::vector<plan_ranker::CandidatePlan>&)>;


    /**
     * Constructs a 'MultiPlanStage'.
     *
     * The 'onPickBestPlan()' callback is invoked once plan selection is complete, with parameters
     * passed that describe the result of multi-planning. This ensures that the 'MultiPlanStage'
     * does not interact with either the classic or SBE plan caches directly.
     */
    MultiPlanStage(ExpressionContext* expCtx,
                   VariantCollectionPtrOrAcquisition collection,
                   CanonicalQuery* cq,
                   OnPickBestPlan onPickBestPlan,
                   boost::optional<std::string> replanReason = boost::none);

    bool isEOF() const final;

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
     * Runs all plans added by addPlan(), ranks them, and picks a best plan. All further calls to
     * doWork() will return results from the best plan.
     *
     * If Multiplan rate limiting is enabled, the function attempts to obtain a token per candidate
     * plan to proceed with multiplanning. If not enough tokens are available, the function waits
     * until either a plan is cached or the tokens become available. Once unblocked, the function
     * returns a RetryMultiPlanning error, which leads to query replanning. Ideally, a cached plan
     * will be available, avoiding the need for multiplanning.
     *
     * If 'yieldPolicy' is non-null, then all locks may be yielded in between round-robin works of
     * the candidate plans. By default, 'yieldPolicy' is null and no yielding will take place.
     *
     * Returns a non-OK status if query planning fails. In particular, this function returns
     * ErrorCodes::QueryPlanKilled if the query plan was killed during a yield.
     */
    Status pickBestPlan(PlanYieldPolicy* yieldPolicy);

    /**
     * Returns true if a best plan has been chosen.
     */
    bool bestPlanChosen() const;

    /**
     * Returns the index of the best plan chosen, or boost::none if there is no such plan.
     */
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
     * Controls the number of threads concurrently multiplanning for the same shape. More details
     * can be found in multi_plan_rate_limiter.h.
     */
    MultiPlanTicket rateLimit(PlanYieldPolicy* yieldPolicy, size_t candidatesSize);

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

    // The query that we're trying to figure out the best solution to.
    // not owned here
    CanonicalQuery* _query;

    // Callback provided by the caller to invoke, passing the results of the plan selection trial
    // period.
    OnPickBestPlan _onPickBestPlan;

    // Candidate plans. Each candidate includes a child PlanStage tree and QuerySolution. Ownership
    // of all QuerySolutions is retained here, and will *not* be transferred to the PlanExecutor
    // that wraps this stage. Ownership of the PlanStages will be in PlanStage::_children which maps
    // one-to-one with _candidates.
    std::vector<plan_ranker::CandidatePlan> _candidates;

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
