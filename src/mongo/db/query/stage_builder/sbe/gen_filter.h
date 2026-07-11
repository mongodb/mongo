// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/util/modules.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {
class PlanStageSlots;

/**
 * This function generates an SbExpr that implements the filter expression represented by 'root'.
 * The 'inputSlot' and 'slots' parameters specify the input(s) that the filter should use.
 *
 * The 'isFilterOverIxscan' parameter controls if we should search for kField slots in 'slots' that
 * correspond to the full paths needed by the filter. Typically 'isFilterOverIxscan' is false unless
 * we are generating a filter over an index scan.
 *
 * The 'canUsePathArrayness' parameter indicates that the filter runs over raw documents from the
 * ExpressionContext's main namespace, so PathArrayness info for that namespace may be used to
 * elide traverseF. Only the fetch residual filter sets this to true; filters that run on
 * computed/intermediate documents (e.g. non-leading $match after $group/$addFields) must leave
 * it false.
 *
 * This function returns an SbExpr. If 'root' is an AND with no children, this function will
 * return a null SbExpr to indicate that there is no filter condition.
 */
SbExpr generateFilter(StageBuilderState& state,
                      const MatchExpression* root,
                      boost::optional<SbSlot> inputSlot,
                      const PlanStageSlots& slots,
                      bool isFilterOverIxscan = false,
                      bool canUsePathArrayness = false);

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
SbExpr generateComparisonExpr(StageBuilderState& state,
                              const ComparisonMatchExpression* expr,
                              abt::Operations binaryOp,
                              SbExpr inputExpr);
SbExpr generateInExpr(StageBuilderState& state, const InMatchExpression* expr, SbExpr inputExpr);
SbExpr generateBitTestExpr(StageBuilderState& state,
                           const BitTestMatchExpression* expr,
                           const sbe::BitTestBehavior& bitOp,
                           SbExpr inputExpr);
SbExpr generateModExpr(StageBuilderState& state, const ModMatchExpression* expr, SbExpr inputExpr);
SbExpr generateRegexExpr(StageBuilderState& state,
                         const RegexMatchExpression* expr,
                         SbExpr inputExpr);
SbExpr generateWhereExpr(StageBuilderState& state,
                         const WhereMatchExpression* expr,
                         SbExpr inputExpr);
}  // namespace mongo::stage_builder
