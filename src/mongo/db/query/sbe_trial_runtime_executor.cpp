// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/sbe_trial_runtime_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/query/plan_executor_sbe.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/record_id.h"
#include "mongo/util/assert_util.h"

#include <deque>
#include <tuple>
#include <variant>

#include <boost/optional/optional.hpp>


namespace mongo::sbe {

bool TrialRuntimeExecutor::fetchNextDocument(plan_ranker::CandidatePlan* candidate,
                                             size_t maxNumResults) {
    auto* materializer = candidate->data.materializer ? &*candidate->data.materializer : nullptr;
    auto* recordIdSlot = candidate->data.recordIdAccessor;
    try {
        if (!candidate->data.open) {
            candidate->root->open(false);
            candidate->data.open = true;
        }

        std::variant<BSONObj, Document> result;
        RecordId recordId;
        auto* recordIdOut = recordIdSlot ? &recordId : nullptr;

        PlanState state;
        if (_stashDocuments) {
            state = fetchNext(candidate->root.get(),
                              materializer,
                              recordIdSlot,
                              &result.emplace<Document>(),
                              recordIdOut);
        } else {
            state = fetchNext<BSONObj::LargeSizeTrait>(candidate->root.get(),
                                                       materializer,
                                                       recordIdSlot,
                                                       &std::get<BSONObj>(result),
                                                       recordIdOut,
                                                       true /* must return owned BSON */);
        }
        if (state == PlanState::IS_EOF) {
            candidate->root->close();
            candidate->stopCondition = MultiPlannerStopCondition::kEof;
            return false;
        }

        invariant(state == PlanState::ADVANCED);
        if (auto* obj = std::get_if<BSONObj>(&result)) {
            tassert(13911800, "Trial run must stash owned BSON", obj->isOwned());
            _stashSizeBytes += obj->objsize();
        } else {
            _stashSizeBytes += std::get<Document>(result).getApproximateSize();
        }
        candidate->results.emplace_back(
            std::move(result), boost::optional<RecordId>{recordIdSlot != nullptr, recordId});

        // Distinguish between reaching a full batch and hitting the stash size limit.
        if (candidate->results.size() >= maxNumResults ||
            candidate->data.tracker->metricReached<TrialRunTracker::kNumResults>()) {
            candidate->stopCondition = MultiPlannerStopCondition::kFullBatch;
            return false;
        }
        if (_stashSizeBytes >= _stashSizeMaxBytes) {
            candidate->stopCondition = MultiPlannerStopCondition::kStashSizeLimit;
            return false;
        }
    } catch (const ExceptionFor<ErrorCodes::QueryTrialRunCompleted>&) {
        candidate->stopCondition = MultiPlannerStopCondition::kExhaustedBudget;
        return false;
    } catch (const ExceptionFor<ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed>& ex) {
        candidate->root->close();
        candidate->status = ex.toStatus();
        candidate->stopCondition = MultiPlannerStopCondition::kFailed;
        return false;
    }
    return true;
}

std::pair<boost::optional<ResultMaterializer>, value::SlotAccessor*>
TrialRuntimeExecutor::prepareExecutionPlan(PlanStage* root,
                                           stage_builder::PlanStageData* data,
                                           const bool preparingFromCache,
                                           RemoteCursorMap* remoteCursors) const {
    tassert(11321202, "root must not be null", root);
    tassert(11321203, "data must not be null", data);

    stage_builder::prepareSlotBasedExecutableTree(
        _opCtx, root, data, _cq, _collections, _yieldPolicy, preparingFromCache, remoteCursors);

    boost::optional<ResultMaterializer> materializer;
    if (data->staticData->resultSlot) {
        materializer.emplace(
            stage_builder::bindResultMaterializer(root, data->env.ctx, *data->staticData));
    }

    value::SlotAccessor* recordIdSlot{nullptr};
    if (auto slot = data->staticData->recordIdSlot) {
        recordIdSlot = root->getAccessor(data->env.ctx, *slot);
        tassert(4822872, "Query does not have a recordId slot.", recordIdSlot);
    }

    return std::make_pair(std::move(materializer), recordIdSlot);
}

void TrialRuntimeExecutor::prepareCandidate(plan_ranker::CandidatePlan* candidate,
                                            bool preparingFromCache) {
    _indexExistenceChecker.check(_opCtx, _collections);
    std::tie(candidate->data.materializer, candidate->data.recordIdAccessor) =
        prepareExecutionPlan(candidate->root.get(), &candidate->data.stageData, preparingFromCache);
}

void TrialRuntimeExecutor::executeCachedCandidateTrial(plan_ranker::CandidatePlan* candidate,
                                                       size_t maxNumResults) {
    prepareCandidate(candidate, /*preparingFromCache=*/true);
    _stashSizeBytes = 0;
    while (fetchNextDocument(candidate, maxNumResults)) {
    }
}
}  // namespace mongo::sbe
