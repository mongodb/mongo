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


#include "mongo/platform/basic.h"

#include "mongo/db/exec/cached_plan.h"

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

// static
const char* CachedPlanStage::kStageType = "CACHED_PLAN";

CachedPlanStage::CachedPlanStage(ExpressionContext* expCtx,
                                 const CollectionPtr& collection,
                                 WorkingSet* ws,
                                 CanonicalQuery* cq,
                                 const QueryPlannerParams& params,
                                 size_t decisionWorks,
                                 std::unique_ptr<PlanStage> root)
    : RequiresAllIndicesStage(kStageType, expCtx, collection),
      _ws(ws),
      _canonicalQuery(cq),
      _plannerParams(params),
      _decisionWorks(decisionWorks) {
    _children.emplace_back(std::move(root));
}

Status CachedPlanStage::pickBestPlan(PlanYieldPolicy* yieldPolicy) {
    // Adds the amount of time taken by pickBestPlan() to executionTimeMillis. There's lots of
    // execution work that happens here, so this is needed for the time accounting to
    // make sense.
    auto optTimer = getOptTimer();

    // During plan selection, the list of indices we are using to plan must remain stable, so the
    // query will die during yield recovery if any index has been dropped. However, once plan
    // selection completes successfully, we no longer need all indices to stick around. The selected
    // plan should safely die on yield recovery if it is using the dropped index.
    //
    // Dismiss the requirement that no indices can be dropped when this method returns.
    ON_BLOCK_EXIT([this] { releaseAllIndicesRequirement(); });

    // If we work this many times during the trial period, then we will replan the
    // query from scratch.
    size_t maxWorksBeforeReplan =
        static_cast<size_t>(internalQueryCacheEvictionRatio * _decisionWorks);

    // The trial period ends without replanning if the cached plan produces this many results.
    size_t numResults = trial_period::getTrialPeriodNumToReturn(*_canonicalQuery);

    for (size_t i = 0; i < maxWorksBeforeReplan; ++i) {
        // Might need to yield between calls to work due to the timer elapsing.
        Status yieldStatus = tryYield(yieldPolicy);
        if (!yieldStatus.isOK()) {
            return yieldStatus;
        }

        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState state;
        try {
            state = child()->work(&id);
        } catch (const ExceptionFor<ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed>& ex) {
            // The plan failed by hitting the limit we impose on memory consumption. It's possible
            // that a different plan is less resource-intensive, so we fall back to replanning the
            // whole query. We neither evict the existing cache entry nor cache the result of
            // replanning.
            auto explainer = plan_explainer_factory::make(child().get());
            LOGV2_DEBUG(20579,
                        1,
                        "Execution of cached plan failed, falling back to replan",
                        "query"_attr = redact(_canonicalQuery->toStringShort()),
                        "planSummary"_attr = explainer->getPlanSummary(),
                        "status"_attr = redact(ex.toStatus()));

            const bool shouldCache = false;
            return replan(yieldPolicy,
                          shouldCache,
                          str::stream() << "cached plan returned: " << ex.toStatus());
        }

        if (PlanStage::ADVANCED == state) {
            // Save result for later.
            WorkingSetMember* member = _ws->get(id);
            // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we yield.
            member->makeObjOwnedIfNeeded();
            _results.push(id);

            if (_results.size() >= numResults) {
                // Once a plan returns enough results, stop working. There is no need to replan.
                _bestPlanChosen = true;
                return Status::OK();
            }
        } else if (PlanStage::IS_EOF == state) {
            // Cached plan hit EOF quickly enough. No need to replan.
            _bestPlanChosen = true;
            return Status::OK();
        } else if (PlanStage::NEED_YIELD == state) {
            invariant(id == WorkingSet::INVALID_ID);
            // Run-time plan selection occurs before a WriteUnitOfWork is opened and it's not
            // subject to TemporarilyUnavailableException's.
            invariant(!expCtx()->getTemporarilyUnavailableException());
            if (!yieldPolicy->canAutoYield()) {
                throwWriteConflictException();
            }

            if (yieldPolicy->canAutoYield()) {
                yieldPolicy->forceYield();
            }

            Status yieldStatus = tryYield(yieldPolicy);
            if (!yieldStatus.isOK()) {
                return yieldStatus;
            }
        } else {
            invariant(PlanStage::NEED_TIME == state);
        }
    }

    // If we're here, the trial period took more than 'maxWorksBeforeReplan' work cycles. This
    // plan is taking too long, so we replan from scratch.
    auto explainer = plan_explainer_factory::make(child().get());
    LOGV2_DEBUG(20580,
                1,
                "Evicting cache entry and replanning query",
                "maxWorksBeforeReplan"_attr = maxWorksBeforeReplan,
                "decisionWorks"_attr = _decisionWorks,
                "query"_attr = redact(_canonicalQuery->toStringShort()),
                "planSummary"_attr = explainer->getPlanSummary());

    const bool shouldCache = true;
    return replan(
        yieldPolicy,
        shouldCache,
        str::stream()
            << "cached plan was less efficient than expected: expected trial execution to take "
            << _decisionWorks << " works but it took at least " << maxWorksBeforeReplan
            << " works");
}

