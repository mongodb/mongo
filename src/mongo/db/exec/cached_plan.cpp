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
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/log.h"

namespace mongo {

using std::auto_ptr;
using std::vector;

// static
const char* CachedPlanStage::kStageType = "CACHED_PLAN";

CachedPlanStage::CachedPlanStage(OperationContext* txn,
                                 Collection* collection,
                                 WorkingSet* ws,
                                 CanonicalQuery* cq,
                                 const QueryPlannerParams& params,
                                 size_t decisionWorks,
                                 PlanStage* mainChild,
                                 QuerySolution* mainQs,
                                 PlanStage* backupChild,
                                 QuerySolution* backupQs)
    : _txn(txn),
      _collection(collection),
      _ws(ws),
      _canonicalQuery(cq),
      _plannerParams(params),
      _replanningEnabled(internalQueryCacheReplanningEnabled),
      _decisionWorks(decisionWorks),
      _mainQs(mainQs),
      _backupQs(backupQs),
      _mainChildPlan(mainChild),
      _backupChildPlan(backupChild),
      _usingBackupChild(false),
      _alreadyProduced(false),
      _updatedCache(false),
      _killed(false),
      _commonStats(kStageType) {}

CachedPlanStage::~CachedPlanStage() {
    // We may have produced all necessary results without hitting EOF. In this case, we still
    // want to update the cache with feedback.
    //
    // We can't touch the plan cache if we've been killed.
    if (!_updatedCache && !_killed) {
        updateCache();
    }
}

bool CachedPlanStage::isEOF() {
    if (_killed) {
        return true;
    }

    if (!_results.empty()) {
        return false;
    }

    return getActiveChild()->isEOF();
}

Status CachedPlanStage::pickBestPlan(PlanYieldPolicy* yieldPolicy) {
    // If replanning is disabled, then this is a no-op.
    if (!_replanningEnabled) {
        return Status::OK();
    }

    // Adds the amount of time taken by pickBestPlan() to executionTimeMillis. There's lots of
    // execution work that happens here, so this is needed for the time accounting to
    // make sense.
    ScopedTimer timer(&_commonStats.executionTimeMillis);

    // If we work this many times during the trial period, then we will replan the
    // query from scratch.
    size_t maxWorksBeforeReplan =
        static_cast<size_t>(internalQueryCacheEvictionRatio * _decisionWorks);

    // The trial period ends without replanning if the cached plan produces this many results
    size_t numResults = MultiPlanStage::getTrialPeriodNumToReturn(*_canonicalQuery);

    for (size_t i = 0; i < maxWorksBeforeReplan; ++i) {
        // Might need to yield between calls to work due to the timer elapsing.
        Status yieldStatus = tryYield(yieldPolicy);
        if (!yieldStatus.isOK()) {
            return yieldStatus;
        }

        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = _mainChildPlan->work(&id);

        if (PlanStage::ADVANCED == state) {
            // Save result for later.
            _results.push_back(id);

            if (_results.size() >= numResults) {
                // Once a plan returns enough results, stop working. Update cache with stats
                // from this run and return.
                updateCache();
                return Status::OK();
            }
        } else if (PlanStage::IS_EOF == state) {
            // Cached plan hit EOF quickly enough. No need to replan. Update cache with stats
            // from this run and return.
            updateCache();
            return Status::OK();
        } else if (PlanStage::NEED_FETCH == state) {
            WorkingSetMember* member = _ws->get(id);
            invariant(member->hasFetcher());
            // Transfer ownership of the fetcher and yield.
            _fetcher.reset(member->releaseFetcher());
            Status fetchYieldStatus = tryYield(yieldPolicy);
            if (!fetchYieldStatus.isOK()) {
                return fetchYieldStatus;
            }
        } else if (PlanStage::FAILURE == state) {
            // On failure, fall back to replanning the whole query. We neither evict the
            // existing cache entry nor cache the result of replanning.
            BSONObj statusObj;
            WorkingSetCommon::getStatusMemberObject(*_ws, id, &statusObj);

            LOG(1) << "Execution of cached plan failed, falling back to replan."
                   << " query: " << _canonicalQuery->toStringShort()
                   << " planSummary: " << Explain::getPlanSummary(_mainChildPlan.get())
                   << " status: " << statusObj;

            const bool shouldCache = false;
            return replan(yieldPolicy, shouldCache);
        } else if (PlanStage::DEAD == state) {
            return Status(ErrorCodes::OperationFailed,
                          "Executor killed during cached plan trial period");
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
           << " plan summary before replan: " << Explain::getPlanSummary(_mainChildPlan.get());

    const bool shouldCache = true;
    return replan(yieldPolicy, shouldCache);
}

Status CachedPlanStage::tryYield(PlanYieldPolicy* yieldPolicy) {
    // These are the conditions which cause us to yield during plan selection if we have a
    // YIELD_AUTO policy:
    //   1) The yield policy's timer elapsed, or
    //   2) some stage requested a yield due to a document fetch (NEED_FETCH).
    // In both cases, the actual yielding happens here.
    if (NULL != yieldPolicy && (yieldPolicy->shouldYield() || NULL != _fetcher.get())) {
        // Here's where we yield.
        bool alive = yieldPolicy->yield(_fetcher.get());

        if (!alive) {
            return Status(ErrorCodes::QueryPlanKilled,
                          "PlanExecutor killed during cached plan trial period");
        }
    }

    // We're done using the fetcher, so it should be freed. We don't want to
    // use the same RecordFetcher twice.
    _fetcher.reset();

    return Status::OK();
}

Status CachedPlanStage::replan(PlanYieldPolicy* yieldPolicy, bool shouldCache) {
    // We're going to start over with a new plan. No need for only old buffered results.
    _results.clear();

    // Clear out the working set. We'll start with a fresh working set.
    _ws->clear();

    // No need for any existing child stages or QuerySolutions. We will create new ones from
    // scratch.
    _mainQs.reset();
    _backupQs.reset();
    _mainChildPlan.reset();
    _backupChildPlan.reset();

    // Remove the current plan cache entry for this shape. The plan cache entry could have
    // already been removed by another thread, so our removal won't necessarily succeed.
    if (shouldCache) {
        PlanCache* cache = _collection->infoCache()->getPlanCache();
        cache->remove(*_canonicalQuery);
    }

    // Use the query planning module to plan the whole query.
    std::vector<QuerySolution*> rawSolutions;
    Status status = QueryPlanner::plan(*_canonicalQuery, _plannerParams, &rawSolutions);
    if (!status.isOK()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "error processing query: " << _canonicalQuery->toString()
                                    << " planner returned error: " << status.reason());
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
        PlanStage* newRoot;
        // Only one possible plan. Build the stages from the solution.
        verify(StageBuilder::build(_txn, _collection, *solutions[0], _ws, &newRoot));
        _mainChildPlan.reset(newRoot);
        _mainQs.reset(solutions.popAndReleaseBack());

        LOG(1)
            << "Replanning of query resulted in single query solution, which will not be cached. "
            << _canonicalQuery->toStringShort()
            << " plan summary after replan: " << Explain::getPlanSummary(_mainChildPlan.get())
            << " previous cache entry evicted: " << (shouldCache ? "yes" : "no");
        return Status::OK();
    }

    // Many solutions. Create a MultiPlanStage to pick the best, update the cache,
    // and so on. The working set will be shared by all candidate plans.
    _mainChildPlan.reset(new MultiPlanStage(_txn, _collection, _canonicalQuery, shouldCache));
    MultiPlanStage* multiPlanStage = static_cast<MultiPlanStage*>(_mainChildPlan.get());

    for (size_t ix = 0; ix < solutions.size(); ++ix) {
        if (solutions[ix]->cacheData.get()) {
            solutions[ix]->cacheData->indexFilterApplied = _plannerParams.indexFiltersApplied;
        }

        PlanStage* nextPlanRoot;
        verify(StageBuilder::build(_txn, _collection, *solutions[ix], _ws, &nextPlanRoot));

        // Takes ownership of 'solutions[ix]' and 'nextPlanRoot'.
        multiPlanStage->addPlan(solutions.releaseAt(ix), nextPlanRoot, _ws);
    }

    // Delegate to the MultiPlanStage's plan selection facility.
    Status pickBestPlanStatus = multiPlanStage->pickBestPlan(yieldPolicy);
    if (!pickBestPlanStatus.isOK()) {
        return pickBestPlanStatus;
    }

    LOG(1) << "Replanning " << _canonicalQuery->toStringShort()
           << " resulted in plan with summary: " << Explain::getPlanSummary(_mainChildPlan.get())
           << ", which " << (shouldCache ? "has" : "has not") << " been written to the cache";
    return Status::OK();
}

PlanStage::StageState CachedPlanStage::work(WorkingSetID* out) {
    ++_commonStats.works;

    // Adds the amount of time taken by work() to executionTimeMillis.
    ScopedTimer timer(&_commonStats.executionTimeMillis);

    // We shouldn't be trying to work a dead plan.
    invariant(!_killed);

    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // First exhaust any results buffered during the trial period.
    if (!_results.empty()) {
        *out = _results.front();
        _results.pop_front();
        _commonStats.advanced++;
        _alreadyProduced = true;
        return PlanStage::ADVANCED;
    }

    StageState childStatus = getActiveChild()->work(out);

    if (PlanStage::ADVANCED == childStatus) {
        // we'll skip backupPlan processing now
        _alreadyProduced = true;
        _commonStats.advanced++;
    } else if (PlanStage::IS_EOF == childStatus) {
        updateCache();
    } else if (PlanStage::FAILURE == childStatus && !_alreadyProduced && !_usingBackupChild &&
               !_replanningEnabled && NULL != _backupChildPlan.get()) {
        // Switch the active child to the backup. Subsequent calls to work() will exercise
        // the backup plan. We are only willing to switch to the backup plan if replanning is
        // disabled.
        _usingBackupChild = true;
        _commonStats.needTime++;
        return PlanStage::NEED_TIME;
    } else if (PlanStage::NEED_FETCH == childStatus) {
        _commonStats.needFetch++;
    } else if (PlanStage::NEED_TIME == childStatus) {
        _commonStats.needTime++;
    }

    return childStatus;
}

void CachedPlanStage::saveState() {
    _mainChildPlan->saveState();

    if (NULL != _backupChildPlan.get()) {
        _backupChildPlan->saveState();
    }
    ++_commonStats.yields;
}

void CachedPlanStage::restoreState(OperationContext* opCtx) {
    _mainChildPlan->restoreState(opCtx);

    if (NULL != _backupChildPlan.get()) {
        _backupChildPlan->restoreState(opCtx);
    }
    ++_commonStats.unyields;
}

void CachedPlanStage::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
    if (!_usingBackupChild) {
        _mainChildPlan->invalidate(txn, dl, type);
    }
    if (NULL != _backupChildPlan.get()) {
        _backupChildPlan->invalidate(txn, dl, type);
    }
    ++_commonStats.invalidates;

    for (auto it = _results.begin(); it != _results.end(); ++it) {
        WorkingSetMember* member = _ws->get(*it);
        if (member->hasLoc() && member->loc == dl) {
            WorkingSetCommon::fetchAndInvalidateLoc(txn, member, _collection);
        }
    }
}

vector<PlanStage*> CachedPlanStage::getChildren() const {
    vector<PlanStage*> children;
    if (_usingBackupChild) {
        children.push_back(_backupChildPlan.get());
    } else {
        children.push_back(_mainChildPlan.get());
    }
    return children;
}

PlanStageStats* CachedPlanStage::getStats() {
    _commonStats.isEOF = isEOF();

    auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_CACHED_PLAN));
    ret->specific.reset(new CachedPlanStats(_specificStats));

    if (_usingBackupChild) {
        ret->children.push_back(_backupChildPlan->getStats());
    } else {
        ret->children.push_back(_mainChildPlan->getStats());
    }

    return ret.release();
}

