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
#include "mongo/db/matcher/expression.h"

namespace mongo::stage_builder {
/**
 * This function generates an SBE plan stage tree implementing a filter expression represented by
 * 'root'. The 'stage' parameter provides the input subtree to build on top of. The 'inputSlotIn'
 * parameter specifies the input slot the filter should use. The 'relevantSlotsIn' parameter
 * specifies the slots produced by the 'stage' subtree that must remain visible to consumers of
 * the tree returned by this function.
 * Optional slot returned by this function stores index of array element that matches the 'root'
 * match expression. The role of this slot is to be a replacement of 'MatchDetails::elemMatchKey()'.
 * If 'trackIndex' is true and 'root' contains match expression with array semantics (there are
 * certain predicates that do not, such as '{}'), valid slot id is returned. This slot is pointing
 * to an optional value of type int32. Otherwise, 'boost::none' is returned.
 * If match expression found matching array element, value behind slot id is an int32 array index.
 * Otherwise, it is Nothing.
 */
std::pair<boost::optional<sbe::value::SlotId>, std::unique_ptr<sbe::PlanStage>> generateFilter(
    OperationContext* opCtx,
    const MatchExpression* root,
    std::unique_ptr<sbe::PlanStage> stage,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    sbe::value::FrameIdGenerator* frameIdGenerator,
    sbe::value::SlotId inputSlotIn,
    sbe::RuntimeEnvironment* env,
    sbe::value::SlotVector relevantSlotsIn,
    PlanNodeId planNodeId,
    bool trackIndex = false);

/**
 * Similar to 'generateFilter' but used to generate a PlanStage sub-tree implementing a filter
 * attached to an 'IndexScan' QSN. It differs from 'generateFilter' in the following way:
 *  - Instead of a single input slot it takes 'keyFields' and 'keySlots' vectors representing a
 *    subset of the fields of the index key pattern that are depended on to evaluate the predicate,
 *    and corresponding slots for each of the fields.
 *  - It cannot track and returned an index of a matching element within an array, because index
 *    keys cannot contain an array. As such, this function doesn't take a 'trackIndex' parameter
 *    and doesn't return an optional SLotId holding the index of a matching array element.
 */
std::unique_ptr<sbe::PlanStage> generateIndexFilter(OperationContext* opCtx,
                                                    const MatchExpression* root,
                                                    std::unique_ptr<sbe::PlanStage> stage,
                                                    sbe::value::SlotIdGenerator* slotIdGenerator,
                                                    sbe::value::FrameIdGenerator* frameIdGenerator,
                                                    sbe::value::SlotVector keySlots,
                                                    std::vector<std::string> keyFields,
                                                    sbe::RuntimeEnvironment* env,
                                                    sbe::value::SlotVector relevantSlots,
                                                    PlanNodeId planNodeId);
}  // namespace mongo::stage_builder
