/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/query/cached_plan_runner.h"

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/db/catalog/collection.h"

namespace mongo {

    CachedPlanRunner::CachedPlanRunner(CanonicalQuery* canonicalQuery,
                                       QuerySolution* solution,
                                       PlanStage* root,
                                       WorkingSet* ws)
        : _canonicalQuery(canonicalQuery),
          _solution(solution),
          _exec(new PlanExecutor(ws, root)),
          _alreadyProduced(false),
          _updatedCache(false) { }

    CachedPlanRunner::~CachedPlanRunner() { }

    Runner::RunnerState CachedPlanRunner::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        Runner::RunnerState state = _exec->getNext(objOut, dlOut);

        if (Runner::RUNNER_ADVANCED == state) {
            // Indicate that the plan executor already produced results.
            _alreadyProduced = true;
        }

        // If the plan executor errors before producing any results,
        // and we have a backup plan available, then fall back on the
        // backup plan. This can happen if '_exec' has a blocking sort.
        if (Runner::RUNNER_ERROR == state && !_alreadyProduced && NULL != _backupPlan.get()) {
            _exec.reset(_backupPlan.release());
            state = _exec->getNext(objOut, dlOut);
        }

        // This could be called several times and we don't want to update the cache every time.
        if (Runner::RUNNER_EOF == state && !_updatedCache) {
            updateCache();
        }

        return state;
    }

    bool CachedPlanRunner::isEOF() {
        return _exec->isEOF();
    }

    void CachedPlanRunner::saveState() {
        _exec->saveState();
        if (NULL != _backupPlan.get()) {
            _backupPlan->saveState();
        }
    }

    bool CachedPlanRunner::restoreState() {
        if (NULL != _backupPlan.get()) {
            _backupPlan->restoreState();
        }
        return _exec->restoreState();
    }

    void CachedPlanRunner::invalidate(const DiskLoc& dl, InvalidationType type) {
        _exec->invalidate(dl, type);
        if (NULL != _backupPlan.get()) {
            _backupPlan->invalidate(dl, type);
        }
    }

    void CachedPlanRunner::setYieldPolicy(Runner::YieldPolicy policy) {
        _exec->setYieldPolicy(policy);
        if (NULL != _backupPlan.get()) {
            _backupPlan->setYieldPolicy(policy);
        }
    }

    const std::string& CachedPlanRunner::ns() {
        return _canonicalQuery->getParsed().ns();
    }

    void CachedPlanRunner::kill() {
        _exec->kill();
        if (NULL != _backupPlan.get()) {
            _backupPlan->kill();
        }
    }

    Status CachedPlanRunner::getExplainPlan(TypeExplain** explain) const {
        verify(_exec.get());

        scoped_ptr<PlanStageStats> stats(_exec->getStats());
        if (NULL == stats.get()) {
            return Status(ErrorCodes::InternalError, "no stats available to explain plan");
        }

        Status status = explainPlan(*stats, explain, true /* full details */);
        if (!status.isOK()) {
            return status;
        }

        // Fill in explain fields that are accounted by on the runner level.
        TypeExplain* chosenPlan = NULL;
        explainPlan(*stats, &chosenPlan, false /* no full details */);
        if (chosenPlan) {
            (*explain)->addToAllPlans(chosenPlan);
        }
        (*explain)->setNScannedObjectsAllPlans((*explain)->getNScannedObjects());
        (*explain)->setNScannedAllPlans((*explain)->getNScanned());

        return Status::OK();
    }

    void CachedPlanRunner::updateCache() {
        _updatedCache = true;
#if 0
        Database* db = cc().database();
        verify(NULL != db);
        Collection* collection = db->getCollection(_canonicalQuery->ns());
        verify(NULL != collection);
        PlanCache* cache = collection->infoCache()->getPlanCache();

        auto_ptr<CacheEntryFeedback> feedback(new CacheEntryFeedback());
        // XXX: what else can we provide here?
        feedback->stats.reset(_exec->getStats());
        Status fbs = cache->feedback(_solution->key, feedback.release());

        if (!fbs.isOK()) {
            // XXX: probably not a warning, could happen.
            warning() << "Failed to update cache: " << fbs.toString() << endl;
        }
#endif
    }

    void CachedPlanRunner::setBackupPlan(QuerySolution* qs, PlanStage* root, WorkingSet* ws) {
        _backupSolution.reset(qs);
        _backupPlan.reset(new PlanExecutor(ws, root));
    }

} // namespace mongo
