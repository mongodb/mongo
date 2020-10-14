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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/trial_run_progress_tracker.h"
#include "mongo/db/query/query_solution.h"

namespace mongo::stage_builder {

class PlanStageSlots;

/**
 * Generates an SBE plan stage sub-tree implementing an collection scan.
 *
 * On success, a tuple containing the following data is returned:
 *   * A slot to access a fetched document (a resultSlot)
 *   * A slot to access a recordId (a recordIdSlot)
 *   * An optional slot to access a latest oplog timestamp (oplogTsSlot), if we scan the oplog and
 *     were requested to track this data.
 *   * A generated PlanStage sub-tree.
 *
 * In cases of an error, throws.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateCollScan(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CollectionScanNode* csn,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    sbe::RuntimeEnvironment* env,
    bool isTailableResumeBranch,
    TrialRunProgressTracker* tracker);

}  // namespace mongo::stage_builder