Status CachedPlanStage::tryYield(PlanYieldPolicy* yieldPolicy) {
    // These are the conditions which can cause us to yield:
    //   1) The yield policy's timer elapsed, or
    //   2) some stage requested a yield, or
    //   3) we need to yield and retry due to a WriteConflictException.
    // In all cases, the actual yielding happens here.
    if (yieldPolicy->shouldYieldOrInterrupt(expCtx()->opCtx)) {
        // Here's where we yield.
        return yieldPolicy->yieldOrInterrupt(expCtx()->opCtx);
    }

    return Status::OK();
}

Status CachedPlanStage::replan(PlanYieldPolicy* yieldPolicy, bool shouldCache, std::string reason) {
    // We're going to start over with a new plan. Clear out info from our old plan.
    {
        std::queue<WorkingSetID> emptyQueue;
        _results.swap(emptyQueue);
    }
    _ws->clear();
    _children.clear();

    _specificStats.replanReason = std::move(reason);

    if (shouldCache) {
        // Deactivate the current cache entry.
        const auto& coll = collection();
        auto cache = CollectionQueryInfo::get(coll).getPlanCache();
        cache->deactivate(plan_cache_key_factory::make<PlanCacheKey>(*_canonicalQuery, coll));
    }

    // Use the query planning module to plan the whole query.
    auto statusWithMultiPlanSolns = QueryPlanner::plan(*_canonicalQuery, _plannerParams);
    if (!statusWithMultiPlanSolns.isOK()) {
        return statusWithMultiPlanSolns.getStatus().withContext(
            str::stream() << "error processing query: " << _canonicalQuery->toString()
                          << " planner returned error");
    }
    auto solutions = std::move(statusWithMultiPlanSolns.getValue());

    if (1 == solutions.size()) {
        // Only one possible plan. Build the stages from the solution.
        auto&& newRoot = stage_builder::buildClassicExecutableTree(
            expCtx()->opCtx, collection(), *_canonicalQuery, *solutions[0], _ws);
        _children.emplace_back(std::move(newRoot));
        _replannedQs = std::move(solutions.back());
        solutions.pop_back();

        auto explainer = plan_explainer_factory::make(child().get());
        LOGV2_DEBUG(
            20581,
            1,
            "Replanning of query resulted in single query solution, which will not be cached.",
            "query"_attr = redact(_canonicalQuery->toStringShort()),
            "planSummary"_attr = explainer->getPlanSummary(),
            "shouldCache"_attr = (shouldCache ? "yes" : "no"));
        _bestPlanChosen = true;
        return Status::OK();
    }

    // Many solutions. Create a MultiPlanStage to pick the best, update the cache,
    // and so on. The working set will be shared by all candidate plans.
    auto cachingMode = shouldCache ? PlanCachingMode::AlwaysCache : PlanCachingMode::NeverCache;
    _children.emplace_back(
        new MultiPlanStage(expCtx(), collection(), _canonicalQuery, cachingMode));
    MultiPlanStage* multiPlanStage = static_cast<MultiPlanStage*>(child().get());

    for (size_t ix = 0; ix < solutions.size(); ++ix) {
        solutions[ix]->indexFilterApplied = _plannerParams.indexFiltersApplied;

        auto&& nextPlanRoot = stage_builder::buildClassicExecutableTree(
            expCtx()->opCtx, collection(), *_canonicalQuery, *solutions[ix], _ws);

        multiPlanStage->addPlan(std::move(solutions[ix]), std::move(nextPlanRoot), _ws);
    }

    // Delegate to the MultiPlanStage's plan selection facility.
    Status pickBestPlanStatus = multiPlanStage->pickBestPlan(yieldPolicy);
    if (!pickBestPlanStatus.isOK()) {
        return pickBestPlanStatus;
    }

    auto explainer = plan_explainer_factory::make(child().get());
    LOGV2_DEBUG(20582,
                1,
                "Query plan after replanning and its cache status",
                "query"_attr = redact(_canonicalQuery->toStringShort()),
                "planSummary"_attr = explainer->getPlanSummary(),
                "shouldCache"_attr = (shouldCache ? "yes" : "no"));
    _bestPlanChosen = true;
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
        _results.pop();
        return PlanStage::ADVANCED;
    }

    // Nothing left in trial period buffer.
    return child()->work(out);
}

std::unique_ptr<PlanStageStats> CachedPlanStage::getStats() {
    _commonStats.isEOF = isEOF();

    std::unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_CACHED_PLAN);
    ret->specific = std::make_unique<CachedPlanStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());

    return ret;
}

const SpecificStats* CachedPlanStage::getSpecificStats() const {
    return &_specificStats;
}
}  // namespace mongo
