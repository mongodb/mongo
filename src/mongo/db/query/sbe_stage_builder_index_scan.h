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
#include "mongo/db/exec/sbe/stages/collection_helpers.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

namespace mongo::stage_builder {

class PlanStageReqs;
class PlanStageSlots;

/**
 * A list of low and high key values representing ranges over a particular index.
 */
using IndexIntervals =
    std::vector<std::pair<std::unique_ptr<KeyString::Value>, std::unique_ptr<KeyString::Value>>>;

/**
 * This method generates an SBE plan stage tree implementing an index scan. It returns a tuple
 * containing: (1) a slot produced by the index scan that holds the record ID ('recordIdSlot');
 * (2) a slot vector produced by the index scan which hold parts of the index key ('indexKeySlots');
 * and (3) the SBE plan stage tree. 'indexKeySlots' will only contain slots for the parts of the
 * index key specified by the 'indexKeysToInclude' bitset.
 *
 * If the caller provides a slot ID for the 'returnKeySlot' parameter, this method will populate
 * the specified slot with the rehydrated index key for each record.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateIndexScan(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const IndexScanNode* ixn,
    const sbe::IndexKeysInclusionSet& indexKeyBitset,
    PlanYieldPolicy* yieldPolicy,
    StringMap<const IndexAccessMethod*>* iamMap,
    bool needsCorruptionCheck);

/**
 * Constructs low/high key values from the given index 'bounds' if they can be represented either as
 * a single interval between the low and high keys, or multiple single intervals. If index bounds
 * for some interval cannot be expressed as valid low/high keys, then an empty vector is returned.
 */
IndexIntervals makeIntervalsFromIndexBounds(const IndexBounds& bounds,
                                            bool forward,
                                            KeyString::Version version,
                                            Ordering ordering);

/**
 * Construct an array containing objects with the low and high keys for each interval.
 *
 * E.g., [ {l: KS(...), h: KS(...)},
 *         {l: KS(...), h: KS(...)}, ... ]
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> packIndexIntervalsInSbeArray(
    IndexIntervals intervals);

/**
 * Constructs a generic multi-interval index scan. Depending on the intervals will either execute
 * the optimized or the generic index scan subplan. The generated subtree will have
 * the following form:
 *
 * branch {isGenericScanSlot} [recordIdSlot, resultSlot, ...]
 * then
 *    filter {isRecordId(resultSlot)}
 *    lspool sp1 [resultSlot] {!isRecordId(resultSlot)}
 *    union [resultSlot]
             project [startKeySlot = anchorSlot, unusedVarSlot0 = Nothing, ...]
 *           limit 1
 *           coscan
 *       [checkBoundsSlot]
 *           nlj [] [seekKeySlot]
 *               left
 *                   sspool sp1 [seekKeySlot]
 *               right
 *                  chkbounds resultSlot recordIdSlot checkBoundsSlot
 *                  nlj [] [lowKeySlot]
 *                      left
 *                          project [lowKeySlot = seekKeySlot]
 *                          limit 1
 *                          coscan
 *                   right
 *                      ixseek lowKeySlot resultSlot recordIdSlot [] @coll @index
 * else
 *     nlj [] [lowKeySlot, highKeySlot]
 *     left
 *         project [lowKeySlot = getField (unwindSlot, "l"),
 *                  highKeySlot = getField (unwindSlot, "h")]
 *         unwind unwindSlot indexSlot boundsSlot false
 *         limit 1
 *         coscan
 *     right
 *         ixseek lowKeySlot highKeySlot recordIdSlot [] @coll @index
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateIndexScanWithDynamicBounds(
    StageBuilderState& state,
    const CollectionPtr& collection,
    const IndexScanNode* ixn,
    const sbe::IndexKeysInclusionSet& indexKeyBitset,
    PlanYieldPolicy* yieldPolicy,
    StringMap<const IndexAccessMethod*>* iamMap,
    bool needsCorruptionCheck);
}  // namespace mongo::stage_builder
