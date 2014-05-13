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

#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/util/mongoutils/str.h"

// for updateCache
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/qlog.h"

namespace mongo {
    MultiPlanStage::MultiPlanStage(const Collection* collection, CanonicalQuery* cq)
        : _collection(collection),
          _query(cq),
          _bestPlanIdx(kNoSuchPlan),
          _backupPlanIdx(kNoSuchPlan),
          _failure(false),
          _failureCount(0),
          _statusMemberId(WorkingSet::INVALID_ID) { }

    MultiPlanStage::~MultiPlanStage() {
        if (bestPlanChosen()) {
            delete _candidates[_bestPlanIdx].root;

            // for now, the runner that executes this multi-plan-stage wants to own
            // the query solution for the best plan.  So we won't delete it here.
            // eventually, plan stages may own their query solutions.
            // 
            // delete _candidates[_bestPlanIdx].solution; // (owned by containing runner)

            if (hasBackupPlan()) {
                delete _candidates[_backupPlanIdx].solution;
                delete _candidates[_backupPlanIdx].root;
            }
	}
        else {
            for (size_t ix = 0; ix < _candidates.size(); ++ix) {
                delete _candidates[ix].solution;
                delete _candidates[ix].root;
            }
        }

        for (vector<PlanStageStats*>::iterator it = _candidateStats.begin();
             it != _candidateStats.end();
             ++it) {
            delete *it;
        }
    }

    void MultiPlanStage::addPlan(QuerySolution* solution, PlanStage* root,
                                 WorkingSet* ws) {
        _candidates.push_back(CandidatePlan(solution, root, ws));
    }

    bool MultiPlanStage::isEOF() {
        if (_failure) { return true; }

        // If _bestPlanIdx hasn't been found, can't be at EOF
        if (!bestPlanChosen()) { return false; }

        // We must have returned all our cached results
	// and there must be no more results from the best plan.
	CandidatePlan& bestPlan = _candidates[_bestPlanIdx];
        return bestPlan.results.empty() && bestPlan.root->isEOF();
    }

    PlanStage::StageState MultiPlanStage::work(WorkingSetID* out) {
	if (_failure) {
            *out = _statusMemberId;
            return PlanStage::FAILURE;
        }

	CandidatePlan& bestPlan = _candidates[_bestPlanIdx];

        // Look for an already produced result that provides the data the caller wants.
        if (!bestPlan.results.empty()) {
            *out = bestPlan.results.front();
            bestPlan.results.pop_front();
	    return PlanStage::ADVANCED;
        }

	// best plan had no (or has no more) cached results

        StageState state = bestPlan.root->work(out);

        if (PlanStage::FAILURE == state && hasBackupPlan()) {
            QLOG() << "Best plan errored out switching to backup\n";
            // Uncache the bad solution if we fall back
            // on the backup solution.
            //
            // XXX: Instead of uncaching we should find a way for the
            // cached plan runner to fall back on a different solution
            // if the best solution fails. Alternatively we could try to
            // defer cache insertion to be after the first produced result.

            _collection->infoCache()->getPlanCache()->remove(*_query);

	    _bestPlanIdx = _backupPlanIdx;
	    _backupPlanIdx = kNoSuchPlan;

	    return _candidates[_bestPlanIdx].root->work(out);
        }

        if (hasBackupPlan() && PlanStage::ADVANCED == state) {
            QLOG() << "Best plan had a blocking sort, became unblocked, deleting backup plan\n";
	    delete _candidates[_backupPlanIdx].solution;
	    delete _candidates[_backupPlanIdx].root;
	    _backupPlanIdx = kNoSuchPlan;
        }

        return state;
    }

