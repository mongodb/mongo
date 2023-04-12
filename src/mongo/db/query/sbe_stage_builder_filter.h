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
 * This function generates an EvalExpr that implements the filter expression represented by 'root'.
 * The 'inputSlot' and 'slots' parameters specify the input(s) that the filter should use.
 *
 * The 'isFilterOverIxscan' parameter controls if we should search for kField slots in 'slots' that
 * correspond to the full paths needed by the filter. Typically 'isFilterOverIxscan' is false unless
 * we are generating a filter over an index scan.
 *
 * If the caller sets 'isFilterOverIxscan' to true, then the caller must also provide a vector of
 * key field names ('keyFields') that lists the names of all the kField slots that are needed by
 * 'root'.
 *
 * This function returns an EvalExpr. If 'root' is an AND with no children, this function will
 * return a null EvalExpr to indicate that there is no filter condition.
 */
EvalExpr generateFilter(StageBuilderState& state,
                        const MatchExpression* root,
                        boost::optional<sbe::value::SlotId> inputSlot,
                        const PlanStageSlots* slots,
                        const std::vector<std::string>& keyFields = {},
                        bool isFilterOverIxscan = false);

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
    const InMatchExpression* expr, const PlanStageData& data);

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
                                EvalExpr inputExpr);
EvalExpr generateInExpr(StageBuilderState& state,
                        const InMatchExpression* expr,
                        EvalExpr inputExpr);
EvalExpr generateBitTestExpr(StageBuilderState& state,
                             const BitTestMatchExpression* expr,
                             const sbe::BitTestBehavior& bitOp,
                             EvalExpr inputExpr);
EvalExpr generateModExpr(StageBuilderState& state,
                         const ModMatchExpression* expr,
                         EvalExpr inputExpr);
EvalExpr generateRegexExpr(StageBuilderState& state,
                           const RegexMatchExpression* expr,
                           EvalExpr inputExpr);
EvalExpr generateWhereExpr(StageBuilderState& state,
                           const WhereMatchExpression* expr,
                           EvalExpr inputExpr);
}  // namespace mongo::stage_builder
