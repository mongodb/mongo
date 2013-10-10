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

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/type_explain.h"

namespace mongo {

    MultiPlanRunner::MultiPlanRunner(CanonicalQuery* query)
        : _killed(false), _failure(false), _failureCount(0), _policy(Runner::YIELD_MANUAL),
          _query(query) { }

    MultiPlanRunner::~MultiPlanRunner() {
        for (size_t i = 0; i < _candidates.size(); ++i) {
            delete _candidates[i].solution;
            delete _candidates[i].root;
            // ws must die after the root.
            delete _candidates[i].ws;
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
        }
        else {
            allPlansSaveState();
        }
    }

    bool MultiPlanRunner::restoreState() {
        if (_failure || _killed) { return false; }

        if (NULL != _bestPlan) {
            return _bestPlan->restoreState();
        }
        else {
            allPlansRestoreState();
            return true;
        }
    }

    void MultiPlanRunner::invalidate(const DiskLoc& dl) {
        if (_failure || _killed) { return; }

        if (NULL != _bestPlan) {
            _bestPlan->invalidate(dl);
            for (deque<WorkingSetID>::iterator it = _alreadyProduced.begin();
                 it != _alreadyProduced.end(); ++it) {
                WorkingSetMember* member = _bestPlan->getWorkingSet()->get(*it);
                if (member->hasLoc() && member->loc == dl) {
                    WorkingSetCommon::fetchAndInvalidateLoc(member);
                }
            }
        }
        else {
            for (size_t i = 0; i < _candidates.size(); ++i) {
                _candidates[i].root->invalidate(dl);
                for (deque<WorkingSetID>::iterator it = _candidates[i].results.begin();
                     it != _candidates[i].results.end(); ++it) {
                    WorkingSetMember* member = _candidates[i].ws->get(*it);
                    if (member->hasLoc() && member->loc == dl) {
                        WorkingSetCommon::fetchAndInvalidateLoc(member);
                    }
                }
            }
        }
    }

    bool MultiPlanRunner::isEOF() {
        if (_failure || _killed) { return true; }
        // If _bestPlan is not NULL, you haven't picked the best plan yet, so you're not EOF.
        if (NULL == _bestPlan) { return false; }
        return _bestPlan->isEOF();
    }

    const std::string& MultiPlanRunner::ns() {
        return _query->getParsed().ns();
    }

    void MultiPlanRunner::kill() {
        _killed = true;
        if (NULL != _bestPlan) { _bestPlan->kill(); }
    }

    Runner::RunnerState MultiPlanRunner::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        if (_killed) { return Runner::RUNNER_DEAD; }
        if (_failure) { return Runner::RUNNER_ERROR; }

        // If we haven't picked the best plan yet...
        if (NULL == _bestPlan) {
            if (!pickBestPlan(NULL)) {
                verify(_failure || _killed);
                if (_killed) { return Runner::RUNNER_DEAD; }
                if (_failure) { return Runner::RUNNER_ERROR; }
            }
        }

        if (!_alreadyProduced.empty()) {
            WorkingSetID id = _alreadyProduced.front();
            _alreadyProduced.pop_front();

            WorkingSetMember* member = _bestPlan->getWorkingSet()->get(id);
            // Note that this copies code from PlanExecutor.
            if (NULL != objOut) {
                if (WorkingSetMember::LOC_AND_IDX == member->state) {
                    if (1 != member->keyData.size()) {
                        _bestPlan->getWorkingSet()->free(id);
                        return Runner::RUNNER_ERROR;
                    }
                    *objOut = member->keyData[0].keyData;
                }
                else if (member->hasObj()) {
                    *objOut = member->obj;
                }
                else {
                    // TODO: Checking the WSM for covered fields goes here.
                    _bestPlan->getWorkingSet()->free(id);
                    return Runner::RUNNER_ERROR;
                }
            }

            if (NULL != dlOut) {
                if (member->hasLoc()) {
                    *dlOut = member->loc;
                }
                else {
                    _bestPlan->getWorkingSet()->free(id);
                    return Runner::RUNNER_ERROR;
                }
            }
            _bestPlan->getWorkingSet()->free(id);
            return Runner::RUNNER_ADVANCED;
        }

