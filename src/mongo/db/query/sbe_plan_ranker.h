// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/result_materializer.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/record_id.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>
#include <variant>

#include <boost/optional/optional.hpp>

namespace mongo::sbe::plan_ranker {

/**
 * Structure with data needed to execute the multi-planning trial period for a single SBE candidate
 * plan.
 */
struct CandidatePlanData {
    stage_builder::PlanStageData stageData;
    std::unique_ptr<TrialRunTracker> tracker;
    boost::optional<ResultMaterializer> materializer;
    value::SlotAccessor* recordIdAccessor = nullptr;
    bool open = false;
};

/**
 * A result that a plan produced before its consumer asked for it, with its record id if the plan
 * produces one. The result is held in the format the consumer is expected to read it in: BSON for
 * PlanExecutor::getNext() and a Document for PlanExecutor::getNextDocument().
 */
using StashedResult = std::pair<std::variant<BSONObj, Document>, boost::optional<RecordId>>;

using CandidatePlan = mongo::plan_ranker::
    BaseCandidatePlan<std::unique_ptr<mongo::sbe::PlanStage>, StashedResult, CandidatePlanData>;

}  // namespace mongo::sbe::plan_ranker
