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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/multi_plan.h"

#include <algorithm>
#include <math.h>
#include <memory>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

namespace mongo {

using std::endl;
using std::list;
using std::unique_ptr;
using std::vector;

// static
const char* MultiPlanStage::kStageType = "MULTI_PLAN";

namespace {
void markShouldCollectTimingInfoOnSubtree(PlanStage* root) {
    root->markShouldCollectTimingInfo();
    for (auto&& child : root->getChildren()) {
        markShouldCollectTimingInfoOnSubtree(child.get());
    }
}
}  // namespace

MultiPlanStage::MultiPlanStage(ExpressionContext* expCtx,
                               const Collection* collection,
                               CanonicalQuery* cq,
                               CachingMode cachingMode)
    : RequiresCollectionStage(kStageType, expCtx, collection),
      _cachingMode(cachingMode),
      _query(cq),
      _bestPlanIdx(kNoSuchPlan),
      _backupPlanIdx(kNoSuchPlan),
      _failure(false),
      _failureCount(0),
      _statusMemberId(WorkingSet::INVALID_ID) {}

void MultiPlanStage::addPlan(std::unique_ptr<QuerySolution> solution,
                             std::unique_ptr<PlanStage> root,
                             WorkingSet* ws) {
    _children.emplace_back(std::move(root));
    _candidates.push_back(CandidatePlan(std::move(solution), _children.back().get(), ws));

    // Tell the new candidate plan that it must collect timing info. This timing info will
    // later be stored in the plan cache, and may be used for explain output.
    PlanStage* newChild = _children.back().get();
    markShouldCollectTimingInfoOnSubtree(newChild);
}

bool MultiPlanStage::isEOF() {
    if (_failure) {
        return true;
    }

    // If _bestPlanIdx hasn't been found, can't be at EOF
    if (!bestPlanChosen()) {
        return false;
    }

    // We must have returned all our cached results
    // and there must be no more results from the best plan.
    CandidatePlan& bestPlan = _candidates[_bestPlanIdx];
    return bestPlan.results.empty() && bestPlan.root->isEOF();
}

PlanStage::StageState MultiPlanStage::doWork(WorkingSetID* out) {
    if (_failure) {
        *out = _statusMemberId;
        return PlanStage::FAILURE;
    }

    CandidatePlan& bestPlan = _candidates[_bestPlanIdx];

    // Look for an already produced result that provides the data the caller wants.
    if (!bestPlan.results.empty()) {
        *out = bestPlan.results.front();
        bestPlan.results.pop();
        return PlanStage::ADVANCED;
    }

    // best plan had no (or has no more) cached results

    StageState state = bestPlan.root->work(out);

    if (PlanStage::FAILURE == state && hasBackupPlan()) {
        LOGV2_DEBUG(20588, 5, "Best plan errored out switching to backup");
        // Uncache the bad solution if we fall back
        // on the backup solution.
        //
        // XXX: Instead of uncaching we should find a way for the
        // cached plan runner to fall back on a different solution
        // if the best solution fails. Alternatively we could try to
        // defer cache insertion to be after the first produced result.

        CollectionQueryInfo::get(collection())
            .getPlanCache()
            ->remove(*_query)
            .transitional_ignore();

        _bestPlanIdx = _backupPlanIdx;
        _backupPlanIdx = kNoSuchPlan;

        return _candidates[_bestPlanIdx].root->work(out);
    }

    if (hasBackupPlan() && PlanStage::ADVANCED == state) {
        LOGV2_DEBUG(20589, 5, "Best plan had a blocking stage, became unblocked");
        _backupPlanIdx = kNoSuchPlan;
    }

    return state;
}

Status MultiPlanStage::tryYield(PlanYieldPolicy* yieldPolicy) {
    // These are the conditions which can cause us to yield:
    //   1) The yield policy's timer elapsed, or
    //   2) some stage requested a yield, or
    //   3) we need to yield and retry due to a WriteConflictException.
    // In all cases, the actual yielding happens here.
    if (yieldPolicy->shouldYieldOrInterrupt()) {
        auto yieldStatus = yieldPolicy->yieldOrInterrupt();

        if (!yieldStatus.isOK()) {
            _failure = true;
            _statusMemberId =
                WorkingSetCommon::allocateStatusMember(_candidates[0].ws, yieldStatus);
            return yieldStatus;
        }
    }

    return Status::OK();
}

// static
size_t MultiPlanStage::getTrialPeriodWorks(OperationContext* opCtx, const Collection* collection) {
    // Run each plan some number of times. This number is at least as great as
    // 'internalQueryPlanEvaluationWorks', but may be larger for big collections.
    size_t numWorks = internalQueryPlanEvaluationWorks.load();
    if (nullptr != collection) {
        // For large collections, the number of works is set to be this
        // fraction of the collection size.
        double fraction = internalQueryPlanEvaluationCollFraction;

        numWorks = std::max(static_cast<size_t>(internalQueryPlanEvaluationWorks.load()),
                            static_cast<size_t>(fraction * collection->numRecords(opCtx)));
    }

    return numWorks;
}

// static
size_t MultiPlanStage::getTrialPeriodNumToReturn(const CanonicalQuery& query) {
    // Determine the number of results which we will produce during the plan
    // ranking phase before stopping.
    size_t numResults = static_cast<size_t>(internalQueryPlanEvaluationMaxResults.load());
    if (query.getQueryRequest().getNToReturn()) {
        numResults =
            std::min(static_cast<size_t>(*query.getQueryRequest().getNToReturn()), numResults);
    } else if (query.getQueryRequest().getLimit()) {
        numResults = std::min(static_cast<size_t>(*query.getQueryRequest().getLimit()), numResults);
    }

    return numResults;
}

Status MultiPlanStage::pickBestPlan(PlanYieldPolicy* yieldPolicy) {
    // Adds the amount of time taken by pickBestPlan() to executionTimeMillis. There's lots of
    // execution work that happens here, so this is needed for the time accounting to
    // make sense.
    auto optTimer = getOptTimer();

    size_t numWorks = getTrialPeriodWorks(opCtx(), collection());
    size_t numResults = getTrialPeriodNumToReturn(*_query);

    try {
        // Work the plans, stopping when a plan hits EOF or returns some fixed number of results.
        for (size_t ix = 0; ix < numWorks; ++ix) {
            bool moreToDo = workAllPlans(numResults, yieldPolicy);
            if (!moreToDo) {
                break;
            }
        }
    } catch (DBException& e) {
        e.addContext("exception thrown while multiplanner was selecting best plan");
        throw;
    }

    if (_failure) {
        invariant(WorkingSet::INVALID_ID != _statusMemberId);
        WorkingSetMember* member = _candidates[0].ws->get(_statusMemberId);
        return WorkingSetCommon::getMemberStatus(*member).withContext(
            "multiplanner encountered a failure while selecting best plan");
    }

    // After picking best plan, ranking will own plan stats from
    // candidate solutions (winner and losers).
    auto statusWithRanking = PlanRanker::pickBestPlan(_candidates);
    if (!statusWithRanking.isOK()) {
        return statusWithRanking.getStatus();
    }

    auto ranking = std::move(statusWithRanking.getValue());
    // Since the status was ok there should be a ranking containing at least one successfully ranked
    // plan.
    invariant(ranking);
    _bestPlanIdx = ranking->candidateOrder[0];

    verify(_bestPlanIdx >= 0 && _bestPlanIdx < static_cast<int>(_candidates.size()));

    // Copy candidate order and failed candidates. We will need this to sort candidate stats for
    // explain after transferring ownership of 'ranking' to plan cache.
    std::vector<size_t> candidateOrder = ranking->candidateOrder;
    std::vector<size_t> failedCandidates = ranking->failedCandidates;

    CandidatePlan& bestCandidate = _candidates[_bestPlanIdx];
    const auto& alreadyProduced = bestCandidate.results;
    const auto& bestSolution = bestCandidate.solution;

    LOGV2_DEBUG(20590,
                5,
                "Winning solution:\n{bestSolution}",
                "bestSolution"_attr = redact(bestSolution->toString()));
    LOGV2_DEBUG(20591,
                2,
                "Winning plan: {Explain_getPlanSummary_bestCandidate_root}",
                "Explain_getPlanSummary_bestCandidate_root"_attr =
                    Explain::getPlanSummary(bestCandidate.root));

    _backupPlanIdx = kNoSuchPlan;
    if (bestSolution->hasBlockingStage && (0 == alreadyProduced.size())) {
        LOGV2_DEBUG(20592, 5, "Winner has blocking stage, looking for backup plan...");
        for (auto&& ix : candidateOrder) {
            if (!_candidates[ix].solution->hasBlockingStage) {
                LOGV2_DEBUG(20593, 5, "Candidate {ix} is backup child", "ix"_attr = ix);
                _backupPlanIdx = ix;
                break;
            }
        }
    }

    // Even if the query is of a cacheable shape, the caller might have indicated that we shouldn't
    // write to the plan cache.
    //
    // TODO: We can remove this if we introduce replanning logic to the SubplanStage.
    bool canCache = (_cachingMode == CachingMode::AlwaysCache);
    if (_cachingMode == CachingMode::SometimesCache) {
        // In "sometimes cache" mode, we cache unless we hit one of the special cases below.
        canCache = true;

        if (ranking->tieForBest) {
            // The winning plan tied with the runner-up and we're using "sometimes cache" mode. We
            // will not write a plan cache entry.
            canCache = false;

            // These arrays having two or more entries is implied by 'tieForBest'.
            invariant(ranking->scores.size() > 1U);
            invariant(ranking->candidateOrder.size() > 1U);

            size_t winnerIdx = ranking->candidateOrder[0];
            size_t runnerUpIdx = ranking->candidateOrder[1];

            LOGV2_DEBUG(20594,
                        1,
                        "Winning plan tied with runner-up. Not caching. query: {query_Short} "
                        "winner score: {ranking_scores_0} winner summary: "
                        "{Explain_getPlanSummary_candidates_winnerIdx_root} runner-up score: "
                        "{ranking_scores_1} runner-up summary: "
                        "{Explain_getPlanSummary_candidates_runnerUpIdx_root}",
                        "query_Short"_attr = redact(_query->toStringShort()),
                        "ranking_scores_0"_attr = ranking->scores[0],
                        "Explain_getPlanSummary_candidates_winnerIdx_root"_attr =
                            Explain::getPlanSummary(_candidates[winnerIdx].root),
                        "ranking_scores_1"_attr = ranking->scores[1],
                        "Explain_getPlanSummary_candidates_runnerUpIdx_root"_attr =
                            Explain::getPlanSummary(_candidates[runnerUpIdx].root));
        }

        if (alreadyProduced.empty()) {
            // We're using the "sometimes cache" mode, and the winning plan produced no results
            // during the plan ranking trial period. We will not write a plan cache entry.
            canCache = false;

            size_t winnerIdx = ranking->candidateOrder[0];
            LOGV2_DEBUG(20595,
                        1,
                        "Winning plan had zero results. Not caching. query: {query_Short} winner "
                        "score: {ranking_scores_0} winner summary: "
                        "{Explain_getPlanSummary_candidates_winnerIdx_root}",
                        "query_Short"_attr = redact(_query->toStringShort()),
                        "ranking_scores_0"_attr = ranking->scores[0],
                        "Explain_getPlanSummary_candidates_winnerIdx_root"_attr =
                            Explain::getPlanSummary(_candidates[winnerIdx].root));
        }
    }

    // Store the choice we just made in the cache, if the query is of a type that is safe to
    // cache.
    if (PlanCache::shouldCacheQuery(*_query) && canCache) {
        // Create list of candidate solutions for the cache with
        // the best solution at the front.
        std::vector<QuerySolution*> solutions;

        // Generate solutions and ranking decisions sorted by score.
        for (auto&& ix : candidateOrder) {
            solutions.push_back(_candidates[ix].solution.get());
        }
        // Insert the failed plans in the back.
        for (auto&& ix : failedCandidates) {
            solutions.push_back(_candidates[ix].solution.get());
        }

        // Check solution cache data. Do not add to cache if
        // we have any invalid SolutionCacheData data.
        // XXX: One known example is 2D queries
        bool validSolutions = true;
        for (size_t ix = 0; ix < solutions.size(); ++ix) {
            if (nullptr == solutions[ix]->cacheData.get()) {
                LOGV2_DEBUG(
                    20596,
                    5,
                    "Not caching query because this solution has no cache data: {solutions_ix}",
                    "solutions_ix"_attr = redact(solutions[ix]->toString()));
                validSolutions = false;
                break;
            }
        }

        if (validSolutions) {
            CollectionQueryInfo::get(collection())
                .getPlanCache()
                ->set(*_query,
                      solutions,
                      std::move(ranking),
                      opCtx()->getServiceContext()->getPreciseClockSource()->now())
                .transitional_ignore();
        }
    }

    return Status::OK();
}

bool MultiPlanStage::workAllPlans(size_t numResults, PlanYieldPolicy* yieldPolicy) {
    bool doneWorking = false;

    for (size_t ix = 0; ix < _candidates.size(); ++ix) {
        CandidatePlan& candidate = _candidates[ix];
        if (candidate.failed) {
            continue;
        }

        // Might need to yield between calls to work due to the timer elapsing.
        if (!(tryYield(yieldPolicy)).isOK()) {
            return false;
        }

        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = candidate.root->work(&id);

        if (PlanStage::ADVANCED == state) {
            // Save result for later.
            WorkingSetMember* member = candidate.ws->get(id);
            // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we choose to
            // return the results from the 'candidate' plan.
            member->makeObjOwnedIfNeeded();
            candidate.results.push(id);

            // Once a plan returns enough results, stop working.
            if (candidate.results.size() >= numResults) {
                doneWorking = true;
            }
        } else if (PlanStage::IS_EOF == state) {
            // First plan to hit EOF wins automatically.  Stop evaluating other plans.
            // Assumes that the ranking will pick this plan.
            doneWorking = true;
        } else if (PlanStage::NEED_YIELD == state) {
            invariant(id == WorkingSet::INVALID_ID);
            if (!yieldPolicy->canAutoYield()) {
                throw WriteConflictException();
            }

            if (yieldPolicy->canAutoYield()) {
                yieldPolicy->forceYield();
            }

            if (!(tryYield(yieldPolicy)).isOK()) {
                return false;
            }
        } else if (PlanStage::NEED_TIME != state) {
            // On FAILURE, mark this candidate as failed, but keep executing the other
            // candidates. The MultiPlanStage as a whole only fails when every candidate
            // plan fails.

            candidate.failed = true;
            ++_failureCount;

            // Propagate most recent seen failure to parent.
            invariant(state == PlanStage::FAILURE);
            _statusMemberId = id;


            if (_failureCount == _candidates.size()) {
                _failure = true;
                return false;
            }
        }
    }

    return !doneWorking;
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
        return nullptr;

    return _candidates[_bestPlanIdx].solution.get();
}

unique_ptr<PlanStageStats> MultiPlanStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_MULTI_PLAN);
    ret->specific = std::make_unique<MultiPlanStats>(_specificStats);
    for (auto&& child : _children) {
        ret->children.emplace_back(child->getStats());
    }
    return ret;
}

const SpecificStats* MultiPlanStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
