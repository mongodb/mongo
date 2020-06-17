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

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/id_generators.h"
#include "mongo/db/exec/trial_run_progress_tracker.h"
#include "mongo/db/query/query_solution.h"

namespace mongo::stage_builder {
/**
 * Generates an SBE plan stage sub-tree implementing an index scan.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateIndexScan(
    OperationContext* opCtx,
    const Collection* collection,
    const IndexScanNode* ixn,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::SpoolIdGenerator* spoolIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    TrialRunProgressTracker* tracker);

/**
 * Constructs the most simple version of an index scan from the single interval index bounds. The
 * generated subtree will have the following form:
 *
 *         nlj [] [lowKeySlot, highKeySlot]
 *              left
 *                  project [lowKeySlot = KS(...), highKeySlot = KS(...)]
 *                  limit 1
 *                  coscan
 *               right
 *                  ixseek lowKeySlot highKeySlot recordIdSlot [] @coll @index
 *
 * The inner branch of the nested loop join produces a single row with the low/high keys which is
 * fed to the ixscan.
 *
 * If 'recordSlot' is provided, than the corresponding slot will be filled out with each KeyString
 * in the index.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateSingleIntervalIndexScan(
    const Collection* collection,
    const std::string& indexName,
    bool forward,
    std::unique_ptr<KeyString::Value> lowKey,
    std::unique_ptr<KeyString::Value> highKey,
    boost::optional<sbe::value::SlotId> recordSlot,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    TrialRunProgressTracker* tracker);
}  // namespace mongo::stage_builder
