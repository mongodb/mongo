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
 */

#include "mongo/db/query/multi_plan_runner.h"

#include "mongo/db/clientcursor.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    MultiPlanRunner::MultiPlanRunner(CanonicalQuery* query)
        : _failure(false), _query(query), _killed(false) { }

    MultiPlanRunner::~MultiPlanRunner() {
        for (size_t i = 0; i < _candidates.size(); ++i) {
            delete _candidates[i].solution;
            delete _candidates[i].root;
            // ws must die after the root.
            delete _candidates[i].ws;
        }
    }

    void MultiPlanRunner::addPlan(QuerySolution* solution, PlanStage* root, WorkingSet* ws) {
        _candidates.push_back(CandidatePlan(solution, root, ws));
    }

    void MultiPlanRunner::saveState() {
        if (_killed) { return; }
        if (NULL != _bestPlan) {
            _bestPlan->saveState();
        }
        else {
            yieldAllPlans();
        }
    }

    void MultiPlanRunner::restoreState() {
        if (_killed) { return; }
        if (NULL != _bestPlan) {
            _bestPlan->restoreState();
        }
        else {
            unyieldAllPlans();
        }
    }

    void MultiPlanRunner::invalidate(const DiskLoc& dl) {
        if (_killed) { return; }

        if (NULL != _bestPlan) {
            _bestPlan->invalidate(dl);
        }
        else {
            for (size_t i = 0; i < _candidates.size(); ++i) {
                _candidates[i].root->invalidate(dl);
            }
        }
    }

    void MultiPlanRunner::kill() {
        _killed = true;
    }

    bool MultiPlanRunner::forceYield() {
        saveState();
        ClientCursor::registerRunner(this);
        ClientCursor::staticYield(ClientCursor::suggestYieldMicros(),
                                  getQuery().getParsed().ns(),
                                  NULL);
        // During the yield, the database we're operating over or any collection we're relying on
        // may be dropped.  When this happens all cursors and runners on that database and
        // collection are killed or deleted in some fashion. (This is how the _killed gets set.)
        ClientCursor::deregisterRunner(this);
        if (!_killed) { restoreState(); }
        return !_killed;
    }

    Runner::RunnerState MultiPlanRunner::getNext(BSONObj* objOut) {
        if (_failure || _killed) {
            return Runner::RUNNER_DEAD;
        }

        // If we haven't picked the best plan yet...
        if (NULL == _bestPlan) {
            // TODO: Consider rewriting pickBestPlan to return results as it iterates.
            if (!pickBestPlan(NULL)) {
                verify(_failure);
                return Runner::RUNNER_DEAD;
            }
        }

        if (!_alreadyProduced.empty()) {
            WorkingSetID id = _alreadyProduced.front();
            _alreadyProduced.pop();

            WorkingSetMember* member = _bestPlan->getWorkingSet()->get(id);
            // TODO: getOwned?
            verify(WorkingSetCommon::fetch(member));
            *objOut = member->obj;
            _bestPlan->getWorkingSet()->free(id);
            return Runner::RUNNER_ADVANCED;
        }

        return _bestPlan->getNext(objOut);
    }

    bool MultiPlanRunner::pickBestPlan(size_t* out) {
        static const int timesEachPlanIsWorked = 100;
        static const int yieldInterval = 10;

        // Run each plan some number of times.
        for (int i = 0; i < timesEachPlanIsWorked; ++i) {
            bool moreToDo = workAllPlans();
            if (!moreToDo) { break; }

            if (0 == ((i + 1) % yieldInterval)) {
                yieldAllPlans();
                // TODO: Actually yield...
                unyieldAllPlans();
            }
        }

        if (_failure) { return false; }

        size_t bestChild = PlanRanker::pickBestPlan(_candidates, NULL);

        // Run the best plan.  Store it.
        _bestPlan.reset(new PlanExecutor(_candidates[bestChild].ws,
                                         _candidates[bestChild].root));
        _alreadyProduced = _candidates[bestChild].results;
        // TODO: Normally we'd hand this to the cache, who would own it.
        delete _candidates[bestChild].solution;

        // Store the choice we just made in the cache.
        // QueryPlanCache* cache = PlanCache::get(somenamespace);
        // cache->add(_query, *_candidates[bestChild]->solution, decision->bestPlanStats);
        // delete decision;

        // Clear out the candidate plans as we're all done w/them.
        for (size_t i = 0; i < _candidates.size(); ++i) {
            if (i == bestChild) { continue; }
            delete _candidates[i].solution;
            delete _candidates[i].root;
            // ws must die after the root.
            delete _candidates[i].ws;
        }

        _candidates.clear();
        if (NULL != out) { *out = bestChild; }
        return true;
    }

    bool MultiPlanRunner::workAllPlans() {
        for (size_t i = 0; i < _candidates.size(); ++i) {
            CandidatePlan& candidate = _candidates[i];

            WorkingSetID id;
            PlanStage::StageState state = candidate.root->work(&id);
            if (PlanStage::ADVANCED == state) {
                // Save result for later.
                candidate.results.push(id);
            }
            else if (PlanStage::NEED_TIME == state) {
                // Nothing to do here.
            }
            else if (PlanStage::NEED_FETCH == state) {
                // XXX: We can yield to do this.  We have to deal with synchronization issues with
                // regards to the working set and invalidation.  What if another thread invalidates
                // the thing we're fetching?  The loc could vanish between hasLoc() and the actual
                // fetch...

                // id has a loc and refers to an obj we need to fetch.
                WorkingSetMember* member = candidate.ws->get(id);

                // This must be true for somebody to request a fetch and can only change when an
                // invalidation happens, which is when we give up a lock.  Don't give up the
                // lock between receiving the NEED_FETCH and actually fetching(?).
                verify(member->hasLoc());

                // Actually bring record into memory.
                Record* record = member->loc.rec();
                record->touch();

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
                return false;
            }
            else {
                // FAILURE.  Do we want to just tank that plan and try the rest?  We probably want
                // to fail globally as this shouldn't happen anyway.
                _failure = true;
                return false;
            }
        }

        return true;
    }

    void MultiPlanRunner::yieldAllPlans() {
        for (size_t i = 0; i < _candidates.size(); ++i) {
            _candidates[i].root->prepareToYield();
        }
    }

    void MultiPlanRunner::unyieldAllPlans() {
        for (size_t i = 0; i < _candidates.size(); ++i) {
            _candidates[i].root->recoverFromYield();
        }
    }

}  // namespace mongo
