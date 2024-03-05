/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/sbe_trial_runtime_executor.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <deque>
#include <tuple>


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/plan_executor_sbe.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/record_id.h"
#include "mongo/util/assert_util.h"

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

        auto state = fetchNext(candidate->root.get(),
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
        candidate->results.push_back({std::move(obj), {recordIdSlot != nullptr, recordId}});
        if (candidate->results.size() >= maxNumResults) {
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
    invariant(root);
    invariant(data);

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
    while (fetchNextDocument(candidate, maxNumResults)) {
    }
}
}  // namespace mongo::sbe
