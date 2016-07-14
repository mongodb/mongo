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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/cached_plan.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

// static
const char* CachedPlanStage::kStageType = "CACHED_PLAN";

CachedPlanStage::CachedPlanStage(OperationContext* txn,
                                 Collection* collection,
                                 WorkingSet* ws,
                                 CanonicalQuery* cq,
                                 const QueryPlannerParams& params,
                                 size_t decisionWorks,
                                 PlanStage* root)
    : PlanStage(kStageType, txn),
      _collection(collection),
      _ws(ws),
      _canonicalQuery(cq),
      _plannerParams(params),
      _decisionWorks(decisionWorks) {
    invariant(_collection);
    _children.emplace_back(root);
}

Status CachedPlanStage::pickBestPlan(PlanYieldPolicy* yieldPolicy) {
    // Adds the amount of time taken by pickBestPlan() to executionTimeMillis. There's lots of
    // execution work that happens here, so this is needed for the time accounting to
    // make sense.
    ScopedTimer timer(getClock(), &_commonStats.executionTimeMillis);

    // If we work this many times during the trial period, then we will replan the
    // query from scratch.
    size_t maxWorksBeforeReplan =
        static_cast<size_t>(internalQueryCacheEvictionRatio * _decisionWorks);

    // The trial period ends without replanning if the cached plan produces this many results.
    size_t numResults = MultiPlanStage::getTrialPeriodNumToReturn(*_canonicalQuery);

    for (size_t i = 0; i < maxWorksBeforeReplan; ++i) {
        // Might need to yield between calls to work due to the timer elapsing.
        Status yieldStatus = tryYield(yieldPolicy);
        if (!yieldStatus.isOK()) {
            return yieldStatus;
        }

        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = child()->work(&id);

        if (PlanStage::ADVANCED == state) {
            // Save result for later.
            WorkingSetMember* member = _ws->get(id);
            // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we yield.
            member->makeObjOwnedIfNeeded();
            _results.push_back(id);

            if (_results.size() >= numResults) {
                // Once a plan returns enough results, stop working. Update cache with stats
                // from this run and return.
                updatePlanCache();
                return Status::OK();
            }
        } else if (PlanStage::IS_EOF == state) {
            // Cached plan hit EOF quickly enough. No need to replan. Update cache with stats
            // from this run and return.
            updatePlanCache();
            return Status::OK();
        } else if (PlanStage::NEED_YIELD == state) {
            if (id == WorkingSet::INVALID_ID) {
                if (!yieldPolicy->allowedToYield()) {
                    throw WriteConflictException();
                }
            } else {
                WorkingSetMember* member = _ws->get(id);
                invariant(member->hasFetcher());
                // Transfer ownership of the fetcher and yield.
                _fetcher.reset(member->releaseFetcher());
            }

            if (yieldPolicy->allowedToYield()) {
                yieldPolicy->forceYield();
            }

            Status yieldStatus = tryYield(yieldPolicy);
            if (!yieldStatus.isOK()) {
                return yieldStatus;
            }
        } else if (PlanStage::FAILURE == state) {
            // On failure, fall back to replanning the whole query. We neither evict the
            // existing cache entry nor cache the result of replanning.
            BSONObj statusObj;
            WorkingSetCommon::getStatusMemberObject(*_ws, id, &statusObj);

            LOG(1) << "Execution of cached plan failed, falling back to replan."
                   << " query: " << _canonicalQuery->toStringShort()
                   << " planSummary: " << Explain::getPlanSummary(child().get())
                   << " status: " << statusObj;

            const bool shouldCache = false;
            return replan(yieldPolicy, shouldCache);
        } else if (PlanStage::DEAD == state) {
            BSONObj statusObj;
            WorkingSetCommon::getStatusMemberObject(*_ws, id, &statusObj);

            LOG(1) << "Execution of cached plan failed: PlanStage died"
                   << ", query: " << _canonicalQuery->toStringShort()
                   << " planSummary: " << Explain::getPlanSummary(child().get())
                   << " status: " << statusObj;

            return WorkingSetCommon::getMemberObjectStatus(statusObj);
        } else {
            invariant(PlanStage::NEED_TIME == state);
        }
    }

    // If we're here, the trial period took more than 'maxWorksBeforeReplan' work cycles. This
    // plan is taking too long, so we replan from scratch.
    LOG(1) << "Execution of cached plan required " << maxWorksBeforeReplan
           << " works, but was originally cached with only " << _decisionWorks
           << " works. Evicting cache entry and replanning query: "
           << _canonicalQuery->toStringShort()
           << " plan summary before replan: " << Explain::getPlanSummary(child().get());

    const bool shouldCache = true;
    return replan(yieldPolicy, shouldCache);
}

Status CachedPlanStage::tryYield(PlanYieldPolicy* yieldPolicy) {
    // These are the conditions which can cause us to yield:
    //   1) The yield policy's timer elapsed, or
    //   2) some stage requested a yield due to a document fetch, or
    //   3) we need to yield and retry due to a WriteConflictException.
    // In all cases, the actual yielding happens here.
    if (yieldPolicy->shouldYield()) {
        // Here's where we yield.
        bool alive = yieldPolicy->yield(_fetcher.get());

        if (!alive) {
            return Status(ErrorCodes::QueryPlanKilled,
                          "CachedPlanStage killed during plan selection");
        }
    }

    // We're done using the fetcher, so it should be freed. We don't want to
    // use the same RecordFetcher twice.
    _fetcher.reset();

    return Status::OK();
}

