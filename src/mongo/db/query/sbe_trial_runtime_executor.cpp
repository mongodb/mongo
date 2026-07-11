// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/sbe_trial_runtime_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/plan_executor_sbe.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/record_id.h"
#include "mongo/util/assert_util.h"

#include <deque>
#include <tuple>


namespace mongo::sbe {

bool TrialRuntimeExecutor::fetchNextDocument(plan_ranker::CandidatePlan* candidate,
                                             size_t maxNumResults) {
    auto* resultSlot = candidate->data.resultAccessor;
    auto* recordIdSlot = candidate->data.recordIdAccessor;
    try {
        if (!candidate->data.open) {
            candidate->root->open(false);
            candidate->data.open = true;
        }

        BSONObj obj;
        RecordId recordId;

        // SBE plan might be followed by an non-pushed down aggregation pipeline. In that case we
        // allow intermediate document to be large.
        auto state = fetchNext<BSONObj::LargeSizeTrait>(candidate->root.get(),
                                                        resultSlot,
                                                        recordIdSlot,
                                                        &obj,
                                                        recordIdSlot ? &recordId : nullptr,
                                                        true /* must return owned BSON */);
        if (state == PlanState::IS_EOF) {
            candidate->root->close();
            return false;
        }

        invariant(state == PlanState::ADVANCED);
        invariant(obj.isOwned());
        _stashSizeBytes += obj.objsize();
        candidate->results.push_back({std::move(obj), {recordIdSlot != nullptr, recordId}});
        if (candidate->results.size() >= maxNumResults ||
            candidate->data.tracker->metricReached<TrialRunTracker::kNumResults>() ||
            _stashSizeBytes >= _stashSizeMaxBytes) {
            return false;
        }
    } catch (const ExceptionFor<ErrorCodes::QueryTrialRunCompleted>&) {
        candidate->exitedEarly = true;
        return false;
    } catch (const ExceptionFor<ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed>& ex) {
        candidate->root->close();
        candidate->status = ex.toStatus();
        return false;
    }
    return true;
}

std::pair<value::SlotAccessor*, value::SlotAccessor*> TrialRuntimeExecutor::prepareExecutionPlan(
    PlanStage* root,
    stage_builder::PlanStageData* data,
    const bool preparingFromCache,
    RemoteCursorMap* remoteCursors) const {
    tassert(11321202, "root must not be null", root);
    tassert(11321203, "data must not be null", data);

    stage_builder::prepareSlotBasedExecutableTree(
        _opCtx, root, data, _cq, _collections, _yieldPolicy, preparingFromCache, remoteCursors);

    value::SlotAccessor* resultSlot{nullptr};
    if (auto slot = data->staticData->resultSlot) {
        resultSlot = root->getAccessor(data->env.ctx, *slot);
        tassert(4822871, "Query does not have a result slot.", resultSlot);
    }

    value::SlotAccessor* recordIdSlot{nullptr};
    if (auto slot = data->staticData->recordIdSlot) {
        recordIdSlot = root->getAccessor(data->env.ctx, *slot);
        tassert(4822872, "Query does not have a recordId slot.", recordIdSlot);
    }

    return std::make_pair(resultSlot, recordIdSlot);
}

void TrialRuntimeExecutor::prepareCandidate(plan_ranker::CandidatePlan* candidate,
                                            bool preparingFromCache) {
    _indexExistenceChecker.check(_opCtx, _collections);
    std::tie(candidate->data.resultAccessor, candidate->data.recordIdAccessor) =
        prepareExecutionPlan(candidate->root.get(), &candidate->data.stageData, preparingFromCache);
}

void TrialRuntimeExecutor::executeCachedCandidateTrial(plan_ranker::CandidatePlan* candidate,
                                                       size_t maxNumResults) {
    prepareCandidate(candidate, true /*preparingFromCache*/);
    _stashSizeBytes = 0;
    while (fetchNextDocument(candidate, maxNumResults)) {
    }
}
}  // namespace mongo::sbe
