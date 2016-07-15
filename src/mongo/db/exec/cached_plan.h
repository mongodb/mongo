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

#include <boost/scoped_ptr.hpp>
#include <list>

#include "mongo/db/jsobj.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_fetcher.h"

namespace mongo {

class PlanYieldPolicy;

/**
 * This stage outputs its mainChild, and possibly its backup child
 * and also updates the cache.
 *
 * Preconditions: Valid RecordId.
 *
 */
class CachedPlanStage : public PlanStage {
public:
    /**
     * Takes ownership of 'mainChild', 'mainQs', 'backupChild', and 'backupQs'.
     */
    CachedPlanStage(OperationContext* txn,
                    Collection* collection,
                    WorkingSet* ws,
                    CanonicalQuery* cq,
                    const QueryPlannerParams& params,
                    size_t decisionWorks,
                    PlanStage* mainChild,
                    QuerySolution* mainQs,
                    PlanStage* backupChild = NULL,
                    QuerySolution* backupQs = NULL);

    virtual ~CachedPlanStage();

    virtual bool isEOF();

    virtual StageState work(WorkingSetID* out);

    virtual void saveState();
    virtual void restoreState(OperationContext* opCtx);
    virtual void invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type);

    virtual std::vector<PlanStage*> getChildren() const;

    virtual StageType stageType() const {
        return STAGE_CACHED_PLAN;
    }

    virtual PlanStageStats* getStats();

    virtual const CommonStats* getCommonStats();

    virtual const SpecificStats* getSpecificStats();

    static const char* kStageType;

    void kill();

    /**
     * Runs the cached plan for a trial period, yielding during the trial period according to
     * 'yieldPolicy'.
     *
     * If the performance is lower than expected, the old plan is evicted and a new plan is
     * selected from scratch (again yielding according to 'yieldPolicy'). Otherwise, the cached
     * plan is run.
     */
    Status pickBestPlan(PlanYieldPolicy* yieldPolicy);

private:
    PlanStage* getActiveChild() const;
    void updateCache();

    /**
     * May yield during the cached plan stage's trial period or replanning phases.
     *
     * Returns a non-OK status if query planning fails. In particular, this function returns
     * ErrorCodes::QueryPlanKilled if the query plan was killed during a yield.
     */
    Status tryYield(PlanYieldPolicy* yieldPolicy);

    /**
     * Uses the QueryPlanner and the MultiPlanStage to re-generate candidate plans for this
     * query and select a new winner.
     *
     * We fallback to a new plan if, based on the number of works during the trial period that
     * put the plan in the cache, the performance was worse than anticipated during the trial
     * period.
     *
     * We only write the result of re-planning to the plan cache if 'shouldCache' is true.
     */
    Status replan(PlanYieldPolicy* yieldPolicy, bool shouldCache);

    // Not owned here.
    OperationContext* _txn;

    // Not owned here.
    Collection* _collection;

    // Not owned here.
    WorkingSet* _ws;

    // Not owned here.
    CanonicalQuery* _canonicalQuery;

    QueryPlannerParams _plannerParams;

    // Whether or not the cached plan trial period and replanning is enabled.
    const bool _replanningEnabled;

    // The number of work cycles taken to decide on a winning plan when the plan was first
    // cached.
    size_t _decisionWorks;

    // Owned by us. Must be deleted after the corresponding PlanStage trees, as
    // those trees point into the query solutions.
    boost::scoped_ptr<QuerySolution> _mainQs;
    boost::scoped_ptr<QuerySolution> _backupQs;

    // Owned by us. Must be deleted before the QuerySolutions above, as these
    // can point into the QuerySolutions.
    boost::scoped_ptr<PlanStage> _mainChildPlan;
    boost::scoped_ptr<PlanStage> _backupChildPlan;

    // True if the main plan errors before producing results
    // and if a backup plan is available (can happen with blocking sorts)
    bool _usingBackupChild;

    // True if the childPlan has produced results yet.
    bool _alreadyProduced;

    // Have we updated the cache with our plan stats yet?
    bool _updatedCache;

    // Has this query been killed?
    bool _killed;

    // Any results produced during trial period execution are kept here.
    std::list<WorkingSetID> _results;

    // When a stage requests a yield for document fetch, it gives us back a RecordFetcher*
    // to use to pull the record into memory. We take ownership of the RecordFetcher here,
    // deleting it after we've had a chance to do the fetch. For timing-based yields, we
    // just pass a NULL fetcher.
    boost::scoped_ptr<RecordFetcher> _fetcher;

    // Stats
    CommonStats _commonStats;
    CachedPlanStats _specificStats;
};

}  // namespace mongo
