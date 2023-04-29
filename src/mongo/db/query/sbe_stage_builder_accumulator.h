/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

namespace mongo::stage_builder {
class PlanStageSlots;

namespace AccArgs {
const StringData kTopBottomNSortSpec = "sortSpec"_sd;
const StringData kTopBottomNKey = "key"_sd;
const StringData kTopBottomNValue = "value"_sd;
}  // namespace AccArgs

/**
 * Translates an input AccumulationStatement into an SBE EExpression for accumulation expressions.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    const AccumulationStatement& acc,
    std::unique_ptr<sbe::EExpression> argExpr,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator&);

/**
 * Similar to above but takes multiple arguments.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    const AccumulationStatement& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator&);

/**
 * When SBE hash aggregation spills to disk, it spills partial aggregates which need to be combined
 * later. This function returns the expressions that can be used to combine partial aggregates for
 * the given accumulator 'acc'. The aggregate-of-aggregates will be stored in a slots owned by the
 * hash agg stage, while the new partial aggregates to combine can be read from the given
 * 'inputSlots'.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggregates(
    const AccumulationStatement& acc,
    const sbe::value::SlotVector& inputSlots,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator&);

/**
 * Similar to above but takes multiple arguments.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildCombinePartialAggregates(
    const AccumulationStatement& acc,
    const sbe::value::SlotVector& inputSlots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator&);

/**
 * Translates an input AccumulationStatement into an SBE EExpression that represents an
 * AccumulationStatement's finalization step. The 'stage' parameter provides the input subtree to
 * build on top of.
 */
std::unique_ptr<sbe::EExpression> buildFinalize(StageBuilderState& state,
                                                const AccumulationStatement& acc,
                                                const sbe::value::SlotVector& aggSlots,
                                                boost::optional<sbe::value::SlotId> collatorSlot,
                                                sbe::value::FrameIdGenerator& frameIdGenerator);

/**
 * Similar to above but takes multiple arguments.
 */
std::unique_ptr<sbe::EExpression> buildFinalize(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    const sbe::value::SlotVector& aggSlots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    sbe::value::FrameIdGenerator& frameIdGenerator);

/**
 * Translates an input AccumulationStatement into an SBE EExpression for the initialization of the
 * accumulator state.
 */
std::vector<std::unique_ptr<sbe::EExpression>> buildInitialize(
    const AccumulationStatement& acc,
    std::unique_ptr<sbe::EExpression> initExpr,
    sbe::value::FrameIdGenerator&);
}  // namespace mongo::stage_builder
