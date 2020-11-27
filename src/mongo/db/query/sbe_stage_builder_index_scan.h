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
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/query_solution.h"

namespace mongo::stage_builder {

class PlanStageReqs;
class PlanStageSlots;

/**
 * This method generates an SBE plan stage tree implementing an index scan. It returns a tuple
 * containing: (1) a slot procued by the index scan that holds the record ID ('recordIdSlot');
 * (2) a slot vector produced by the index scan which hold parts of the index key ('indexKeySlots');
 * and (3) the SBE plan stage tree. 'indexKeySlots' will only contain slots for the parts of the
 * index key specified by the 'indexKeysToInclude' bitset.
 *
 * If the caller provides a slot ID for the 'returnKeySlot' parameter, this method will populate
 * the specified slot with the rehydrated index key for each record.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateIndexScan(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const IndexScanNode* ixn,
    PlanStageReqs reqs,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::SpoolIdGenerator* spoolIdGenerator,
    PlanYieldPolicy* yieldPolicy);

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
    const CollectionPtr& collection,
    const std::string& indexName,
    bool forward,
    std::unique_ptr<KeyString::Value> lowKey,
    std::unique_ptr<KeyString::Value> highKey,
    sbe::IndexKeysInclusionSet indexKeysToInclude,
    sbe::value::SlotVector vars,
    boost::optional<sbe::value::SlotId> recordSlot,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    PlanYieldPolicy* yieldPolicy,
    PlanNodeId nodeId);

}  // namespace mongo::stage_builder
