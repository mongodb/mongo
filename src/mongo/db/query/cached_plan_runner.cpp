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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/type_explain.h"

namespace mongo {

    CachedPlanRunner::CachedPlanRunner(const Collection* collection,
                                       CanonicalQuery* canonicalQuery,
                                       QuerySolution* solution,
                                       PlanStage* root,
                                       WorkingSet* ws)
        : _collection(collection),
          _canonicalQuery(canonicalQuery),
          _solution(solution),
          _exec(new PlanExecutor(ws, root)),
          _alreadyProduced(false),
          _updatedCache(false),
          _killed(false) { }

    CachedPlanRunner::~CachedPlanRunner() {
        // The runner may produce all necessary results without hitting EOF.  In this case, we still
        // want to update the cache with feedback.
        if (!_updatedCache) {
            updateCache();
        }
    }

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
        _killed = true;
        _collection = NULL;
        _exec->kill();
        if (NULL != _backupPlan.get()) {
            _backupPlan->kill();
        }
    }

    Status CachedPlanRunner::getInfo(TypeExplain** explain,
                                     PlanInfo** planInfo) const {
        if (NULL != explain) {
            if (NULL == _exec.get()) {
                return Status(ErrorCodes::InternalError, "No plan available to provide stats");
            }

            //
            // Explain for the winner plan
            //

            scoped_ptr<PlanStageStats> stats(_exec->getStats());
            if (NULL == stats.get()) {
                return Status(ErrorCodes::InternalError, "no stats available to explain plan");
            }

            // Alternate plans not needed for explainMultiPlain.
            // We don't bother showing the alternative plans because their bounds
            // may not match the bounds of the query that we're running.  If a user
            // is explaining a query that could have been executed in >1 way it
            // won't use a cached plan runner for that reason
            // getInfo is used to generate explain info for system.profile and
            // slow query logging only.
            // User visible explain output is generated by multi plan runner's getInfo().
            std::vector<PlanStageStats*> emptyStats;
            return explainMultiPlan(*stats, emptyStats, _solution.get(), explain);
        }
        else if (NULL != planInfo) {
            if (NULL == _solution.get()) {
                return Status(ErrorCodes::InternalError,
                              "no best solution available for plan info");
            }
            getPlanInfo(*_solution, planInfo);
        }

        return Status::OK();
    }

    void CachedPlanRunner::updateCache() {
        _updatedCache = true;

        if (_killed) {
            return;
        }

        Database* db = cc().database();

        // We need to check db and collection for NULL because updateCache() is called upon destruction of
        // the CachedPlanRunner. In some cases, the db or collection could be dropped without kill()
        // being called on the runner (for example, timeout of a ClientCursor holding the runner).
        if (NULL == db) { return; }
        Collection* collection = db->getCollection(_canonicalQuery->ns());
        if (NULL == collection) { return; }
        PlanCache* cache = collection->infoCache()->getPlanCache();

        std::auto_ptr<PlanCacheEntryFeedback> feedback(new PlanCacheEntryFeedback());
        feedback->stats.reset(_exec->getStats());
        feedback->score = PlanRanker::scoreTree(feedback->stats.get());

        Status fbs = cache->feedback(*_canonicalQuery, feedback.release());

        if (!fbs.isOK()) {
            QLOG() << _canonicalQuery->ns() << ": Failed to update cache with feedback: "
                   << fbs.toString() << " - "
                   << "(query: " << _canonicalQuery->getQueryObj()
                   << "; sort: " << _canonicalQuery->getParsed().getSort()
                   << "; projection: " << _canonicalQuery->getParsed().getProj()
                   << ") is no longer in plan cache.";
        }
    }

    void CachedPlanRunner::setBackupPlan(QuerySolution* qs, PlanStage* root, WorkingSet* ws) {
        _backupSolution.reset(qs);
        _backupPlan.reset(new PlanExecutor(ws, root));
    }

} // namespace mongo
