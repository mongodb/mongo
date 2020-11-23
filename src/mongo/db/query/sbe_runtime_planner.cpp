/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/query/sbe_runtime_planner.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/query/plan_executor_sbe.h"

namespace mongo::sbe {
namespace {
/**
 * Fetches a next document form the given plan stage tree and returns 'true' if the plan stage
 * returns EOF, or throws 'TrialRunProgressTracker::EarlyExitException' exception. Otherwise, the
 * loaded document is placed into the candidate's plan result queue.
 *
 * If the plan stage throws a 'QueryExceededMemoryLimitNoDiskUseAllowed', it will be caught and the
 * 'candidate->failed' flag will be set to 'true', and the 'numFailures' parameter incremented by 1.
 * This failure is considered recoverable, as another candidate plan may require less memory, or may
 * not contain a stage requiring spilling to disk at all.
 */
bool fetchNextDocument(plan_ranker::CandidatePlan* candidate,
                       const std::pair<sbe::value::SlotAccessor*, sbe::value::SlotAccessor*>& slots,
                       size_t* numFailures) {
    try {
        BSONObj obj;
        RecordId recordId;

        auto [resultSlot, recordIdSlot] = slots;
        auto state = fetchNext(candidate->root.get(),
                               resultSlot,
                               recordIdSlot,
                               &obj,
                               recordIdSlot ? &recordId : nullptr);
        if (state == sbe::PlanState::IS_EOF) {
            candidate->root->close();
            return true;
        }

        invariant(state == sbe::PlanState::ADVANCED);
        candidate->results.push({obj.getOwned(), {recordIdSlot != nullptr, recordId}});
    } catch (const ExceptionFor<ErrorCodes::QueryTrialRunCompleted>&) {
        candidate->exitedEarly = true;
        return true;
    } catch (const ExceptionFor<ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed>&) {
        candidate->root->close();
        candidate->failed = true;
        ++(*numFailures);
    }
    return false;
}
}  // namespace

std::tuple<sbe::value::SlotAccessor*, sbe::value::SlotAccessor*, bool>
BaseRuntimePlanner::prepareExecutionPlan(PlanStage* root,
                                         stage_builder::PlanStageData* data) const {
    invariant(root);
    invariant(data);

    root->prepare(data->ctx);

    sbe::value::SlotAccessor* resultSlot{nullptr};
    if (auto slot = data->outputs.getIfExists(stage_builder::PlanStageSlots::kResult); slot) {
        resultSlot = root->getAccessor(data->ctx, *slot);
        uassert(4822871, "Query does not have result slot.", resultSlot);
    }

    sbe::value::SlotAccessor* recordIdSlot{nullptr};
    if (auto slot = data->outputs.getIfExists(stage_builder::PlanStageSlots::kRecordId); slot) {
        recordIdSlot = root->getAccessor(data->ctx, *slot);
        uassert(4822872, "Query does not have record ID slot.", recordIdSlot);
    }

    root->attachFromOperationContext(_opCtx);

    auto exitedEarly{false};
    try {
        root->open(false);
    } catch (const ExceptionFor<ErrorCodes::QueryTrialRunCompleted>&) {
        exitedEarly = true;
    }

    return {resultSlot, recordIdSlot, exitedEarly};
}

std::vector<plan_ranker::CandidatePlan> BaseRuntimePlanner::collectExecutionStats(
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots) {
    invariant(solutions.size() == roots.size());

    std::vector<plan_ranker::CandidatePlan> candidates;
    std::vector<std::pair<sbe::value::SlotAccessor*, sbe::value::SlotAccessor*>> slots;

    for (size_t ix = 0; ix < roots.size(); ++ix) {
        auto&& [root, data] = roots[ix];
        auto [resultSlot, recordIdSlot, exitedEarly] = prepareExecutionPlan(root.get(), &data);

        candidates.push_back(
            {std::move(solutions[ix]), std::move(root), std::move(data), exitedEarly});
        slots.push_back({resultSlot, recordIdSlot});
    }

    auto done{false};
    size_t numFailures{0};
    const auto maxNumResults{trial_period::getTrialPeriodNumToReturn(_cq)};
    for (size_t it = 0; it < maxNumResults && !done; ++it) {
        for (size_t ix = 0; ix < candidates.size(); ++ix) {
            // Even if we had a candidate plan that exited early, we still want continue the trial
            // run as the early exited plan may not be the best. E.g., it could be blocked in a SORT
            // stage until one of the trial period metrics was reached, causing the plan to raise an
            // early exit exception and return control back to the runtime planner. If that happens,
            // we need to continue and complete the trial period for all candidates, as some of them
            // may have a better cost.
            if (candidates[ix].failed || candidates[ix].exitedEarly) {
                continue;
            }

            done |= fetchNextDocument(&candidates[ix], slots[ix], &numFailures) ||
                (numFailures == candidates.size());
        }
    }

    // Make sure we have at least one plan which hasn't failed.
    uassert(4822873,
            "Runtime planner encountered a failure while collecting execution stats",
            numFailures != candidates.size());

    return candidates;
}
}  // namespace mongo::sbe
