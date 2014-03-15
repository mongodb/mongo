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

#include "mongo/db/query/multi_plan_runner.h"

#include <algorithm>
#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/db/catalog/collection.h"

namespace mongo {

    MultiPlanRunner::MultiPlanRunner(const Collection* collection, CanonicalQuery* query)
        : _collection(collection),
          _killed(false),
          _failure(false),
          _failureCount(0),
          _policy(Runner::YIELD_MANUAL),
          _query(query),
          _bestChild(numeric_limits<size_t>::max()),
          _backupSolution(NULL),
          _backupPlan(NULL) { }

    MultiPlanRunner::~MultiPlanRunner() {
        for (size_t i = 0; i < _candidates.size(); ++i) {
            delete _candidates[i].solution;
            delete _candidates[i].root;
            // ws must die after the root.
            delete _candidates[i].ws;
        }

        if (NULL != _backupSolution) {
            delete _backupSolution;
        }

        if (NULL != _backupPlan) {
            delete _backupPlan;
        }

        for (vector<PlanStageStats*>::iterator it = _candidateStats.begin();
             it != _candidateStats.end();
             ++it) {
            delete *it;
        }
    }

    void MultiPlanRunner::addPlan(QuerySolution* solution, PlanStage* root, WorkingSet* ws) {
        _candidates.push_back(CandidatePlan(solution, root, ws));
    }

    void MultiPlanRunner::setYieldPolicy(Runner::YieldPolicy policy) {
        if (_failure || _killed) { return; }

        _policy = policy;

        if (NULL != _bestPlan) {
            _bestPlan->setYieldPolicy(policy);
            if (NULL != _backupPlan) {
                _backupPlan->setYieldPolicy(policy);
            }
        } else {
            // Still running our candidates and doing our own yielding.
            if (Runner::YIELD_MANUAL == policy) {
                _yieldPolicy.reset();
            }
            else {
                _yieldPolicy.reset(new RunnerYieldPolicy());
            }
        }
    }

    void MultiPlanRunner::saveState() {
        if (_failure || _killed) { return; }

        if (NULL != _bestPlan) {
            _bestPlan->saveState();
            if (NULL != _backupPlan) {
                _backupPlan->saveState();
            }
        }
        else {
            allPlansSaveState();
        }
    }

    bool MultiPlanRunner::restoreState() {
        if (_failure || _killed) { return false; }

        if (NULL != _bestPlan) {
            return _bestPlan->restoreState();
            if (NULL != _backupPlan) {
                _backupPlan->restoreState();
            }
        }
        else {
            allPlansRestoreState();
            return true;
        }
    }

    void MultiPlanRunner::invalidate(const DiskLoc& dl, InvalidationType type) {
        if (_failure || _killed) { return; }

        if (NULL != _bestPlan) {
            _bestPlan->invalidate(dl, type);
            for (list<WorkingSetID>::iterator it = _alreadyProduced.begin();
                 it != _alreadyProduced.end();) {
                WorkingSetMember* member = _bestPlan->getWorkingSet()->get(*it);
                if (member->hasLoc() && member->loc == dl) {
                    list<WorkingSetID>::iterator next = it;
                    next++;
                    WorkingSetCommon::fetchAndInvalidateLoc(member);
                    _bestPlan->getWorkingSet()->flagForReview(*it);
                    _alreadyProduced.erase(it);
                    it = next;
                }
                else {
                    it++;
                }
            }
            if (NULL != _backupPlan) {
                _backupPlan->invalidate(dl, type);
                for (list<WorkingSetID>::iterator it = _backupAlreadyProduced.begin();
                        it != _backupAlreadyProduced.end();) {
                    WorkingSetMember* member = _backupPlan->getWorkingSet()->get(*it);
                    if (member->hasLoc() && member->loc == dl) {
                        list<WorkingSetID>::iterator next = it;
                        next++;
                        WorkingSetCommon::fetchAndInvalidateLoc(member);
                        _backupPlan->getWorkingSet()->flagForReview(*it);
                        _backupAlreadyProduced.erase(it);
                        it = next;
                    }
                    else {
                        it++;
                    }
                }
            }
        }
        else {
            for (size_t i = 0; i < _candidates.size(); ++i) {
                _candidates[i].root->invalidate(dl, type);
                for (list<WorkingSetID>::iterator it = _candidates[i].results.begin();
                     it != _candidates[i].results.end();) {
                    WorkingSetMember* member = _candidates[i].ws->get(*it);
                    if (member->hasLoc() && member->loc == dl) {
                        list<WorkingSetID>::iterator next = it;
                        next++;
                        WorkingSetCommon::fetchAndInvalidateLoc(member);
                        _candidates[i].ws->flagForReview(*it);
                        _candidates[i].results.erase(it);
                        it = next;
                    }
                    else {
                        it++;
                    }
                }
            }
        }
    }