Status CachedPlanStage::replan(PlanYieldPolicy* yieldPolicy, bool shouldCache) {
    // We're going to start over with a new plan. Clear out info from our old plan.
    _results.clear();
    _ws->clear();
    _children.clear();

    _specificStats.replanned = true;

    // Use the query planning module to plan the whole query.
    std::vector<QuerySolution*> rawSolutions;
    Status status = QueryPlanner::plan(*_canonicalQuery, _plannerParams, &rawSolutions);
    if (!status.isOK()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "error processing query: " << _canonicalQuery->toString()
                                    << " planner returned error: "
                                    << status.reason());
    }

    OwnedPointerVector<QuerySolution> solutions(rawSolutions);

    // We cannot figure out how to answer the query.  Perhaps it requires an index
    // we do not have?
    if (0 == solutions.size()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "error processing query: " << _canonicalQuery->toString()
                                    << " No query solutions");
    }

    if (1 == solutions.size()) {
        // If there's only one solution, it won't get cached. Make sure to evict the existing
        // cache entry if requested by the caller.
        if (shouldCache) {
            PlanCache* cache = _collection->infoCache()->getPlanCache();
            cache->remove(*_canonicalQuery);
        }

        PlanStage* newRoot;
        // Only one possible plan. Build the stages from the solution.
        verify(StageBuilder::build(
            getOpCtx(), _collection, *_canonicalQuery, *solutions[0], _ws, &newRoot));
        _children.emplace_back(newRoot);
        _replannedQs.reset(solutions.popAndReleaseBack());

        LOG(1)
            << "Replanning of query resulted in single query solution, which will not be cached. "
            << _canonicalQuery->toStringShort()
            << " plan summary after replan: " << Explain::getPlanSummary(child().get())
            << " previous cache entry evicted: " << (shouldCache ? "yes" : "no");
        return Status::OK();
    }

    // Many solutions. Create a MultiPlanStage to pick the best, update the cache,
    // and so on. The working set will be shared by all candidate plans.
    auto cachingMode = shouldCache ? MultiPlanStage::CachingMode::AlwaysCache
                                   : MultiPlanStage::CachingMode::NeverCache;
    _children.emplace_back(
        new MultiPlanStage(getOpCtx(), _collection, _canonicalQuery, cachingMode));
    MultiPlanStage* multiPlanStage = static_cast<MultiPlanStage*>(child().get());

    for (size_t ix = 0; ix < solutions.size(); ++ix) {
        if (solutions[ix]->cacheData.get()) {
            solutions[ix]->cacheData->indexFilterApplied = _plannerParams.indexFiltersApplied;
        }

        PlanStage* nextPlanRoot;
        verify(StageBuilder::build(
            getOpCtx(), _collection, *_canonicalQuery, *solutions[ix], _ws, &nextPlanRoot));

        // Takes ownership of 'solutions[ix]' and 'nextPlanRoot'.
        multiPlanStage->addPlan(solutions.releaseAt(ix), nextPlanRoot, _ws);
    }

    // Delegate to the MultiPlanStage's plan selection facility.
    Status pickBestPlanStatus = multiPlanStage->pickBestPlan(yieldPolicy);
    if (!pickBestPlanStatus.isOK()) {
        return pickBestPlanStatus;
    }

    LOG(1) << "Replanning " << _canonicalQuery->toStringShort()
           << " resulted in plan with summary: " << Explain::getPlanSummary(child().get())
           << ", which " << (shouldCache ? "has" : "has not") << " been written to the cache";
    return Status::OK();
}

bool CachedPlanStage::isEOF() {
    return _results.empty() && child()->isEOF();
}

PlanStage::StageState CachedPlanStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // First exhaust any results buffered during the trial period.
    if (!_results.empty()) {
        *out = _results.front();
        _results.pop_front();
        return PlanStage::ADVANCED;
    }

    // Nothing left in trial period buffer.
    return child()->work(out);
}

void CachedPlanStage::doInvalidate(OperationContext* txn,
                                   const RecordId& dl,
                                   InvalidationType type) {
    for (auto it = _results.begin(); it != _results.end(); ++it) {
        WorkingSetMember* member = _ws->get(*it);
        if (member->hasRecordId() && member->recordId == dl) {
            WorkingSetCommon::fetchAndInvalidateRecordId(txn, member, _collection);
        }
    }
}

std::unique_ptr<PlanStageStats> CachedPlanStage::getStats() {
    _commonStats.isEOF = isEOF();

    std::unique_ptr<PlanStageStats> ret =
        stdx::make_unique<PlanStageStats>(_commonStats, STAGE_CACHED_PLAN);
    ret->specific = stdx::make_unique<CachedPlanStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());

    return ret;
}

const SpecificStats* CachedPlanStage::getSpecificStats() const {
    return &_specificStats;
}

void CachedPlanStage::updatePlanCache() {
    std::unique_ptr<PlanCacheEntryFeedback> feedback = stdx::make_unique<PlanCacheEntryFeedback>();
    feedback->stats = getStats();
    feedback->score = PlanRanker::scoreTree(feedback->stats->children[0].get());

    PlanCache* cache = _collection->infoCache()->getPlanCache();
    Status fbs = cache->feedback(*_canonicalQuery, feedback.release());
    if (!fbs.isOK()) {
        LOG(5) << _canonicalQuery->ns()
               << ": Failed to update cache with feedback: " << fbs.toString() << " - "
               << "(query: " << _canonicalQuery->getQueryObj()
               << "; sort: " << _canonicalQuery->getQueryRequest().getSort()
               << "; projection: " << _canonicalQuery->getQueryRequest().getProj()
               << ") is no longer in plan cache.";
    }
}

}  // namespace mongo
