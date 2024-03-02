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

using AccumulatorArgs = std::pair<SbExpr::Vector, std::vector<std::string>>;

namespace AccArgs {
extern const StringData kCount;
extern const StringData kCovarianceX;
extern const StringData kCovarianceY;
extern const StringData kDefaultVal;
extern const StringData kInput;
extern const StringData kInputFirst;
extern const StringData kInputLast;
extern const StringData kIsAscending;
extern const StringData kIsGroupAccum;
extern const StringData kMaxSize;
extern const StringData kSortBy;
extern const StringData kSortByFirst;
extern const StringData kSortByLast;
extern const StringData kSortSpec;
extern const StringData kUnit;
extern const StringData kValue;

extern const std::vector<std::string> kAccumulatorSingleParam;
extern const std::vector<std::string> kAccumulatorAvgParams;
extern const std::vector<std::string> kAccumulatorCovarianceParams;
extern const std::vector<std::string> kAccumulatorDenseRankParams;
extern const std::vector<std::string> kAccumulatorIntegralParams;
extern const std::vector<std::string> kAccumulatorLinearFillParams;
extern const std::vector<std::string> kAccumulatorRankParams;
extern const std::vector<std::string> kAccumulatorTopBottomNParams;
}  // namespace AccArgs

class AccumulationOp {
public:
    AccumulationOp(std::string opName) : _opName(std::move(opName)) {}

    AccumulationOp(StringData opName) : _opName(opName.toString()) {}

    AccumulationOp(const AccumulationStatement& acc);

    const std::string& getOpName() const {
        return _opName;
    }

    bool countAddendIsIntegerOrDouble() const {
        return _countAddendIsIntegerOrDouble;
    }

private:
    std::string _opName;
    bool _countAddendIsIntegerOrDouble = false;
};

struct BlockAggAndRowAgg {
    SbExpr blockAgg;
    SbExpr rowAgg;
};

/**
 * Given an AccumulationOp ('acc') and one or more input expressions ('input' / 'inputs'), these
 * functions generate the arg expressions needed for the op specified by 'acc'.
 */
AccumulatorArgs buildAccumulatorArgs(StageBuilderState& state,
                                     const AccumulationOp& acc,
                                     SbExpr input,
                                     boost::optional<sbe::value::SlotId> collatorSlot);

AccumulatorArgs buildAccumulatorArgs(StageBuilderState& state,
                                     const AccumulationOp& acc,
                                     StringDataMap<SbExpr> inputs,
                                     boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Given an AccumulationOp ('acc') and vector of named arg expressions ('args' / 'argNames'),
 * this function generates the accumulate expressions for 'acc'.
 */
SbExpr::Vector buildAccumulator(const AccumulationOp& acc,
                                SbExpr::Vector args,
                                const std::vector<std::string>& argNames,
                                boost::optional<sbe::value::SlotId> collatorSlot,
                                StageBuilderState& state);

/**
 * Given an AccumulationOp ('acc') and vector of named arg expressions ('args' / 'argNames'),
 * this function generates the "block" versions of the accumulate expressions for 'acc'.
 */
std::vector<BlockAggAndRowAgg> buildBlockAccumulator(
    const AccumulationOp& acc,
    SbExpr::Vector args,
    const std::vector<std::string>& argNames,
    SbSlot bitmapInternalSlot,
    SbSlot accInternalSlot,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState& state);

/**
 * Given an AccumulationOp 'acc' and a single input expression ('input'), these functions
 * generate the accumulate expressions for 'acc'.
 */
SbExpr::Vector buildAccumulator(const AccumulationOp& acc,
                                SbExpr input,
                                boost::optional<sbe::value::SlotId> collatorSlot,
                                StageBuilderState& state);

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    const AccumulationOp& acc,
    std::unique_ptr<sbe::EExpression> input,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState& state);

/**
 * Given an AccumulationOp 'acc' and a set of input expressions ('inputs'), these functions
 * generate the accumulate expressions for 'acc'.
 */
SbExpr::Vector buildAccumulator(const AccumulationOp& acc,
                                StringDataMap<SbExpr> inputs,
                                boost::optional<sbe::value::SlotId> collatorSlot,
                                StageBuilderState& state);

std::vector<std::unique_ptr<sbe::EExpression>> buildAccumulator(
    const AccumulationOp& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> inputs,
    boost::optional<sbe::value::SlotId> collatorSlot,
    StageBuilderState& state);

/**
 * When SBE hash aggregation spills to disk, it spills partial aggregates which need to be combined
 * later. This function returns the expressions that can be used to combine partial aggregates for
 * the given accumulator 'acc'. The aggregate-of-aggregates will be stored in a slots owned by the
 * hash agg stage, while the new partial aggregates to combine can be read from the given
 * 'inputSlots'.
 */
SbExpr::Vector buildCombinePartialAggregates(const AccumulationOp& acc,
                                             const SbSlotVector& inputSlots,
                                             boost::optional<sbe::value::SlotId> collatorSlot,
                                             StageBuilderState&);

/**
 * Similar to above but takes multiple arguments.
 */
SbExpr::Vector buildCombinePartialAggregates(const AccumulationOp& acc,
                                             const SbSlotVector& inputSlots,
                                             StringDataMap<SbExpr> argExprs,
                                             boost::optional<sbe::value::SlotId> collatorSlot,
                                             StageBuilderState&);

/**
 * Translates an input AccumulationOp into an SBE EExpression that represents an
 * AccumulationOp's finalization step. The 'stage' parameter provides the input subtree to
 * build on top of.
 */
SbExpr buildFinalize(StageBuilderState& state,
                     const AccumulationOp& acc,
                     const SbSlotVector& aggSlots,
                     boost::optional<sbe::value::SlotId> collatorSlot);

SbExpr buildFinalize(StageBuilderState& state,
                     const AccumulationOp& acc,
                     const sbe::value::SlotVector& aggSlots,
                     boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Similar to above but takes multiple arguments.
 */
SbExpr buildFinalize(StageBuilderState& state,
                     const AccumulationOp& acc,
                     const SbSlotVector& aggSlots,
                     StringDataMap<SbExpr> argExprs,
                     boost::optional<sbe::value::SlotId> collatorSlot);

SbExpr buildFinalize(StageBuilderState& state,
                     const AccumulationOp& acc,
                     const sbe::value::SlotVector& aggSlots,
                     StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
                     boost::optional<sbe::value::SlotId> collatorSlot);

/**
 * Translates an input AccumulationOp into an SBE EExpression for the initialization of the
 * accumulator state.
 */
SbExpr::Vector buildInitialize(const AccumulationOp& acc, SbExpr initExpr, StageBuilderState&);

std::vector<std::unique_ptr<sbe::EExpression>> buildInitialize(
    const AccumulationOp& acc, std::unique_ptr<sbe::EExpression> initExpr, StageBuilderState&);

/**
 * Similar to above but takes multiple arguments
 */
SbExpr::Vector buildInitialize(const AccumulationOp& acc,
                               StringDataMap<SbExpr> argExprs,
                               StageBuilderState&);

std::vector<std::unique_ptr<sbe::EExpression>> buildInitialize(
    const AccumulationOp& acc,
    StringDataMap<std::unique_ptr<sbe::EExpression>> argExprs,
    StageBuilderState&);

}  // namespace mongo::stage_builder
