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
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/sbe_stage_builder_eval_frame.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"

namespace mongo::stage_builder {
class PlanStageSlots;

/**
 * This function generates an SBE plan stage tree implementing a filter expression represented by
 * 'root'. The 'stage' parameter provides the input subtree to build on top of. The 'inputSlot'
 * and 'slots' parameters specify the input(s) that the filter should use.
 *
 * The 'useKeySlots' parameter controls whether kKey slots in 'slots' are allowed to be used as a
 * source of input. Typically 'useKeySlots' is false unless we are generating a filter over an
 * index scan.
 *
 * If the caller sets 'useKeySlots' to true, then the caller must also provide a vector of key field
 * names ('keyFields') that lists the names of all the kKey slots that are needed by 'root'.
 *
 * This function returns a pair containing an optional<SlotId> and an EvalStage. If 'trackIndex' is
 * false then the optional<SlotId> will always be boost::none. If 'trackIndex' is true, then this
 * optional<SlotId> will be set to a slot that holds information about the index of the matching
 * element if an array traversal was performed. (If multiple array traversals were performed, it is
 * undefined which traversal will report the index of the matching array element.)
 *
 * Note that this function does not allow both 'useKeySlots' and 'trackIndex' to be true. If they
 * are both true, then this function will throw an exception.
 */
std::pair<boost::optional<sbe::value::SlotId>, EvalStage> generateFilter(
    StageBuilderState& state,
    const MatchExpression* root,
    EvalStage stage,
    boost::optional<sbe::value::SlotId> inputSlot,
    const PlanStageSlots* slots,
    PlanNodeId planNodeId,
    const std::vector<std::string>& keyFields = {},
    bool useKeySlots = false,
    bool trackIndex = false);

/**
 * Converts the list of equalities inside the given $in expression ('expr') into an SBE array, which
 * is returned as a (typeTag, value) pair. The caller owns the resulting value.
 *
 * The returned tuple also includes three booleans, in this order:
 *  - 'hasArray': True if at least one of the values inside the $in equality list is an array.
 *  - 'hasObject': True if at least one of the values inside the $in equality list is an object.
 *  - 'hasNull': True if at least one of the values inside the $in equality list is a literal null
 * value.
 */
std::tuple<sbe::value::TypeTags, sbe::value::Value, bool, bool, bool> convertInExpressionEqualities(
    const InMatchExpression* expr);

/**
 * Converts the list of bit positions inside of any of the bit-test match expressions
 * ($bitsAllClear, $bitsAllSet, $bitsAnyClear, and $bitsAnySet) to an SBE array, returned as a
 * (typeTag, value) pair. The caller owns the resulting value.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> convertBitTestBitPositions(
    const BitTestMatchExpression* expr);

/**
 * The following family of functions convert the given MatchExpression that consumes a single input
 * into an EExpression that consumes the input from the provided slot.
 */
EvalExpr generateComparisonExpr(StageBuilderState& state,
                                const ComparisonMatchExpression* expr,
                                sbe::EPrimBinary::Op binaryOp,
                                const sbe::EVariable& var);
EvalExpr generateInExpr(StageBuilderState& state,
                        const InMatchExpression* expr,
                        const sbe::EVariable& var);
EvalExpr generateBitTestExpr(StageBuilderState& state,
                             const BitTestMatchExpression* expr,
                             const sbe::BitTestBehavior& bitOp,
                             const sbe::EVariable& var);
EvalExpr generateModExpr(StageBuilderState& state,
                         const ModMatchExpression* expr,
                         const sbe::EVariable& var);
EvalExpr generateRegexExpr(StageBuilderState& state,
                           const RegexMatchExpression* expr,
                           const sbe::EVariable& var);
EvalExpr generateWhereExpr(StageBuilderState& state,
                           const WhereMatchExpression* expr,
                           const sbe::EVariable& var);
}  // namespace mongo::stage_builder
