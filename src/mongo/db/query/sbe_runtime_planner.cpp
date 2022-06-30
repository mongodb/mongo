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

#include "mongo/db/query/sbe_runtime_planner.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/histogram_server_status_metric.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/query/plan_executor_sbe.h"

namespace mongo::sbe {
namespace {

/**
 * Aggregation of the total number of microseconds spent (in SBE multiplanner).
 */
CounterMetric sbeMicrosTotal("query.multiPlanner.sbeMicros");

/**
 * Aggregation of the total number of reads done (in SBE multiplanner).
 */
CounterMetric sbeNumReadsTotal("query.multiPlanner.sbeNumReads");

/**
 * Aggregation of the total number of invocations (of the SBE multiplanner).
 */
CounterMetric sbeCount("query.multiPlanner.sbeCount");

/**
 * An element in this histogram is the number of microseconds spent in an invocation (of the SBE
 * multiplanner).
 */
HistogramServerStatusMetric sbeMicrosHistogram("query.multiPlanner.histograms.sbeMicros",
                                               HistogramServerStatusMetric::pow(11, 1024, 4));

/**
 * An element in this histogram is the number of reads performance during an invocation (of the SBE
 * multiplanner).
 */
HistogramServerStatusMetric sbeNumReadsHistogram("query.multiPlanner.histograms.sbeNumReads",
                                                 HistogramServerStatusMetric::pow(9, 128, 2));

/**
 * An element in this histogram is the number of plans in the candidate set of an invocation (of the
 * SBE multiplanner).
 */
HistogramServerStatusMetric sbeNumPlansHistogram("query.multiPlanner.histograms.sbeNumPlans",
                                                 HistogramServerStatusMetric::pow(5, 2, 2));

/**
 * Fetches a next document form the given plan stage tree and returns 'true' if the plan stage
 * returns EOF, or throws 'TrialRunTracker::EarlyExitException' exception. Otherwise, the
 * loaded document is placed into the candidate's plan result queue.
 *
 * If the plan stage throws a 'QueryExceededMemoryLimitNoDiskUseAllowed', it will be caught and the
 * 'candidate->failed' flag will be set to 'true', and the 'numFailures' parameter incremented by 1.
 * This failure is considered recoverable, as another candidate plan may require less memory, or may
 * not contain a stage requiring spilling to disk at all.
 */
enum class FetchDocStatus {
    done = 0,
    exitedEarly,
    inProgress,
};
FetchDocStatus fetchNextDocument(
    plan_ranker::CandidatePlan* candidate,
    const std::pair<value::SlotAccessor*, value::SlotAccessor*>& slots) {
    try {
        BSONObj obj;
        RecordId recordId;

        auto [resultSlot, recordIdSlot] = slots;
        auto state = fetchNext(candidate->root.get(),
                               resultSlot,
                               recordIdSlot,
                               &obj,
                               recordIdSlot ? &recordId : nullptr,
                               true /* must return owned BSON */);
        if (state == PlanState::IS_EOF) {
            candidate->root->close();
            return FetchDocStatus::done;
        }

        invariant(state == PlanState::ADVANCED);
        invariant(obj.isOwned());
        candidate->results.push_back({obj, {recordIdSlot != nullptr, recordId}});
    } catch (const ExceptionFor<ErrorCodes::QueryTrialRunCompleted>&) {
        return FetchDocStatus::exitedEarly;
    } catch (const ExceptionFor<ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed>& ex) {
        candidate->root->close();
        candidate->status = ex.toStatus();
    }
    return FetchDocStatus::inProgress;
}
}  // namespace

StatusWith<std::tuple<value::SlotAccessor*, value::SlotAccessor*, bool>>
BaseRuntimePlanner::prepareExecutionPlan(PlanStage* root,
                                         stage_builder::PlanStageData* data,
                                         const bool preparingFromCache) const {
    invariant(root);
    invariant(data);

    stage_builder::prepareSlotBasedExecutableTree(
        _opCtx, root, data, _cq, _collections, _yieldPolicy, preparingFromCache);

    value::SlotAccessor* resultSlot{nullptr};
    if (auto slot = data->outputs.getIfExists(stage_builder::PlanStageSlots::kResult); slot) {
        resultSlot = root->getAccessor(data->ctx, *slot);
        tassert(4822871, "Query does not have a result slot.", resultSlot);
    }

    value::SlotAccessor* recordIdSlot{nullptr};
    if (auto slot = data->outputs.getIfExists(stage_builder::PlanStageSlots::kRecordId); slot) {
        recordIdSlot = root->getAccessor(data->ctx, *slot);
        tassert(4822872, "Query does not have a recordId slot.", recordIdSlot);
    }

    auto exitedEarly{false};
    try {
        root->open(false);
    } catch (const ExceptionFor<ErrorCodes::QueryTrialRunCompleted>&) {
        exitedEarly = true;
    } catch (const ExceptionFor<ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed>& ex) {
        root->close();
        return ex.toStatus();
    }

    return std::make_tuple(resultSlot, recordIdSlot, exitedEarly);
}

void BaseRuntimePlanner::executeCandidateTrial(plan_ranker::CandidatePlan* candidate,
                                               size_t maxNumResults,
                                               const bool isCachedPlanTrial) {
    _indexExistenceChecker.check();

    auto status = prepareExecutionPlan(candidate->root.get(), &candidate->data, isCachedPlanTrial);
    if (!status.isOK()) {
        candidate->status = status.getStatus();
        return;
    }

    auto [resultAccessor, recordIdAccessor, exitedEarly] = status.getValue();
    if (exitedEarly) {
        candidate->exitedEarly = true;
        return;
    }

    for (size_t i = 0; i < maxNumResults && candidate->status.isOK(); ++i) {
        FetchDocStatus fetch =
            fetchNextDocument(candidate, std::make_pair(resultAccessor, recordIdAccessor));
        if (fetch == FetchDocStatus::done || fetch == FetchDocStatus::exitedEarly) {
            candidate->exitedEarly = (fetch == FetchDocStatus::exitedEarly);
            return;
        }
    }
}

std::vector<plan_ranker::CandidatePlan> BaseRuntimePlanner::collectExecutionStats(
    std::vector<std::unique_ptr<QuerySolution>> solutions,
    std::vector<std::pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>> roots,
    size_t maxTrialPeriodNumReads) {
    invariant(solutions.size() == roots.size());

    std::vector<plan_ranker::CandidatePlan> candidates;
    std::vector<std::pair<value::SlotAccessor*, value::SlotAccessor*>> accessors;

    const auto maxNumResults{trial_period::getTrialPeriodNumToReturn(_cq)};

    auto tickSource = _opCtx->getServiceContext()->getTickSource();
    auto startTicks = tickSource->getTicks();
    sbeNumPlansHistogram.increment(solutions.size());
    sbeCount.increment();

    // Determine which plans are blocking and which are non blocking. The non blocking plans will
    // be run first in order to provide an upper bound on the number of reads allowed for the
    // blocking plans.
    std::vector<size_t> nonBlockingPlanIndexes;
    std::vector<size_t> blockingPlanIndexes;
    for (size_t index = 0; index < solutions.size(); ++index) {
        if (solutions[index]->hasBlockingStage) {
            blockingPlanIndexes.push_back(index);
        } else {
            nonBlockingPlanIndexes.push_back(index);
        }
    }

    // If all the plans are blocking, then the trial period risks going on for too long. Because the
    // plans are blocking, they may not provide 'maxNumResults' within the allotted budget of reads.
    // We could end up in a situation where each plan's trial period runs for a long time,
    // substantially slowing down the multi-planning process. For this reason, when all the plans
    // are blocking, we pass 'maxNumResults' to the trial run tracker. This causes the sort stage to
    // exit early as soon as it sees 'maxNumResults' _input_ values, which keeps the trial period
    // shorter.
    //
    // On the other hand, if we have a mix of blocking and non-blocking plans, we don't want the
    // sort stage to exit early based on the number of input rows it observes. This could cause the
    // trial period for the blocking plans to run for a much shorter timeframe than the non-blocking
    // plans. This leads to an apples-to-oranges comparison between the blocking and non-blocking
    // plans which could artificially favor the blocking plans.
    const size_t trackerResultsBudget = nonBlockingPlanIndexes.empty() ? maxNumResults : 0;

    uint64_t totalNumReads = 0;

    auto runPlans = [&](const std::vector<size_t>& planIndexes, size_t& maxNumReads) -> void {
        for (auto planIndex : planIndexes) {
            // Prepare the plan.
            auto&& [root, data] = roots[planIndex];
            // Make a copy of the original plan. This pristine copy will be inserted into the plan
            // cache if this candidate becomes the winner.
            auto origPlan =
                std::make_pair<std::unique_ptr<PlanStage>, stage_builder::PlanStageData>(
                    root->clone(), stage_builder::PlanStageData(data));

            // Attach a unique TrialRunTracker to the plan, which is configured to use at most
            // 'maxNumReads' reads.
            auto tracker = std::make_unique<TrialRunTracker>(trackerResultsBudget, maxNumReads);
            ON_BLOCK_EXIT([rootPtr = root.get()] { rootPtr->detachFromTrialRunTracker(); });
            root->attachToTrialRunTracker(tracker.get());

            candidates.push_back({std::move(solutions[planIndex]),
                                  std::move(root),
                                  std::move(data),
                                  false /* exitedEarly */,
                                  Status::OK()});
            auto& currentCandidate = candidates.back();
            // Store the original plan in the CandidatePlan.
            currentCandidate.clonedPlan.emplace(std::move(origPlan));
            executeCandidateTrial(&currentCandidate, maxNumResults, /*isCachedPlanTrial*/ false);

            auto reads = tracker->getMetric<TrialRunTracker::TrialRunMetric::kNumReads>();
            // We intentionally increment the metrics outside of the isOk/existedEarly check.
            totalNumReads += reads;

            // Reduce the number of reads the next candidates are allocated if this candidate is
            // more efficient than the current bound.
            if (currentCandidate.status.isOK() && !currentCandidate.exitedEarly) {
                maxNumReads = std::min(maxNumReads, reads);
            }
        }
    };

    runPlans(nonBlockingPlanIndexes, maxTrialPeriodNumReads);
    runPlans(blockingPlanIndexes, maxTrialPeriodNumReads);

    sbeNumReadsHistogram.increment(totalNumReads);
    sbeNumReadsTotal.increment(totalNumReads);

    auto durationMicros = durationCount<Microseconds>(
        tickSource->ticksTo<Microseconds>(tickSource->getTicks() - startTicks));
    sbeMicrosHistogram.increment(durationMicros);
    sbeMicrosTotal.increment(durationMicros);

    return candidates;
}
}  // namespace mongo::sbe
