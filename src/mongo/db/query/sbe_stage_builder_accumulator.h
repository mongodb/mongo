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

#include <boost/optional/optional.hpp>
#include <memory>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/accumulation_statement.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/util/string_map.h"

namespace mongo::stage_builder {
class PlanStageSlots;

namespace AccArgs {
const StringData kTopBottomNSortSpec = "sortSpec"_sd;
const StringData kTopBottomNKey = "key"_sd;
const StringData kTopBottomNValue = "value"_sd;

const StringData kMaxSize = "maxSize"_sd;
const StringData kIsGroupAccum = "isGroupAccum"_sd;

const StringData kCovarianceX = "x"_sd;
const StringData kCovarianceY = "y"_sd;

const StringData kUnit = "unit"_sd;
const StringData kInput = "input"_sd;
const StringData kSortBy = "sortBy"_sd;

const StringData kDerivativeInputFirst = "inputFirst"_sd;
const StringData kDerivativeInputLast = "inputLast"_sd;
const StringData kDerivativeSortByFirst = "sortByFirst"_sd;
const StringData kDerivativeSortByLast = "sortByLast"_sd;

const StringData kRankIsAscending = "isAscending"_sd;

const StringData kDefaultVal = "defaultVal"_sd;
}  // namespace AccArgs

/**
 * Translates an input AccumulationStatement into an SBE EExpression for accumulation expressions.
 */
SbExpr::Vector buildAccumulator(const AccumulationStatement& acc,
                                SbExpr argExpr,
                                boost::optional<sbe::value::SlotId> collatorSlot,
                                StageBuilderState&);

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    const AccumulationStatement& acc,
    std::unique_ptr<sbe::EExpression> argExpr,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState&);

/**
 * Similar to above but takes multiple arguments.
 */
SbExpr::Vector buildAccumulator(const AccumulationStatement& acc,
                                StringDataMap<SbExpr> argExprs,
                                boost::optional<sbe::value::SlotId> collatorSlot,
                                StageBuilderState&);

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    const AccumulationStatement& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState&);

/**
 * When SBE hash aggregation spills to disk, it spills partial aggregates which need to be combined
 * later. This function returns the expressions that can be used to combine partial aggregates for
 * the given accumulator 'acc'. The aggregate-of-aggregates will be stored in a slots owned by the
 * hash agg stage, while the new partial aggregates to combine can be read from the given
 * 'inputSlots'.
 */
SbExpr::Vector buildCombinePartialAggregates(const AccumulationStatement& acc,
                                             const sbe::value::SlotVector& inputSlots,
                                             boost::optional<sbe::value::SlotId> collatorSlot,
                                             StageBuilderState&);

/**
 * Similar to above but takes multiple arguments.
 */
SbExpr::Vector buildCombinePartialAggregates(const AccumulationStatement& acc,
                                             const sbe::value::SlotVector& inputSlots,
                                             StringDataMap<SbExpr> argExprs,
                                             boost::optional<sbe::value::SlotId> collatorSlot,
                                             StageBuilderState&);

/**
 * Translates an input AccumulationStatement into an SBE EExpression that represents an
 * AccumulationStatement's finalization step. The 'stage' parameter provides the input subtree to
 * build on top of.
 */
SbExpr buildFinalize(StageBuilderState& state,
                     const AccumulationStatement& acc,
                     const sbe::value::SlotVector& aggSlots,
                     boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Similar to above but takes multiple arguments.
 */
SbExpr buildFinalize(StageBuilderState& state,
                     const AccumulationStatement& acc,
                     const sbe::value::SlotVector& aggSlots,
                     StringDataMap<SbExpr> argExprs,
                     boost::optional<sbe::value::SlotId> collatorSlot);

std::unique_ptr<sbe::EExpression> buildFinalize(
    StageBuilderState& state,
    const AccumulationStatement& acc,
    const sbe::value::SlotVector& aggSlots,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Translates an input AccumulationStatement into an SBE EExpression for the initialization of the
 * accumulator state.
 */
SbExpr::Vector buildInitialize(const AccumulationStatement& acc,
                               SbExpr initExpr,
                               StageBuilderState&);

std::vector<std::unique_ptr<sbe::EExpression>> buildInitialize(
    const AccumulationStatement& acc,
    std::unique_ptr<sbe::EExpression> initExpr,
    StageBuilderState&);

/**
 * Similar to above but takes multiple arguments
 */
SbExpr::Vector buildInitialize(const AccumulationStatement& acc,
                               StringDataMap<SbExpr> argExprs,
                               StageBuilderState&);

std::vector<std::unique_ptr<sbe::EExpression>> buildInitialize(
    const AccumulationStatement& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    StageBuilderState&);

}  // namespace mongo::stage_builder