const CommonStats* CachedPlanStage::getCommonStats() {
    return &_commonStats;
}

const SpecificStats* CachedPlanStage::getSpecificStats() {
    return &_specificStats;
}

void CachedPlanStage::updateCache() {
    _updatedCache = true;

    std::auto_ptr<PlanCacheEntryFeedback> feedback(new PlanCacheEntryFeedback());
    feedback->stats.reset(getStats());
    feedback->score = PlanRanker::scoreTree(feedback->stats.get());

    PlanCache* cache = _collection->infoCache()->getPlanCache();
    const bool allowedToEvict = !_replanningEnabled;
    Status fbs = cache->feedback(*_canonicalQuery, feedback.release(), allowedToEvict);

    if (!fbs.isOK()) {
        QLOG() << _canonicalQuery->ns()
               << ": Failed to update cache with feedback: " << fbs.toString() << " - "
               << "(query: " << _canonicalQuery->getQueryObj()
               << "; sort: " << _canonicalQuery->getParsed().getSort()
               << "; projection: " << _canonicalQuery->getParsed().getProj()
               << ") is no longer in plan cache.";
    }
}

PlanStage* CachedPlanStage::getActiveChild() const {
    return _usingBackupChild ? _backupChildPlan.get() : _mainChildPlan.get();
}

void CachedPlanStage::kill() {
    _killed = true;
}

}  // namespace mongo
