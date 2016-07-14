/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once


#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_solution.h"
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
class MultiPlanStage final : public PlanStage {
public:
    /**
     * Callers use this to specify how the MultiPlanStage should interact with the plan cache.
     */
    enum class CachingMode {
        // Always write a cache entry for the winning plan to the plan cache, overwriting any
        // previously existing cache entry for the query shape.
        AlwaysCache,

        // Write a cache entry for the query shape *unless* we encounter one of the following edge
        // cases:
        //  - Two or more plans tied for the win.
        //  - The winning plan returned zero query results during the plan ranking trial period.
        SometimesCache,

        // Do not write to the plan cache.
        NeverCache,
    };

    /**
     * Takes no ownership.
     *
     * If 'shouldCache' is true, writes a cache entry for the winning plan to the plan cache
     * when possible. If 'shouldCache' is false, the plan cache will never be written.
     */
    MultiPlanStage(OperationContext* txn,
                   const Collection* collection,
                   CanonicalQuery* cq,
                   CachingMode cachingMode = CachingMode::AlwaysCache);

    bool isEOF() final;

    StageState doWork(WorkingSetID* out) final;

    void doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) final;

    StageType stageType() const final {
        return STAGE_MULTI_PLAN;
    }

    std::unique_ptr<PlanStageStats> getStats() final;


    const SpecificStats* getSpecificStats() const final;

    /**
     * Takes ownership of QuerySolution and PlanStage. not of WorkingSet
     */
    void addPlan(QuerySolution* solution, PlanStage* root, WorkingSet* sharedWs);

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

    /**
     * Returns the number of times that we are willing to work a plan during a trial period.
     *
     * Calculated based on a fixed query knob and the size of the collection.
     */
    static size_t getTrialPeriodWorks(OperationContext* txn, const Collection* collection);

    /**
     * Returns the max number of documents which we should allow any plan to return during the
     * trial period. As soon as any plan hits this number of documents, the trial period ends.
     */
    static size_t getTrialPeriodNumToReturn(const CanonicalQuery& query);

    /** Return true if a best plan has been chosen  */
    bool bestPlanChosen() const;

    /** Return the index of the best plan chosen, for testing */
    int bestPlanIdx() const;

    /**
     * Returns the QuerySolution for the best plan, or NULL if no best plan
     *
     * The MultiPlanStage retains ownership of the winning QuerySolution and returns an
     * unowned pointer.
     */
    QuerySolution* bestSolution();

    /**
     * Returns true if a backup plan was picked.
     * This is the case when the best plan has a blocking stage.
     * Exposed for testing.
     */
    bool hasBackupPlan() const;

    //
    // Used by explain.
    //

    static const char* kStageType;

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
     * Returns a non-OK status if killed during a yield.
     */
    Status tryYield(PlanYieldPolicy* yieldPolicy);

    static const int kNoSuchPlan = -1;

    // Not owned here. Must be non-null.
    const Collection* _collection;

    // Describes the cases in which we should write an entry for the winning plan to the plan cache.
    const CachingMode _cachingMode;

    // The query that we're trying to figure out the best solution to.
    // not owned here
    CanonicalQuery* _query;

    // Candidate plans. Each candidate includes a child PlanStage tree and QuerySolution. Ownership
    // of all QuerySolutions is retained here, and will *not* be tranferred to the PlanExecutor that
    // wraps this stage. Ownership of the PlanStages will be in PlanStage::_children which maps
    // one-to-one with _candidates.
    std::vector<CandidatePlan> _candidates;

    // index into _candidates, of the winner of the plan competition
    // uses -1 / kNoSuchPlan when best plan is not (yet) known
    int _bestPlanIdx;

    // index into _candidates, of the backup plan for sort
    // uses -1 / kNoSuchPlan when best plan is not (yet) known
    int _backupPlanIdx;

    // Set if this MultiPlanStage cannot continue, and the query must fail. This can happen in
    // two ways. The first is that all candidate plans fail. Note that one plan can fail
    // during normal execution of the plan competition.  Here is an example:
    //
    // Plan 1: collection scan with sort.  Sort runs out of memory.
    // Plan 2: ixscan that provides sort.  Won't run out of memory.
    //
    // We want to choose plan 2 even if plan 1 fails.
    //
    // The second way for failure to occur is that the execution of this query is killed during
    // a yield, by some concurrent event such as a collection drop.
    bool _failure;

    // If everything fails during the plan competition, we can't pick one.
    size_t _failureCount;

    // if pickBestPlan fails, this is set to the wsid of the statusMember
    // returned by ::work()
    WorkingSetID _statusMemberId;

    // When a stage requests a yield for document fetch, it gives us back a RecordFetcher*
    // to use to pull the record into memory. We take ownership of the RecordFetcher here,
    // deleting it after we've had a chance to do the fetch. For timing-based yields, we
    // just pass a NULL fetcher.
    std::unique_ptr<RecordFetcher> _fetcher;

    // Stats
    MultiPlanStats _specificStats;
};

}  // namespace mongo