    void MultiPlanStage::pickBestPlan() {
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
        for (size_t ix = 0; ix < numWorks; ++ix) {
            bool moreToDo = workAllPlans();
            if (!moreToDo) { break; }
        }

        if (_failure) { return; }

        // After picking best plan, ranking will own plan stats from
        // candidate solutions (winner and losers).
        std::auto_ptr<PlanRankingDecision> ranking(new PlanRankingDecision);
        _bestPlanIdx = PlanRanker::pickBestPlan(_candidates, ranking.get());
        verify(_bestPlanIdx >= 0 && _bestPlanIdx < static_cast<int>(_candidates.size()));

        // Copy candidate order. We will need this to sort candidate stats for explain
        // after transferring ownership of 'ranking' to plan cache.
        std::vector<size_t> candidateOrder = ranking->candidateOrder;

	CandidatePlan& bestCandidate = _candidates[_bestPlanIdx];
	std::list<WorkingSetID>& alreadyProduced = bestCandidate.results;
	QuerySolution* bestSolution = bestCandidate.solution;

        QLOG() << "Winning solution:\n" << bestSolution->toString() << endl;
        LOG(2) << "Winning plan: " << getPlanSummary(*bestSolution);

        _backupPlanIdx = kNoSuchPlan;
        if (bestSolution->hasBlockingStage && (0 == alreadyProduced.size())) {
            QLOG() << "Winner has blocking stage, looking for backup plan...\n";
            for (size_t ix = 0; ix < _candidates.size(); ++ix) {
                if (!_candidates[ix].solution->hasBlockingStage) {
                    QLOG() << "Candidate " << ix << " is backup child\n";
                    _backupPlanIdx = ix;
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
        const PlanStageStats* bestStats = ranking->stats.vector()[0];
        if (PlanCache::shouldCacheQuery(*_query)
            && (!alreadyProduced.empty() || bestStats->common.isEOF)) {

            // Create list of candidate solutions for the cache with
            // the best solution at the front.
            std::vector<QuerySolution*> solutions;

            // Generate solutions and ranking decisions sorted by score.
            for (size_t orderingIndex = 0;
                 orderingIndex < candidateOrder.size(); ++orderingIndex) {
                // index into candidates/ranking
                size_t ix = candidateOrder[orderingIndex];
                solutions.push_back(_candidates[ix].solution);
            }

            // Check solution cache data. Do not add to cache if
            // we have any invalid SolutionCacheData data.
            // XXX: One known example is 2D queries
            bool validSolutions = true;
            for (size_t ix = 0; ix < solutions.size(); ++ix) {
                if (NULL == solutions[ix]->cacheData.get()) {
                    QLOG() << "Not caching query because this solution has no cache data: "
                           << solutions[ix]->toString();
                    validSolutions = false;
                    break;
                }
            }

            if (validSolutions) {
                _collection->infoCache()->getPlanCache()->add(*_query, solutions, ranking.release());
            }
        }

        // Clear out the candidate plans, leaving only stats as we're all done w/them.
        // Traverse candidate plans in order or score
        for (size_t orderingIndex = 0;
             orderingIndex < candidateOrder.size(); ++orderingIndex) {
            // index into candidates/ranking
            int ix = candidateOrder[orderingIndex];

            if (ix == _bestPlanIdx) { continue; }
            if (ix == _backupPlanIdx) { continue; }

            delete _candidates[ix].solution;

            // Remember the stats for the candidate plan because we always show it on an
            // explain. (The {verbose:false} in explain() is client-side trick; we always
            // generate a "verbose" explain.)
            PlanStageStats* stats = _candidates[ix].root->getStats();
            if (stats) {
                _candidateStats.push_back(stats);
            }
            delete _candidates[ix].root;
        }
    }

    bool MultiPlanStage::workAllPlans() {
        bool doneWorking = false;

        for (size_t ix = 0; ix < _candidates.size(); ++ix) {
            CandidatePlan& candidate = _candidates[ix];
            if (candidate.failed) { continue; }

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
            else if (PlanStage::NEED_FETCH == state) {
                // id has a loc and refers to an obj we need to fetch.
                WorkingSetMember* member = candidate.ws->get(id);

                // This must be true for somebody to request a fetch and can only change when an
                // invalidation happens, which is when we give up a lock.  Don't give up the
                // lock between receiving the NEED_FETCH and actually fetching(?).
                verify(member->hasLoc());

                // XXX: remove NEED_FETCH
            }
            else if (PlanStage::IS_EOF == state) {
                // First plan to hit EOF wins automatically.  Stop evaluating other plans.
                // Assumes that the ranking will pick this plan.
                doneWorking = true;
            }
            else if (PlanStage::NEED_TIME != state) {
                // FAILURE or DEAD.  Do we want to just tank that plan and try the rest?  We
                // probably want to fail globally as this shouldn't happen anyway.

                candidate.failed = true;
                ++_failureCount;

                // Propagate most recent seen failure to parent.
                if (PlanStage::FAILURE == state) {
		    BSONObj objOut;
                    WorkingSetCommon::getStatusMemberObject(*candidate.ws, id, &objOut);
                    _statusMemberId = id;
                }

                if (_failureCount == _candidates.size()) {
                    _failure = true;
                    return false;
                }
            }
        }

        return !doneWorking;
    }

    void MultiPlanStage::prepareToYield() {
        if (_failure) return;

	// this logic is from multi_plan_runner
	// but does it really make sense to operate on
	// the _bestPlan if we've switched to the backup?

        if (bestPlanChosen()) {
            _candidates[_bestPlanIdx].root->prepareToYield();
            if (hasBackupPlan()) {
                _candidates[_backupPlanIdx].root->prepareToYield();
            }
        }
        else {
            allPlansSaveState();
        }
    }

    void MultiPlanStage::recoverFromYield() {
        if (_failure) return;

	// this logic is from multi_plan_runner
	// but does it really make sense to operate on
	// the _bestPlan if we've switched to the backup?

        if (bestPlanChosen()) {
            _candidates[_bestPlanIdx].root->recoverFromYield();
            if (hasBackupPlan()) {
                _candidates[_backupPlanIdx].root->recoverFromYield();
            }
        }
        else {
            allPlansRestoreState();
        }
    }

    namespace {
        void invalidateHelper(
            WorkingSet* ws, // may flag for review
            const DiskLoc& dl,
            list<WorkingSetID>* idsToInvalidate,
            const Collection* collection
        ) {
            for (list<WorkingSetID>::iterator it = idsToInvalidate->begin();
		 it != idsToInvalidate->end();) {
                WorkingSetMember* member = ws->get(*it);
                if (member->hasLoc() && member->loc == dl) {
                    list<WorkingSetID>::iterator next = it;
                    next++;
                    WorkingSetCommon::fetchAndInvalidateLoc(member, collection);
                    ws->flagForReview(*it);
                    idsToInvalidate->erase(it);
                    it = next;
                }
                else {
                    it++;
                }
            }
        }
    }

    void MultiPlanStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        if (_failure) { return; }

        if (bestPlanChosen()) {
	    CandidatePlan& bestPlan = _candidates[_bestPlanIdx];
            bestPlan.root->invalidate(dl, type);
	    invalidateHelper(bestPlan.ws, dl, &bestPlan.results, _collection);
            if (hasBackupPlan()) {
	        CandidatePlan& backupPlan = _candidates[_backupPlanIdx];
                backupPlan.root->invalidate(dl, type);
		invalidateHelper(backupPlan.ws, dl, &backupPlan.results, _collection);
            }
        }
        else {
            for (size_t ix = 0; ix < _candidates.size(); ++ix) {
                _candidates[ix].root->invalidate(dl, type);
		invalidateHelper(_candidates[ix].ws, dl, &_candidates[ix].results, _collection);
            }
        }
    }

    bool MultiPlanStage::hasBackupPlan() const {
        return kNoSuchPlan != _backupPlanIdx;
    }

    bool MultiPlanStage::bestPlanChosen() const {
        return kNoSuchPlan != _bestPlanIdx;
    }

    int MultiPlanStage::bestPlanIdx() const {
        return _bestPlanIdx;
    }

    QuerySolution* MultiPlanStage::bestSolution() {
        if (_bestPlanIdx == kNoSuchPlan)
            return NULL;

        return _candidates[_bestPlanIdx].solution;
    }

    void MultiPlanStage::allPlansSaveState() {
        for (size_t i = 0; i < _candidates.size(); ++i) {
            _candidates[i].root->prepareToYield();
        }
    }

    void MultiPlanStage::allPlansRestoreState() {
        for (size_t i = 0; i < _candidates.size(); ++i) {
            _candidates[i].root->recoverFromYield();
        }
    }

    PlanStageStats* MultiPlanStage::getStats() {
        if (bestPlanChosen()) {
	    return _candidates[_bestPlanIdx].root->getStats();
	}
	if (hasBackupPlan()) {
	    return _candidates[_backupPlanIdx].root->getStats();
	}
        _commonStats.isEOF = isEOF();

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_MULTI_PLAN));

        return ret.release();
    }

}  // namespace mongo