    bool MultiPlanRunner::isEOF() {
        if (_failure || _killed) { return true; }
        // If _bestPlan is not NULL, you haven't picked the best plan yet, so you're not EOF.
        if (NULL == _bestPlan) { return false; }
        // We must return all our cached results and there must be no results from the best plan.
        return _alreadyProduced.empty() && _bestPlan->isEOF();
    }

    const std::string& MultiPlanRunner::ns() {
        return _query->getParsed().ns();
    }

    void MultiPlanRunner::kill() {
        _killed = true;
        _collection = NULL;
        if (NULL != _bestPlan) { _bestPlan->kill(); }
    }

    Runner::RunnerState MultiPlanRunner::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        if (_killed) { return Runner::RUNNER_DEAD; }
        if (_failure) { return Runner::RUNNER_ERROR; }

        // If we haven't picked the best plan yet...
        if (NULL == _bestPlan) {
            if (!pickBestPlan(NULL, objOut)) {
                verify(_failure || _killed);
                if (_killed) { return Runner::RUNNER_DEAD; }
                if (_failure) { return Runner::RUNNER_ERROR; }
            }
            cacheBestPlan();
        }

        // Look for an already produced result that provides the data the caller wants.
        while (!_alreadyProduced.empty()) {
            WorkingSetID id = _alreadyProduced.front();
            _alreadyProduced.pop_front();

            WorkingSetMember* member = _bestPlan->getWorkingSet()->get(id);

            // Note that this copies code from PlanExecutor.
            if (NULL != objOut) {
                if (WorkingSetMember::LOC_AND_IDX == member->state) {
                    if (1 != member->keyData.size()) {
                        _bestPlan->getWorkingSet()->free(id);
                        // If the caller needs the key data and the WSM doesn't have it, drop the
                        // result and carry on.
                        continue;
                    }
                    *objOut = member->keyData[0].keyData;
                }
                else if (member->hasObj()) {
                    *objOut = member->obj;
                }
                else {
                    // If the caller needs an object and the WSM doesn't have it, drop and
                    // try the next result.
                    _bestPlan->getWorkingSet()->free(id);
                    continue;
                }
            }

            if (NULL != dlOut) {
                if (member->hasLoc()) {
                    *dlOut = member->loc;
                }
                else {
                    // If the caller needs a DiskLoc and the WSM doesn't have it, drop and carry on.
                    _bestPlan->getWorkingSet()->free(id);
                    continue;
                }
            }

            // If we're here, the caller has all the data needed and we've set the out
            // parameters.  Remove the result from the WorkingSet.
            _bestPlan->getWorkingSet()->free(id);
            return Runner::RUNNER_ADVANCED;
        }

        RunnerState state = _bestPlan->getNext(objOut, dlOut);

        if (Runner::RUNNER_ERROR == state && (NULL != _backupSolution)) {
            QLOG() << "Best plan errored out; switching to backup.\n";
            // Uncache the bad solution if we fall back
            // on the backup solution.
            //
            // XXX: Instead of uncaching we should find a way for the
            // cached plan runner to fall back on a different solution
            // if the best solution fails. Alternatively we could try to
            // defer cache insertion to be after the first produced result.
            Database* db = cc().database();
            verify(NULL != db);
            Collection* collection = db->getCollection(_query->ns());
            verify(NULL != collection);
            PlanCache* cache = collection->infoCache()->getPlanCache();
            cache->remove(*_query);

            _bestPlan.reset(_backupPlan);
            _backupPlan = NULL;
            _bestSolution.reset(_backupSolution);
            _backupSolution = NULL;
            _alreadyProduced = _backupAlreadyProduced;
            return getNext(objOut, dlOut);
        }

        if (NULL != _backupSolution && Runner::RUNNER_ADVANCED == state) {
            QLOG() << "Best plan had a blocking sort, became unblocked; deleting backup plan.\n";
            delete _backupSolution;
            delete _backupPlan;
            _backupSolution = NULL;
            _backupPlan = NULL;
            // TODO: free from WS?
            _backupAlreadyProduced.clear();
        }