        return _bestPlan->getNext(objOut, dlOut);
    }

    bool MultiPlanRunner::pickBestPlan(size_t* out) {
        static const int timesEachPlanIsWorked = 100;

        // Run each plan some number of times.
        for (int i = 0; i < timesEachPlanIsWorked; ++i) {
            bool moreToDo = workAllPlans();
            if (!moreToDo) { break; }
        }

        if (_failure || _killed) { return false; }

        size_t bestChild = PlanRanker::pickBestPlan(_candidates, NULL);

        // Run the best plan.  Store it.
        _bestPlan.reset(new PlanExecutor(_candidates[bestChild].ws,
                                         _candidates[bestChild].root));
        _bestPlan->setYieldPolicy(_policy);
        _alreadyProduced = _candidates[bestChild].results;
        _bestSolution.reset(_candidates[bestChild].solution);

        QLOG() << "Winning solution:\n" << _bestSolution->toString() << endl;

        // TODO:
        // Store the choice we just made in the cache.
        // QueryPlanCache* cache = PlanCache::get(somenamespace);
        // cache->add(_query, *_candidates[bestChild]->solution, decision->bestPlanStats);
        // delete decision;

        // Clear out the candidate plans, leaving only stats as we're all done w/them.
        for (size_t i = 0; i < _candidates.size(); ++i) {
            if (i == bestChild) { continue; }
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
        if (NULL != out) { *out = bestChild; }
        return true;
    }

    bool MultiPlanRunner::workAllPlans() {
        bool planHitEOF = false;

        for (size_t i = 0; i < _candidates.size(); ++i) {
            CandidatePlan& candidate = _candidates[i];
            if (candidate.failed) { continue; }

            WorkingSetID id;
            PlanStage::StageState state = candidate.root->work(&id);

            if (PlanStage::ADVANCED == state) {
                // Save result for later.
                candidate.results.push_back(id);
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
                planHitEOF = true;
            }
            else {
                // FAILURE.  Do we want to just tank that plan and try the rest?  We probably want
                // to fail globally as this shouldn't happen anyway.

                candidate.failed = true;
                ++_failureCount;

                if (_failureCount == _candidates.size()) {
                    _failure = true;
                    return false;
                }
            }

            // Yield, if we can yield ourselves.
            if (NULL != _yieldPolicy.get() && _yieldPolicy->shouldYield()) {
                saveState();
                _yieldPolicy->yield();
                if (_failure || _killed) { return false; }
                restoreState();
            }
        }

        return !planHitEOF;
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

    Status MultiPlanRunner::getExplainPlan(TypeExplain** explain) const {
        dassert(_bestPlan.get());

        //
        // Explain for the winner plan
        //

        scoped_ptr<PlanStageStats> stats(_bestPlan->getStats());
        if (NULL == stats.get()) {
            return Status(ErrorCodes::InternalError, "no stats available to explain plan");
        }

        Status status = explainPlan(*stats, explain, true /* full details */);
        if (!status.isOK()) {
            return status;
        }

        // TODO Hook the cached plan if there was one.
        // (*explain)->setOldPlan(???);

        //
        // Alternative plans' explains
        //
        // We get information about all the plans considered and hook them up the the main
        // explain structure. If we fail to get any of them, we still return the main explain.
        // Make sure we initialize the "*AllPlans" fields with the plan that was chose.
        //

        TypeExplain* chosenPlan = NULL;
        status = explainPlan(*stats, &chosenPlan, false /* no full details */);
        if (!status.isOK()) {
            return status;
        }

        (*explain)->addToAllPlans(chosenPlan); // ownership xfer

        uint64_t nScannedObjectsAllPlans = chosenPlan->getNScannedObjects();
        uint64_t nScannedAllPlans = chosenPlan->getNScanned();
        for (std::vector<PlanStageStats*>::const_iterator it = _candidateStats.begin();
             it != _candidateStats.end();
             ++it) {

            TypeExplain* candidateExplain;
            status = explainPlan(**it, &candidateExplain, false /* no full details */);
            if (status != Status::OK()) {
                continue;
            }

            (*explain)->addToAllPlans(candidateExplain); // ownership xfer

            nScannedObjectsAllPlans += candidateExplain->getNScannedObjects();
            nScannedAllPlans += candidateExplain->getNScanned();
        }

        (*explain)->setNScannedObjectsAllPlans(nScannedObjectsAllPlans);
        (*explain)->setNScannedAllPlans(nScannedAllPlans);

        return Status::OK();
    }

}  // namespace mongo
