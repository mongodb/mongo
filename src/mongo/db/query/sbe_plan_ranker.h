// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/record_id.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

namespace mongo::sbe::plan_ranker {

/**
 * Structure with data needed to execute the multi-planning trial period for a single SBE candidate
 * plan.
 */
struct CandidatePlanData {
    stage_builder::PlanStageData stageData;
    std::unique_ptr<TrialRunTracker> tracker;
    value::SlotAccessor* resultAccessor = nullptr;
    value::SlotAccessor* recordIdAccessor = nullptr;
    bool open = false;
};

using CandidatePlan =
    mongo::plan_ranker::BaseCandidatePlan<std::unique_ptr<mongo::sbe::PlanStage>,
                                          std::pair<BSONObj, boost::optional<RecordId>>,
                                          CandidatePlanData>;

}  // namespace mongo::sbe::plan_ranker