        return state;
    }

    bool MultiPlanRunner::pickBestPlan(size_t* out, BSONObj* objOut) {
        // Run each plan some number of times. This number is at least as great as
        // 'internalQueryPlanEvaluationWorks', but may be larger for big collections.
        size_t numWorks = internalQueryPlanEvaluationWorks;
        if (NULL != _collection) {
            // For large collections, the number of works is set to be this
            // fraction of the collection size.
            double fraction = internalQueryPlanEvaluationCollFraction;

            numWorks = std::max(size_t(internalQueryPlanEvaluationWorks),
                                size_t(fraction * _collection->numRecords()));
        }

        // Work the plans, stopping when a plan hits EOF or returns some
        // fixed number of results.
        for (size_t i = 0; i < numWorks; ++i) {
            bool moreToDo = workAllPlans(objOut);
            if (!moreToDo) { break; }
        }

        if (_failure || _killed) { return false; }

        // After picking best plan, ranking will own plan stats from
        // candidate solutions (winner and losers).
        _ranking.reset(new PlanRankingDecision());
        _bestChild = PlanRanker::pickBestPlan(_candidates, _ranking.get());
        if (NULL != out) { *out = _bestChild; }
        return true;
    }

    void MultiPlanRunner::cacheBestPlan() {
        // Must call pickBestPlan before.
        verify(_bestChild != numeric_limits<size_t>::max());

        // Copy candidate order. We will need this to sort candidate stats for explain
        // after transferring ownership of 'ranking' to plan cache.
        std::vector<size_t> candidateOrder = _ranking->candidateOrder;

        // Run the best plan.  Store it.
        _bestPlan.reset(new PlanExecutor(_candidates[_bestChild].ws,
                                         _candidates[_bestChild].root));
        _bestPlan->setYieldPolicy(_policy);
        _alreadyProduced = _candidates[_bestChild].results;
        _bestSolution.reset(_candidates[_bestChild].solution);

        QLOG() << "Winning solution:\n" << _bestSolution->toString() << endl;
        LOG(2) << "Winning plan: " << getPlanSummary(*_bestSolution);

        size_t backupChild = _bestChild;
        if (_bestSolution->hasBlockingStage && (0 == _alreadyProduced.size())) {
            QLOG() << "Winner has blocking stage, looking for backup plan.\n";
            for (size_t i = 0; i < _candidates.size(); ++i) {
                if (!_candidates[i].solution->hasBlockingStage) {
                    QLOG() << "Candidate " << i << " is backup child.\n";
                    backupChild = i;
                    _backupSolution = _candidates[i].solution;
                    _backupAlreadyProduced = _candidates[i].results;
                    _backupPlan = new PlanExecutor(_candidates[i].ws, _candidates[i].root);
                    _backupPlan->setYieldPolicy(_policy);
                    break;
                }
            }
        }

        // Store the choice we just made in the cache. We do
        // not cache the query if:
        //   1) The query is of a type that is not safe to cache, or
        //   2) the winning plan did not actually produce any results,
        //   without hitting EOF. In this case, we have no information to
        //   suggest that this plan is good.
        const PlanStageStats* bestStats = _ranking->stats.vector()[0];
        if (PlanCache::shouldCacheQuery(*_query)
            && (!_alreadyProduced.empty() || bestStats->common.isEOF)) {
            Database* db = cc().database();
            verify(NULL != db);
            Collection* collection = db->getCollection(_query->ns());
            verify(NULL != collection);
            PlanCache* cache = collection->infoCache()->getPlanCache();
            // Create list of candidate solutions for the cache with
            // the best solution at the front.
            std::vector<QuerySolution*> solutions;

            // Generate solutions and ranking decisions sorted by score.
            for (size_t orderingIndex = 0;
                 orderingIndex < candidateOrder.size(); ++orderingIndex) {
                // index into candidates/ranking
                size_t i = candidateOrder[orderingIndex];
                solutions.push_back(_candidates[i].solution);
            }

            // Check solution cache data. Do not add to cache if
            // we have any invalid SolutionCacheData data.
            // XXX: One known example is 2D queries
            bool validSolutions = true;
            for (size_t i = 0; i < solutions.size(); ++i) {
                if (NULL == solutions[i]->cacheData.get()) {
                    QLOG() << "Not caching query because this solution has no cache data: "
                           << solutions[i]->toString();
                    validSolutions = false;
                    break;
                }
            }

            if (validSolutions) {
                cache->add(*_query, solutions, _ranking.release());
            }
        }

        // Clear out the candidate plans, leaving only stats as we're all done w/them.
        // Traverse candidate plans in order or score
        for (size_t orderingIndex = 0;
             orderingIndex < candidateOrder.size(); ++orderingIndex) {
            // index into candidates/ranking
            size_t i = candidateOrder[orderingIndex];

            if (i == _bestChild) { continue; }
            if (i == backupChild) { continue; }

            delete _candidates[i].solution;

            // Remember the stats for the candidate plan because we always show it on an
            // explain. (The {verbose:false} in explain() is client-side trick; we always
            // generate a "verbose" explain.)
            PlanStageStats* stats = _candidates[i].root->getStats();
            if (stats) {
                _candidateStats.push_back(stats);
            }
            delete _candidates[i].root;

            // ws must die after the root.
            delete _candidates[i].ws;
        }

        _candidates.clear();
    }

    bool MultiPlanRunner::hasBackupPlan() const {
        return NULL != _backupPlan;
    }

    bool MultiPlanRunner::workAllPlans(BSONObj* objOut) {
        bool doneWorking = false;

        for (size_t i = 0; i < _candidates.size(); ++i) {
            CandidatePlan& candidate = _candidates[i];
            if (candidate.failed) { continue; }

            // Yield, if we can yield ourselves.
            if (NULL != _yieldPolicy.get() && _yieldPolicy->shouldYield()) {
                saveState();
                _yieldPolicy->yield();
                if (_failure || _killed) { return false; }
                restoreState();
            }

            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = candidate.root->work(&id);

            if (PlanStage::ADVANCED == state) {
                // Save result for later.
                candidate.results.push_back(id);

                // Once a plan returns enough results, stop working.
                if (candidate.results.size()
                    >= size_t(internalQueryPlanEvaluationMaxResults)) {
                    doneWorking = true;
                }
            }
            else if (PlanStage::NEED_TIME == state) {
                // Fall through to yield check at end of large conditional.
            }
            else if (PlanStage::NEED_FETCH == state) {
                // id has a loc and refers to an obj we need to fetch.
                WorkingSetMember* member = candidate.ws->get(id);

                // This must be true for somebody to request a fetch and can only change when an
                // invalidation happens, which is when we give up a lock.  Don't give up the
                // lock between receiving the NEED_FETCH and actually fetching(?).
                verify(member->hasLoc());

                // Actually bring record into memory.
                Record* record = member->loc.rec();

                // If we're allowed to, go to disk outside of the lock.
                if (NULL != _yieldPolicy.get()) {
                    saveState();
                    _yieldPolicy->yield(record);
                    if (_failure || _killed) { return false; }
                    restoreState();
                }
                else {
                    // We're set to manually yield.  We go to disk in the lock.
                    record->touch();
                }

                // Record should be in memory now.  Log if it's not.
                if (!Record::likelyInPhysicalMemory(record->dataNoThrowing())) {
                    OCCASIONALLY {
                        warning() << "Record wasn't in memory immediately after fetch: "
                            << member->loc.toString() << endl;
                    }
                }

                // Note that we're not freeing id.  Fetch semantics say that we shouldn't.
            }
            else if (PlanStage::IS_EOF == state) {
                // First plan to hit EOF wins automatically.  Stop evaluating other plans.
                // Assumes that the ranking will pick this plan.
                doneWorking = true;
            }
            else {
                // FAILURE or DEAD.  Do we want to just tank that plan and try the rest?  We
                // probably want to fail globally as this shouldn't happen anyway.

                candidate.failed = true;
                ++_failureCount;

                // Propage most recent seen failure to parent.
                if (PlanStage::FAILURE == state && (NULL != objOut)) {
                    WorkingSetCommon::getStatusMemberObject(*candidate.ws, id, objOut);
                }

                if (_failureCount == _candidates.size()) {
                    _failure = true;
                    return false;
                }
            }
        }

        return !doneWorking;
    }

    void MultiPlanRunner::allPlansSaveState() {
        for (size_t i = 0; i < _candidates.size(); ++i) {
            _candidates[i].root->prepareToYield();
        }
    }

    void MultiPlanRunner::allPlansRestoreState() {
        for (size_t i = 0; i < _candidates.size(); ++i) {
            _candidates[i].root->recoverFromYield();
        }
    }

    Status MultiPlanRunner::getInfo(TypeExplain** explain,
                                    PlanInfo** planInfo) const {
        if (NULL != explain) {
            if (NULL == _bestPlan.get()) {
                return Status(ErrorCodes::InternalError, "No plan available to provide stats");
            }

            //
            // Explain for the winner plan
            //

            scoped_ptr<PlanStageStats> stats(_bestPlan->getStats());
            if (NULL == stats.get()) {
                return Status(ErrorCodes::InternalError, "no stats available to explain plan");
            }

            return explainMultiPlan(*stats, _candidateStats, _bestSolution.get(), explain);
        }
        else if (NULL != planInfo) {
            if (NULL == _bestSolution.get()) {
                return Status(ErrorCodes::InternalError,
                              "no best solution available for plan info");
            }
            getPlanInfo(*_bestSolution, planInfo);
        }

        return Status::OK();
    }

}  // namespace mongo
