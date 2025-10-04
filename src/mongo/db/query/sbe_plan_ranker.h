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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/record_id.h"

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
